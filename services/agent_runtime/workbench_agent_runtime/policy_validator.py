"""面向类型化语义动作的失败即拒绝授权策略。

``ToolRegistry`` 是唯一的动作与参数字段白名单。本模块消费其校验结果，
然后应用两条相互独立的策略规则：即使在注册表接受的嵌套载荷中，原始
控制标识符也一律禁止；已配置的高影响动作必须针对确切的 ``action_id``
完成确认。

输出是不可变的授权证据。它绝不派发动作、获取或存储确认、产出执行或
验证结果、写入 WorldState，也绝不宣告完成。
"""

from __future__ import annotations

import json
import re
from collections.abc import Mapping
from dataclasses import dataclass
from enum import StrEnum
from types import MappingProxyType

from workbench_contracts import ActionType, TaskGraph, TaskStep

from .tool_registry import ToolRegistry

_POLICY_CONFIG_KEYS = frozenset({"policy_version", "high_impact_actions"})
_RAW_CONTROL_TOKENS = frozenset({"joint", "velocity", "torque", "firmware"})
_PARAMETER_IDENTIFIER = re.compile(r"^[A-Za-z_][A-Za-z0-9_.\-\[\]]*$")
_SERIALIZED_KEY = re.compile(r"(?<![A-Za-z0-9_.\-\]])([A-Za-z_][A-Za-z0-9_.\-\[\]]*)(?:(?:\s*=\s*)|:)(?=\S)")
_CAMEL_BOUNDARY = re.compile(r"(?<=[a-z0-9])(?=[A-Z])")
_MAX_SCAN_DEPTH = 64
_MAX_SCAN_NODES = 10_000
_CONFIG_SNAPSHOT_FAILED = object()


class PolicyOutcome(StrEnum):
    """单个 TaskGraph 步骤的授权结果。"""

    ALLOW = "allow"
    DENY = "deny"
    CONFIRMATION_REQUIRED = "confirmation_required"


class PolicyReasonCode(StrEnum):
    """策略结果的稳定、机器可读原因。"""

    POLICY_ALLOWED = "policy_allowed"
    POLICY_CONFIG_MISSING = "policy_config_missing"
    POLICY_VERSION_BLANK = "policy_version_blank"
    POLICY_CONFIG_UNKNOWN_ACTION = "policy_config_unknown_action"
    POLICY_CONFIG_MALFORMED = "policy_config_malformed"
    CONFIRMATION_INPUT_MALFORMED = "confirmation_input_malformed"
    TOOL_REGISTRY_DENIED = "tool_registry_denied"
    RAW_CONTROL_PARAMETER = "raw_control_parameter"
    POLICY_INPUT_MALFORMED = "policy_input_malformed"
    ACTION_CONFIRMATION_REQUIRED = "action_confirmation_required"


@dataclass(frozen=True)
class PolicyFinding:
    """一条策略详情，定位到具体的步骤与字段。"""

    step_id: str
    action_id: str
    field: str
    message: str


@dataclass(frozen=True)
class PolicyDecision:
    """恰好针对一个 TaskGraph 步骤的不可变授权决策。"""

    step_id: str
    action_id: str
    outcome: PolicyOutcome
    reason_code: PolicyReasonCode
    policy_version: str | None
    findings: tuple[PolicyFinding, ...] = ()


@dataclass(frozen=True)
class PolicyReport:
    """每步骤授权决策的不可变集合。"""

    decisions: tuple[PolicyDecision, ...]

    @property
    def findings(self) -> tuple[PolicyFinding, ...]:
        """拒绝决策详情的扁平化兼容视图。"""
        return tuple(finding for decision in self.decisions for finding in decision.findings)

    @property
    def is_valid(self) -> bool:
        return all(decision.outcome is PolicyOutcome.ALLOW for decision in self.decisions)


class PolicyViolation(RuntimeError):
    """当 TaskGraph 包含非允许的策略决策时抛出。"""

    def __init__(self, report: PolicyReport) -> None:
        self.report = report
        super().__init__(_format_decisions(report.decisions))


@dataclass(frozen=True)
class _PolicyConfigState:
    policy_version: str | None
    high_impact_actions: frozenset[ActionType]
    error_code: PolicyReasonCode | None = None
    error_field: str = ""
    error_message: str = ""


class _PolicyInputMalformed(ValueError):
    """内部信号，将转换为结构化的失败即拒绝决策。"""

    def __init__(self, field: str, message: str) -> None:
        self.field = field
        self.message = message
        super().__init__(message)


class PolicyValidator:
    """依据显式策略与注入的注册表对 TaskGraph 进行授权。

    ``policy_config`` 必须是恰好包含两个键的内存映射：``policy_version``
    是非空字符串，``high_impact_actions`` 是已注册于注入注册表的
    ActionType 值的 ``frozenset``。

    确认信息以已确认动作 ID 集合的形式按调用提供。它只用于当次检查，
    此处从不获取或持久化它。
    """

    def __init__(self, registry: ToolRegistry | None = None, *, policy_config: object = None) -> None:
        self._registry = registry if registry is not None else ToolRegistry()
        self._policy_config = _snapshot_policy_config(policy_config)

    def check(
        self,
        graph: TaskGraph,
        *,
        confirmed_action_ids: frozenset[str] = frozenset(),
    ) -> PolicyReport:
        """为每个步骤返回一个不可变的结构化授权决策。"""
        config = _validate_policy_config(self._policy_config, self._registry)
        if config.error_code is not None:
            return PolicyReport(tuple(_config_error_decision(step, config) for step in graph.steps))

        confirmed_ids = _snapshot_confirmed_action_ids(confirmed_action_ids)
        if confirmed_ids is None:
            return PolicyReport(tuple(_malformed_confirmation_decision(step, config) for step in graph.steps))

        decisions = tuple(self._check_step(step, config, confirmed_ids) for step in graph.steps)
        return PolicyReport(decisions)

    def enforce(
        self,
        graph: TaskGraph,
        *,
        confirmed_action_ids: frozenset[str] = frozenset(),
    ) -> None:
        """对拒绝与需确认的结果抛出 PolicyViolation。"""
        report = self.check(graph, confirmed_action_ids=confirmed_action_ids)
        if not report.is_valid:
            raise PolicyViolation(report)

    def _check_step(
        self,
        step: TaskStep,
        config: _PolicyConfigState,
        confirmed_action_ids: frozenset[str],
    ) -> PolicyDecision:
        action = step.action
        registry_result = self._registry.validate(action)
        if not registry_result.is_valid:
            findings = tuple(
                PolicyFinding(step.step_id, action.action_id, error.field, error.message)
                for error in registry_result.errors
            )
            return PolicyDecision(
                step_id=step.step_id,
                action_id=action.action_id,
                outcome=PolicyOutcome.DENY,
                reason_code=PolicyReasonCode.TOOL_REGISTRY_DENIED,
                policy_version=config.policy_version,
                findings=findings,
            )

        try:
            raw_identifiers = _find_raw_control_identifiers(action.parameters)
        except _PolicyInputMalformed as error:
            finding = PolicyFinding(
                step.step_id,
                action.action_id,
                error.field,
                error.message,
            )
            return PolicyDecision(
                step_id=step.step_id,
                action_id=action.action_id,
                outcome=PolicyOutcome.DENY,
                reason_code=PolicyReasonCode.POLICY_INPUT_MALFORMED,
                policy_version=config.policy_version,
                findings=(finding,),
            )
        if raw_identifiers:
            findings = tuple(
                PolicyFinding(
                    step.step_id,
                    action.action_id,
                    field,
                    f"raw-control parameter identifier '{identifier}' is forbidden",
                )
                for field, identifier in raw_identifiers
            )
            return PolicyDecision(
                step_id=step.step_id,
                action_id=action.action_id,
                outcome=PolicyOutcome.DENY,
                reason_code=PolicyReasonCode.RAW_CONTROL_PARAMETER,
                policy_version=config.policy_version,
                findings=findings,
            )

        if action.action_type in config.high_impact_actions and action.action_id not in confirmed_action_ids:
            finding = PolicyFinding(
                step.step_id,
                action.action_id,
                "confirmation",
                f"high-impact action '{action.action_id}' requires confirmation for this exact action_id",
            )
            return PolicyDecision(
                step_id=step.step_id,
                action_id=action.action_id,
                outcome=PolicyOutcome.CONFIRMATION_REQUIRED,
                reason_code=PolicyReasonCode.ACTION_CONFIRMATION_REQUIRED,
                policy_version=config.policy_version,
                findings=(finding,),
            )

        return PolicyDecision(
            step_id=step.step_id,
            action_id=action.action_id,
            outcome=PolicyOutcome.ALLOW,
            reason_code=PolicyReasonCode.POLICY_ALLOWED,
            policy_version=config.policy_version,
        )


def _snapshot_confirmed_action_ids(value: object) -> frozenset[str] | None:
    if not isinstance(value, set | frozenset):
        return None
    try:
        snapshot = frozenset(value)
    except Exception:  # noqa: BLE001 - 格式错误的注入确认输入必须失败即拒绝
        return None
    if any(not isinstance(action_id, str) or not action_id.strip() for action_id in snapshot):
        return None
    return snapshot


def _snapshot_policy_config(policy_config: object) -> object:
    if not isinstance(policy_config, Mapping):
        return policy_config
    try:
        return MappingProxyType(dict(policy_config))
    except Exception:  # noqa: BLE001 - 格式错误的注入映射必须在检查期间失败即拒绝
        return _CONFIG_SNAPSHOT_FAILED


def _validate_policy_config(policy_config: object, registry: ToolRegistry) -> _PolicyConfigState:
    if policy_config is None:
        return _config_error(
            PolicyReasonCode.POLICY_CONFIG_MISSING,
            "policy_config",
            "explicit policy configuration is required",
        )
    if policy_config is _CONFIG_SNAPSHOT_FAILED or not isinstance(policy_config, Mapping):
        return _config_error(
            PolicyReasonCode.POLICY_CONFIG_MALFORMED,
            "policy_config",
            "policy configuration must be a mapping",
        )

    if set(policy_config) != _POLICY_CONFIG_KEYS:
        return _config_error(
            PolicyReasonCode.POLICY_CONFIG_MALFORMED,
            "policy_config",
            f"policy configuration keys must be exactly {sorted(_POLICY_CONFIG_KEYS)}",
        )

    policy_version = policy_config["policy_version"]
    if not isinstance(policy_version, str):
        return _config_error(
            PolicyReasonCode.POLICY_CONFIG_MALFORMED,
            "policy_config.policy_version",
            "policy_version must be a string",
        )
    if not policy_version.strip():
        return _config_error(
            PolicyReasonCode.POLICY_VERSION_BLANK,
            "policy_config.policy_version",
            "policy_version must be non-blank",
        )

    high_impact_actions = policy_config["high_impact_actions"]
    if not isinstance(high_impact_actions, frozenset):
        return _config_error(
            PolicyReasonCode.POLICY_CONFIG_MALFORMED,
            "policy_config.high_impact_actions",
            "high_impact_actions must be a frozenset",
            policy_version=policy_version,
        )

    registered_actions = frozenset(registry.list_all())
    unknown_actions = tuple(
        action
        for action in high_impact_actions
        if not isinstance(action, ActionType) or action not in registered_actions
    )
    if unknown_actions:
        labels = sorted(action.value if isinstance(action, ActionType) else repr(action) for action in unknown_actions)
        return _config_error(
            PolicyReasonCode.POLICY_CONFIG_UNKNOWN_ACTION,
            "policy_config.high_impact_actions",
            f"high_impact_actions contains unknown actions: {labels}",
            policy_version=policy_version,
        )

    return _PolicyConfigState(
        policy_version=policy_version,
        high_impact_actions=high_impact_actions,
    )


def _config_error(
    reason_code: PolicyReasonCode,
    field: str,
    message: str,
    *,
    policy_version: str | None = None,
) -> _PolicyConfigState:
    return _PolicyConfigState(
        policy_version=policy_version,
        high_impact_actions=frozenset(),
        error_code=reason_code,
        error_field=field,
        error_message=message,
    )


def _config_error_decision(step: TaskStep, config: _PolicyConfigState) -> PolicyDecision:
    finding = PolicyFinding(
        step.step_id,
        step.action.action_id,
        config.error_field,
        config.error_message,
    )
    return PolicyDecision(
        step_id=step.step_id,
        action_id=step.action.action_id,
        outcome=PolicyOutcome.DENY,
        reason_code=config.error_code,
        policy_version=config.policy_version,
        findings=(finding,),
    )


def _malformed_confirmation_decision(
    step: TaskStep,
    config: _PolicyConfigState,
) -> PolicyDecision:
    finding = PolicyFinding(
        step.step_id,
        step.action.action_id,
        "confirmed_action_ids",
        "confirmed_action_ids must be a set or frozenset of non-blank strings",
    )
    return PolicyDecision(
        step_id=step.step_id,
        action_id=step.action.action_id,
        outcome=PolicyOutcome.DENY,
        reason_code=PolicyReasonCode.CONFIRMATION_INPUT_MALFORMED,
        policy_version=config.policy_version,
        findings=(finding,),
    )


def _find_raw_control_identifiers(value: object) -> tuple[tuple[str, str], ...]:
    findings: list[tuple[str, str]] = []
    seen: set[int] = set()
    stack: list[tuple[object, str, int]] = [(value, "parameters", 0)]
    scanned_nodes = 0

    while stack:
        item, path, depth = stack.pop()
        scanned_nodes += 1
        if depth > _MAX_SCAN_DEPTH or scanned_nodes > _MAX_SCAN_NODES:
            raise _PolicyInputMalformed(
                path,
                f"raw-control scan limit exceeded (max_depth={_MAX_SCAN_DEPTH}, max_nodes={_MAX_SCAN_NODES})",
            )

        if isinstance(item, str):
            if _PARAMETER_IDENTIFIER.fullmatch(item) and _contains_raw_control_token(item):
                findings.append((path, item))
            for match in _SERIALIZED_KEY.finditer(item):
                identifier = match.group(1)
                if _contains_raw_control_token(identifier):
                    findings.append((path, identifier))
            decoded = _decode_serialized_container(item, path)
            if decoded is not None:
                stack.append((decoded, path, depth + 1))
            continue

        if isinstance(item, Mapping):
            marker = id(item)
            if marker in seen:
                continue
            seen.add(marker)
            try:
                entries = tuple(item.items())
            except Exception as error:
                raise _PolicyInputMalformed(path, "policy input mapping could not be inspected") from error
            for key, child in reversed(entries):
                if not isinstance(key, str):
                    raise _PolicyInputMalformed(
                        path,
                        "policy input mapping keys must be strings",
                    )
                child_path = f"{path}.{key}" if _PARAMETER_IDENTIFIER.fullmatch(key) else f"{path}[{key!r}]"
                if _contains_raw_control_token(key):
                    findings.append((child_path, key))
                stack.append((child, child_path, depth + 1))
            continue

        if isinstance(item, list | tuple):
            marker = id(item)
            if marker in seen:
                continue
            seen.add(marker)
            for index in range(len(item) - 1, -1, -1):
                stack.append((item[index], f"{path}[{index}]", depth + 1))

    return tuple(findings)


def _decode_serialized_container(value: str, path: str) -> object | None:
    stripped = value.strip()
    if not stripped.startswith(("{", "[")):
        return None
    try:
        decoded = json.loads(stripped)
    except json.JSONDecodeError:
        return None
    except (RecursionError, ValueError) as error:
        raise _PolicyInputMalformed(path, "raw-control scan limit exceeded while decoding JSON") from error
    if isinstance(decoded, Mapping | list | tuple):
        return decoded
    return None


def _contains_raw_control_token(identifier: str) -> bool:
    separated = _CAMEL_BOUNDARY.sub("_", identifier)
    tokens = {token.casefold() for token in re.split(r"[^A-Za-z0-9]+", separated) if token}
    if tokens & _RAW_CONTROL_TOKENS:
        return True
    compact = re.sub(r"[^A-Za-z0-9]+", "", separated).casefold()
    return any(compact.startswith(token) or compact.endswith(token) for token in _RAW_CONTROL_TOKENS)


def _format_decisions(decisions: tuple[PolicyDecision, ...]) -> str:
    lines: list[str] = []
    for decision in decisions:
        if decision.outcome is PolicyOutcome.ALLOW:
            continue
        details = "; ".join(f"{finding.field}: {finding.message}" for finding in decision.findings)
        lines.append(
            f"step '{decision.step_id}' action '{decision.action_id}': "
            f"outcome={decision.outcome.value} reason_code={decision.reason_code.value} "
            f"policy_version={decision.policy_version!r}: {details}"
        )
    return "TaskGraph violates policy:\n" + "\n".join(lines)
