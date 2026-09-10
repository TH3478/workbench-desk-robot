"""带失败即拒绝参数校验的工具注册表。

Agent Runtime 派发的每个 SemanticAction 在执行前都必须经过
ToolRegistry.validate()。注册表是合法参数形态的唯一事实来源；
A5（Policy Validator）读取它以构建白名单。
"""

from __future__ import annotations

from collections.abc import Mapping
from dataclasses import dataclass
from math import isfinite
from types import MappingProxyType

from workbench_contracts import ActionType, SemanticAction

from . import tool_schemas as _schemas

# ---------------------------------------------------------------------------
# 公开类型
# ---------------------------------------------------------------------------

_TARGET_REQUIRED_ACTIONS: frozenset[ActionType] = frozenset({ActionType.GRASP, ActionType.PLACE, ActionType.NAVIGATE})
_SCHEMA_KEYS: frozenset[str] = frozenset(
    {
        "description",
        "target_id_required",
        "required_params",
        "optional_params",
        "param_types",
        "param_constraints",
        "relational_constraints",
    }
)
_SUPPORTED_PARAM_TYPES: frozenset[type] = frozenset({str, int, float, bool, list})
_CONSTRAINT_KEYS: frozenset[str] = frozenset({"non_blank", "finite", "minimum", "maximum"})
_SUPPORTED_RELATIONAL_CONSTRAINTS: frozenset[str] = frozenset({"destination_counts_consistent"})
_DESTINATION_COUNT_PARAMS: frozenset[str] = frozenset(
    {"destination_capacity", "destination_occupancy_after", "destination_remaining_after"}
)


@dataclass(frozen=True)
class ValidationError:
    """单条校验失败。"""

    field: str
    message: str


@dataclass(frozen=True)
class ValidationResult:
    """对照注册表校验单个 SemanticAction 的结果。"""

    is_valid: bool
    action_id: str
    errors: tuple[ValidationError, ...] = ()

    def __bool__(self) -> bool:
        return self.is_valid

    @classmethod
    def ok(cls, action_id: str) -> ValidationResult:
        return cls(is_valid=True, action_id=action_id)

    @classmethod
    def fail(cls, action_id: str, errors: list[ValidationError]) -> ValidationResult:
        return cls(is_valid=False, action_id=action_id, errors=tuple(errors))


# ---------------------------------------------------------------------------
# 注册表实现
# ---------------------------------------------------------------------------


class ToolRegistry:
    """注册并校验七种受限语义动作工具。

    校验层次按顺序依次应用：

    1. **存在性**——action_type 必须已注册。
    2. **目标**——GRASP 与 PLACE 要求非空 ``target_id``。
    3. **字段集合**——parameters 必须包含全部必需键，且不得包含
       必需 + 可选并集之外的键。
    4. **类型安全**——每个参数值必须与其声明类型相符。``bool`` 在
       ``int`` 之前检查，``True`` / ``False`` 因此永远无法静默通过
       整型槽位。
    5. **Schema 约束**——有限的数值范围、非空字符串，以及参数之间
       已声明的关系。
    6. **语义约束**——emotion_state 枚举、观测属性白名单。
    """

    def __init__(self, *, load_defaults: bool = True) -> None:
        self._tool_param_schemas: dict[ActionType, Mapping[str, object]] = {}
        if load_defaults:
            for action_type, schema in _schemas.TOOL_SCHEMAS.items():
                self.register(action_type, schema)

    # -- 公开 API ------------------------------------------------------------

    def register(self, action_type: ActionType, schema: Mapping[str, object]) -> None:
        """注册一个工具 schema。

        Raises ``ValueError``：当 *action_type* 不是 ``ActionType`` 成员时抛出。
        拒绝重复注册已注册过的类型。"""
        if not isinstance(action_type, ActionType):
            raise ValueError(f"action_type must be an ActionType enum member, got {type(action_type).__name__!r}")
        if action_type in self._tool_param_schemas:
            raise ValueError(f"ActionType '{action_type.value}' is already registered")
        if not isinstance(schema, Mapping):
            raise ValueError(f"schema must be a mapping, got {type(schema).__name__!r}")
        frozen_schema = _freeze_mapping(schema)
        _validate_schema(frozen_schema)
        self._tool_param_schemas[action_type] = frozen_schema

    def get(self, action_type: ActionType) -> Mapping[str, object]:
        """返回 *action_type* 的 schema。

        Raises ``ValueError``：当 *action_type* 不是 ``ActionType`` 成员时抛出。
        Raises ``KeyError``：当 *action_type* 未注册时抛出。"""
        if not isinstance(action_type, ActionType):
            raise ValueError(f"action_type must be an ActionType enum member, got {type(action_type).__name__!r}")
        if action_type not in self._tool_param_schemas:
            raise KeyError(action_type)
        return self._tool_param_schemas[action_type]

    def list_all(self) -> tuple[ActionType, ...]:
        """返回所有已注册的 ActionType。"""
        return tuple(self._tool_param_schemas)

    def required_params(self, action_type: ActionType) -> frozenset[str]:
        return self._tool_param_schemas[action_type]["required_params"]

    def optional_params(self, action_type: ActionType) -> frozenset[str]:
        return self._tool_param_schemas[action_type]["optional_params"]

    def allowed_params(self, action_type: ActionType) -> frozenset[str]:
        schema = self._tool_param_schemas[action_type]
        return schema["required_params"] | schema["optional_params"]

    def validate(self, action: SemanticAction) -> ValidationResult:
        """校验 *action* 并返回 ``ValidationResult``。

        本方法**绝不抛出异常**——每次拒绝都编码进结果，调用方即使忘记
        写 ``try`` / ``except`` 也无法意外跳过校验。连非 ActionType 的
        输入也会被捕获，并以校验失败而不是异常的形式返回。
        """
        errors: list[ValidationError] = []

        # ---- 0. 防护——action_type 到底是不是 ActionType？ ----------------
        try:
            action_type_value = action.action_type.value
        except AttributeError:
            return ValidationResult.fail(
                action.action_id,
                [
                    ValidationError(
                        "action_type",
                        f"expected an ActionType enum member, got {type(action.action_type).__name__!r}",
                    )
                ],
            )

        # ---- 1. 存在性 ----------------------------------------------------
        if action.action_type not in self._tool_param_schemas:
            return ValidationResult.fail(
                action.action_id,
                [
                    ValidationError(
                        "action_type",
                        f"unknown action_type '{action_type_value}'; "
                        f"allowed: {sorted(t.value for t in self._tool_param_schemas)}",
                    )
                ],
            )
        schema = self._tool_param_schemas[action.action_type]
        target_id_required: bool = schema.get("target_id_required", False)
        required: frozenset[str] = schema["required_params"]
        optional: frozenset[str] = schema["optional_params"]
        allowed: frozenset[str] = required | optional
        param_types: Mapping[str, type | object] = schema["param_types"]
        raw_params = action.parameters
        if not isinstance(raw_params, Mapping):
            return ValidationResult.fail(
                action.action_id,
                [ValidationError("parameters", f"expected a mapping, got {type(raw_params).__name__!r}")],
            )
        params: Mapping[str, object] = raw_params

        # ---- 2. target_id --------------------------------------------------
        if target_id_required:
            if not isinstance(action.target_id, str) or not action.target_id.strip():
                errors.append(
                    ValidationError(
                        "target_id",
                        f"'{action_type_value}' requires a non-empty target_id",
                    )
                )

        # ---- 3. 字段集合 --------------------------------------------------
        actual_keys = set(params)
        extra = actual_keys - allowed
        missing = required - actual_keys

        if extra:
            errors.append(
                ValidationError(
                    "parameters",
                    f"forbidden keys for '{action_type_value}': {sorted(extra)}; allowed: {sorted(allowed)}",
                )
            )
        if missing:
            errors.append(
                ValidationError(
                    "parameters",
                    f"missing required keys for '{action_type_value}': {sorted(missing)}",
                )
            )
        # 字段集合损坏时提前退出——对结构无效的字典做类型检查会
        # 产生大量嘈杂的次级错误。
        if errors:
            return ValidationResult.fail(action.action_id, errors)

        # ---- 4. 类型安全 --------------------------------------------------
        for key, value in params.items():
            expected_type = param_types.get(key)
            if expected_type is None:
                # 参数没有声明类型——放行
                continue

            if expected_type is bool:
                if not isinstance(value, bool):
                    errors.append(
                        ValidationError(
                            f"parameters.{key}",
                            f"expected bool, got {_type_label(value)}",
                        )
                    )
                continue

            if expected_type is int:
                # 在 Python 中 bool 是 int 的子类——显式拒绝它
                if isinstance(value, bool):
                    errors.append(
                        ValidationError(
                            f"parameters.{key}",
                            "expected int, got bool (bool is not int)",
                        )
                    )
                elif not isinstance(value, int):
                    errors.append(
                        ValidationError(
                            f"parameters.{key}",
                            f"expected int, got {_type_label(value)}",
                        )
                    )
                continue

            if expected_type is float:
                if isinstance(value, bool):
                    errors.append(
                        ValidationError(
                            f"parameters.{key}",
                            "expected float, got bool",
                        )
                    )
                elif not isinstance(value, int | float):
                    errors.append(
                        ValidationError(
                            f"parameters.{key}",
                            f"expected float, got {_type_label(value)}",
                        )
                    )
                continue

            if expected_type is str:
                if not isinstance(value, str):
                    errors.append(
                        ValidationError(
                            f"parameters.{key}",
                            f"expected str, got {_type_label(value)}",
                        )
                    )
                continue

            if expected_type is list:
                if not isinstance(value, list):
                    errors.append(
                        ValidationError(
                            f"parameters.{key}",
                            f"expected list, got {_type_label(value)}",
                        )
                    )
                continue

            # 通用的 isinstance 兜底
            if not isinstance(value, expected_type):
                errors.append(
                    ValidationError(
                        f"parameters.{key}",
                        f"expected {_type_name(expected_type)}, got {_type_label(value)}",
                    )
                )

        # 不要比较或检查声明类型不符的值。
        if errors:
            return ValidationResult.fail(action.action_id, errors)

        # ---- 5. schema 约束 -----------------------------------------------
        errors.extend(_validate_parameter_values(params, schema["param_constraints"]))
        errors.extend(_validate_parameter_relations(params, schema["relational_constraints"]))

        # ---- 6. 语义约束 ---------------------------------------------------
        if action.action_type is ActionType.EXPRESS:
            emotion = params.get("emotion_state")
            if isinstance(emotion, str) and emotion not in _schemas.EXPRESS_EMOTION_STATES:
                errors.append(
                    ValidationError(
                        "parameters.emotion_state",
                        f"'{emotion}' is not a valid emotion state; allowed: {sorted(_schemas.EXPRESS_EMOTION_STATES)}",
                    )
                )

        if action.action_type is ActionType.OBSERVE:
            raw_attributes = params.get("attributes")
            if isinstance(raw_attributes, list):
                unknown_attrs = [
                    attr
                    for attr in raw_attributes
                    if not isinstance(attr, str) or attr not in _schemas.KNOWN_OBSERVE_ATTRIBUTES
                ]
                if unknown_attrs:
                    errors.append(
                        ValidationError(
                            "parameters.attributes",
                            f"unknown observe attributes: {unknown_attrs}; "
                            f"allowed: {sorted(_schemas.KNOWN_OBSERVE_ATTRIBUTES)}",
                        )
                    )
                # 每个元素都必须是字符串
                non_strings = [attr for attr in raw_attributes if not isinstance(attr, str)]
                if non_strings:
                    errors.append(
                        ValidationError(
                            "parameters.attributes",
                            f"all observe attributes must be strings, got: {[_type_label(a) for a in non_strings]}",
                        )
                    )

        if errors:
            return ValidationResult.fail(action.action_id, errors)
        return ValidationResult.ok(action.action_id)


# ---------------------------------------------------------------------------
# 辅助函数
# ---------------------------------------------------------------------------


def _validate_schema(schema: Mapping[str, object]) -> None:
    """拒绝可能导致 :meth:`ToolRegistry.validate` 抛出异常的 schema。"""
    if not isinstance(schema, Mapping):
        raise ValueError(f"schema must be a mapping, got {type(schema).__name__!r}")

    actual_keys = set(schema)
    if any(not isinstance(key, str) for key in actual_keys):
        raise ValueError("schema keys must be strings")
    missing_keys = _SCHEMA_KEYS - actual_keys
    unknown_keys = actual_keys - _SCHEMA_KEYS
    if missing_keys:
        raise ValueError(f"schema is missing keys: {sorted(missing_keys)}")
    if unknown_keys:
        raise ValueError(f"schema has unknown keys: {sorted(unknown_keys)}")

    description = schema["description"]
    if not isinstance(description, str):
        raise ValueError("schema 'description' must be a string")
    if not isinstance(schema["target_id_required"], bool):
        raise ValueError("schema 'target_id_required' must be a bool")

    required = _validate_param_set("required_params", schema["required_params"])
    optional = _validate_param_set("optional_params", schema["optional_params"])
    overlap = required & optional
    if overlap:
        raise ValueError(f"required_params and optional_params overlap: {sorted(overlap)}")

    param_types = schema["param_types"]
    if not isinstance(param_types, Mapping):
        raise ValueError("schema 'param_types' must be a mapping")
    allowed = required | optional
    declared = set(param_types)
    if any(not isinstance(key, str) for key in declared):
        raise ValueError("schema 'param_types' keys must be strings")
    missing_types = allowed - declared
    unknown_types = declared - allowed
    if missing_types:
        raise ValueError(f"param_types is missing allowed parameters: {sorted(missing_types)}")
    if unknown_types:
        raise ValueError(f"param_types contains unknown parameters: {sorted(unknown_types)}")
    unsupported = {
        name: value
        for name, value in param_types.items()
        if not any(value is supported_type for supported_type in _SUPPORTED_PARAM_TYPES)
    }
    if unsupported:
        labels = {name: _type_name(value) for name, value in unsupported.items()}
        raise ValueError(f"param_types contains unsupported types: {labels}")

    _validate_param_constraints(schema["param_constraints"], allowed, param_types)
    relations = _validate_param_set("relational_constraints", schema["relational_constraints"])
    unsupported_relations = relations - _SUPPORTED_RELATIONAL_CONSTRAINTS
    if unsupported_relations:
        raise ValueError(f"relational_constraints contains unsupported constraints: {sorted(unsupported_relations)}")
    if "destination_counts_consistent" in relations:
        if not _DESTINATION_COUNT_PARAMS <= allowed:
            raise ValueError("destination_counts_consistent requires all destination count parameters")
        if any(param_types[name] is not int for name in _DESTINATION_COUNT_PARAMS):
            raise ValueError("destination_counts_consistent requires integer destination count parameters")


def _validate_param_constraints(
    constraints: object,
    allowed: frozenset[str],
    param_types: Mapping[str, object],
) -> None:
    if not isinstance(constraints, Mapping):
        raise ValueError("schema 'param_constraints' must be a mapping")
    names = set(constraints)
    if any(not isinstance(name, str) for name in names):
        raise ValueError("schema 'param_constraints' keys must be strings")
    unknown_names = names - allowed
    if unknown_names:
        raise ValueError(f"param_constraints contains unknown parameters: {sorted(unknown_names)}")

    for name, raw_rules in constraints.items():
        if not isinstance(raw_rules, Mapping):
            raise ValueError(f"param_constraints for '{name}' must be a mapping")
        rule_names = set(raw_rules)
        if any(not isinstance(rule, str) for rule in rule_names):
            raise ValueError(f"param_constraints for '{name}' must have string keys")
        unknown_rules = rule_names - _CONSTRAINT_KEYS
        if unknown_rules:
            raise ValueError(f"param_constraints for '{name}' has unknown rules: {sorted(unknown_rules)}")

        expected_type = param_types[name]
        for flag in ("non_blank", "finite"):
            if flag in raw_rules and raw_rules[flag] is not True:
                raise ValueError(f"constraint '{flag}' for '{name}' must be true")
        if "non_blank" in raw_rules and expected_type is not str:
            raise ValueError(f"constraint 'non_blank' for '{name}' requires a string parameter")
        numeric_rules = rule_names & {"finite", "minimum", "maximum"}
        if numeric_rules and expected_type not in {int, float}:
            raise ValueError(f"numeric constraints for '{name}' require an int or float parameter")

        for bound_name in ("minimum", "maximum"):
            if bound_name not in raw_rules:
                continue
            bound = raw_rules[bound_name]
            if isinstance(bound, bool) or not isinstance(bound, int | float) or not _is_finite_number(bound):
                raise ValueError(f"constraint '{bound_name}' for '{name}' must be a finite number")
        if "minimum" in raw_rules and "maximum" in raw_rules:
            if raw_rules["minimum"] > raw_rules["maximum"]:
                raise ValueError(f"constraint minimum exceeds maximum for '{name}'")


def _validate_param_set(name: str, value: object) -> frozenset[str]:
    if not isinstance(value, set | frozenset):
        raise ValueError(f"schema '{name}' must be a set or frozenset of strings")
    non_strings = [item for item in value if not isinstance(item, str)]
    if non_strings:
        raise ValueError(f"schema '{name}' must contain only strings")
    return frozenset(value)


def _validate_parameter_values(
    params: Mapping[str, object], constraints: Mapping[str, object]
) -> list[ValidationError]:
    errors: list[ValidationError] = []
    for name, raw_rules in constraints.items():
        if name not in params:
            continue
        rules: Mapping[str, object] = raw_rules
        value = params[name]
        field = f"parameters.{name}"
        if rules.get("non_blank") and isinstance(value, str) and not value.strip():
            errors.append(ValidationError(field, "must be a non-empty, non-whitespace string"))
        if rules.get("finite") and isinstance(value, int | float) and not _is_finite_number(value):
            errors.append(ValidationError(field, "must be finite"))
            continue
        if "minimum" in rules and value < rules["minimum"]:
            errors.append(ValidationError(field, f"must be greater than or equal to {rules['minimum']}"))
        if "maximum" in rules and value > rules["maximum"]:
            errors.append(ValidationError(field, f"must be less than or equal to {rules['maximum']}"))
    return errors


def _validate_parameter_relations(params: Mapping[str, object], constraints: frozenset[str]) -> list[ValidationError]:
    errors: list[ValidationError] = []
    if "destination_counts_consistent" in constraints and _DESTINATION_COUNT_PARAMS & set(params):
        missing = _DESTINATION_COUNT_PARAMS - set(params)
        if missing:
            errors.append(
                ValidationError(
                    "parameters",
                    f"destination count snapshot is incomplete; missing: {sorted(missing)}",
                )
            )
        elif params["destination_capacity"] != (
            params["destination_occupancy_after"] + params["destination_remaining_after"]
        ):
            errors.append(
                ValidationError(
                    "parameters",
                    "destination counts must satisfy capacity = occupancy_after + remaining_after",
                )
            )
    return errors


def _type_label(value: object) -> str:
    if isinstance(value, bool):
        return "bool"
    return type(value).__name__


def _is_finite_number(value: int | float) -> bool:
    return isinstance(value, int) or isfinite(value)


def _freeze_mapping(value: Mapping[object, object]) -> Mapping:
    return MappingProxyType({key: _freeze_value(item) for key, item in value.items()})


def _freeze_value(value: object) -> object:
    if isinstance(value, Mapping):
        return _freeze_mapping(value)
    if isinstance(value, set | frozenset):
        return frozenset(_freeze_value(item) for item in value)
    if isinstance(value, list | tuple):
        return tuple(_freeze_value(item) for item in value)
    return value


def _type_name(tp: type | object) -> str:
    if tp is str:
        return "str"
    if tp is int:
        return "int"
    if tp is float:
        return "float"
    if tp is bool:
        return "bool"
    if tp is list:
        return "list"
    return getattr(tp, "__name__", str(tp))
