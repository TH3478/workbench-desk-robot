"""对照其 schema 与类型化模型，校验每一个已提交的契约示例。

此处运行的独立检查：
1. 每个 schema 都已注册（不允许出现既无示例又无书面理由的 schema）
2. 所有示例文件都能解析为合法 JSON
3. schema 中的所有必填字段都在 properties 中定义
4. 所有示例都满足其 schema 的必填字段
5. jsonschema Draft-2020-12 完整结构校验（enum、类型、allOf、$ref 等）
6. Pydantic 模型接受其示例（运行时类型检查）
7. Pydantic 输出满足源 schema，并拒绝省略 schema 必填字段的输入
8. 模板规划器可以往返序列化为 JSON

第 5 项 jsonschema 检查正是此前缺失的一环——它能够捕获 enum 不匹配、
类型错误、范围越界与 $ref 约束，而这些是手工字段存在性检查会静默忽略的。
"""

import json
import sys
from dataclasses import dataclass
from typing import Any

from _paths import ROOT, enable_local_packages

enable_local_packages()

from workbench_agent_runtime import build_template_plan
from workbench_contracts import (
    ActionResult,
    EmotionIntent,
    McuFrame,
    Observation,
    Pose,
    ScenarioManifest,
    SemanticAction,
    TaskGraph,
    VerificationResult,
    WorldEvent,
    WorldState,
)

try:
    from jsonschema import Draft202012Validator
    from referencing import Registry, Resource

    HAS_JSONSCHEMA = True
except ImportError:
    HAS_JSONSCHEMA = False

SCHEMA_DIR = ROOT / "interfaces" / "json_schema"
EXAMPLE_DIR = ROOT / "interfaces" / "examples"


@dataclass(frozen=True)
class SchemaCoverage:
    """单个已提交 JSON Schema 的可执行归属记录。"""

    stem: str
    example: str | None
    model: type[Any] | None
    schema_only_fields: frozenset[str] = frozenset()
    exemption_owner: str | None = None
    exemption_reason: str | None = None
    replacement_validation: str | None = None


# 每个 schema 都必须有一条记录。只有在三个豁免字段全部填写时才允许缺少 example/model；
# 这样既保留了经过评审的逃生通道，又不会让未经追踪的 None 记录悄悄降低覆盖率。
SCHEMA_COVERAGE = (
    SchemaCoverage("action_result", "action-result-place-confirmed.json", ActionResult),
    SchemaCoverage("emotion_intent", "emotion-intent-uncertain.json", EmotionIntent),
    SchemaCoverage(
        "mcu_protocol",
        "mcu-frame-stop-ack.json",
        McuFrame,
        schema_only_fields=frozenset({"sent_at"}),
        exemption_owner="TH3478",
        exemption_reason="Legacy Schema Compiler field rejected by every MCU Wire V1 frame branch.",
        replacement_validation="tests/unit/test_mcu_protocol_contract.py invalid-frame corpus",
    ),
    SchemaCoverage("observation", "observation-red-block.json", Observation),
    SchemaCoverage("pose", "pose-tabletop.json", Pose),
    SchemaCoverage("scenario", "scenario-normal-001.json", ScenarioManifest),
    SchemaCoverage("semantic_action", "semantic-action-place.json", SemanticAction),
    SchemaCoverage("task_graph", "task-graph-place.json", TaskGraph),
    SchemaCoverage("verification_result", "verification-insufficient-evidence.json", VerificationResult),
    SchemaCoverage("world_event", "world-event-observation.json", WorldEvent),
    SchemaCoverage("world_state", "world-state-block-in-tray.json", WorldState),
)


def check_every_schema_is_registered() -> list[str]:
    on_disk = {p.name.removesuffix(".schema.json") for p in SCHEMA_DIR.glob("*.schema.json")}
    entries_by_stem: dict[str, list[SchemaCoverage]] = {}
    for entry in SCHEMA_COVERAGE:
        entries_by_stem.setdefault(entry.stem, []).append(entry)
    problems = []
    for stem in sorted(on_disk):
        entries = entries_by_stem.get(stem, [])
        if not entries:
            problems.append(f"{stem}.schema.json has no coverage entry")
            continue
        if len(entries) != 1:
            problems.append(f"{stem}.schema.json has {len(entries)} coverage entries; exactly one is required")
            continue
        entry = entries[0]
        has_model = entry.model is not None
        has_example = entry.example is not None
        has_exemption = all(
            isinstance(value, str) and bool(value.strip())
            for value in (entry.exemption_owner, entry.exemption_reason, entry.replacement_validation)
        )
        if not (has_model and has_example) and not has_exemption:
            problems.append(
                f"{stem}.schema.json needs a typed model and example, or an owner/reason/replacement exemption"
            )
        if entry.schema_only_fields and not has_exemption:
            problems.append(f"{stem}.schema.json schema-only fields need owner/reason/replacement metadata")
    for stem in sorted(set(entries_by_stem) - on_disk):
        problems.append(f"{stem} has a coverage entry but no committed schema")
    return problems


def check_schema_properties_have_model_fields() -> list[str]:
    """捕获映射的对象模型会拒绝的已声明 schema 字段。"""
    problems = []
    for entry in sorted(SCHEMA_COVERAGE, key=lambda item: item.stem):
        if entry.model is None:
            continue
        schema_path = SCHEMA_DIR / f"{entry.stem}.schema.json"
        if not schema_path.is_file():
            continue
        schema = json.loads(schema_path.read_text(encoding="utf-8"))
        model_schema = entry.model.model_json_schema()
        model_fields = set(model_schema.get("properties", {}))
        if not model_fields:
            for definition in model_schema.get("$defs", {}).values():
                model_fields.update(definition.get("properties", {}))
        missing = set(schema.get("properties", {})) - model_fields
        for field_name in sorted(missing - entry.schema_only_fields):
            problems.append(f"{entry.stem}.schema.json field {field_name!r} has no mapped Pydantic field")
        for field_name in sorted(entry.schema_only_fields - missing):
            problems.append(
                f"{entry.stem}.schema.json field {field_name!r} is marked schema-only but has a Pydantic field"
            )
        if model_schema.get("properties") is not None:
            schema_required = set(schema.get("required", []))
            model_required = set(model_schema.get("required", []))
            for field_name in sorted(schema_required - model_required):
                problems.append(f"{entry.stem}.schema.json field {field_name!r} is required but optional in Pydantic")
            for field_name in sorted(model_required - schema_required):
                problems.append(f"{entry.model.__name__} requires schema-optional field {field_name!r}")
    return problems


def check_examples_parse() -> list[str]:
    problems = []
    for path in sorted(EXAMPLE_DIR.glob("*.json")):
        try:
            json.loads(path.read_text(encoding="utf-8"))
        except json.JSONDecodeError as exc:
            problems.append(f"{path.name} is not valid JSON: {exc}")
    return problems


def check_required_fields_are_defined() -> list[str]:
    """要求一个从未定义字段的 schema 会静默接受缺少该字段的文档。
    此检查的存在是因为有两个 schema 曾经恰好带着该缺陷发布。"""
    problems = []
    for path in sorted(SCHEMA_DIR.glob("*.schema.json")):
        schema = json.loads(path.read_text(encoding="utf-8"))
        defined = set(schema.get("properties", {}))
        for field in schema.get("required", []):
            if field not in defined:
                problems.append(f"{path.name} requires '{field}' but never defines it")
    return problems


def check_examples_satisfy_required() -> list[str]:
    problems = []
    for entry in sorted(SCHEMA_COVERAGE, key=lambda item: item.stem):
        stem, example_name = entry.stem, entry.example
        if example_name is None:
            continue
        example_path = EXAMPLE_DIR / example_name
        if not example_path.is_file():
            problems.append(f"{stem}: example {example_name} is missing")
            continue
        schema = json.loads((SCHEMA_DIR / f"{stem}.schema.json").read_text(encoding="utf-8"))
        example = json.loads(example_path.read_text(encoding="utf-8"))
        for field in schema.get("required", []):
            if field not in example:
                problems.append(f"{example_name} is missing required field '{field}'")
    return problems


def _schema_registry() -> "Registry":
    """以裸文件名注册每个 schema，使 {"$ref": "pose.schema.json"} 这类同级 $ref
    无需网络抓取即可解析。"""
    resources = []
    for path in sorted(SCHEMA_DIR.glob("*.schema.json")):
        contents = json.loads(path.read_text(encoding="utf-8"))
        resources.append((path.name, Resource.from_contents(contents)))
    return Registry().with_resources(resources)


def check_jsonschema_validation() -> list[str]:
    """Draft-2020-12 完整结构校验：enum、类型、范围、allOf、$ref。
    这正是此前版本所缺失的——上面的检查只验证字段是否存在，不验证字段值。
    """
    if not HAS_JSONSCHEMA:
        return [
            "jsonschema is not installed; run `pip install jsonschema` to enable "
            "full structural validation. Install it by adding 'jsonschema>=4,<5' to "
            "[project.optional-dependencies].dev in pyproject.toml."
        ]

    problems = []
    for entry in sorted(SCHEMA_COVERAGE, key=lambda item: item.stem):
        stem, example_name = entry.stem, entry.example
        if example_name is None:
            continue
        schema_path = SCHEMA_DIR / f"{stem}.schema.json"
        example_path = EXAMPLE_DIR / example_name
        if not example_path.is_file():
            continue  # 已由 check_examples_satisfy_required 报告

        schema = json.loads(schema_path.read_text(encoding="utf-8"))
        example = json.loads(example_path.read_text(encoding="utf-8"))

        validator = Draft202012Validator(schema, registry=_schema_registry())

        errors = list(validator.iter_errors(example))
        for err in errors:
            path = " -> ".join(str(p) for p in err.absolute_path) or "(root)"
            problems.append(f"{example_name} [{path}]: {err.message}")
    return problems


def check_models_accept_examples() -> list[str]:
    problems = []
    for entry in sorted(SCHEMA_COVERAGE, key=lambda item: item.stem):
        example_name, model = entry.example, entry.model
        if example_name is None or model is None:
            continue
        path = EXAMPLE_DIR / example_name
        if not path.is_file():
            problems.append(f"{example_name}: file missing, cannot validate {model.__name__}")
            continue
        try:
            model.model_validate_json(path.read_text(encoding="utf-8"))
        except Exception as exc:  # noqa: BLE001
            problems.append(f"{model.__name__} rejected {example_name}: {exc}")
    return problems


def check_bidirectional_model_validation() -> list[str]:
    """同时校验已提交的 JSON 输入与规范的模型输出。"""
    if not HAS_JSONSCHEMA:
        return ["jsonschema is not installed; bidirectional contract validation is unavailable"]

    problems = []
    registry = _schema_registry()
    for entry in sorted(SCHEMA_COVERAGE, key=lambda item: item.stem):
        if entry.example is None or entry.model is None:
            continue
        example_path = EXAMPLE_DIR / entry.example
        schema_path = SCHEMA_DIR / f"{entry.stem}.schema.json"
        if not example_path.is_file() or not schema_path.is_file():
            continue
        try:
            raw = example_path.read_text(encoding="utf-8")
            payload = json.loads(raw)
            value = entry.model.model_validate_json(raw)
            serialized = value.model_dump(mode="json")
            schema = json.loads(schema_path.read_text(encoding="utf-8"))
            validator = Draft202012Validator(schema, registry=registry)
            errors = list(validator.iter_errors(serialized))
        except Exception as exc:  # noqa: BLE001
            problems.append(f"{entry.stem}: model-to-schema validation failed: {exc}")
            continue
        for error in errors:
            path = " -> ".join(str(item) for item in error.absolute_path) or "(root)"
            problems.append(f"{entry.example} serialized [{path}]: {error.message}")

        for field_name in schema.get("required", []):
            if field_name not in payload:
                continue
            without_required = dict(payload)
            del without_required[field_name]
            if not list(validator.iter_errors(without_required)):
                problems.append(f"{entry.stem}.schema.json declares {field_name!r} required but accepts it missing")
                continue
            try:
                entry.model.model_validate_json(json.dumps(without_required))
            except Exception:  # noqa: BLE001
                continue
            problems.append(
                f"{entry.model.__name__} accepts {entry.example} without schema-required field {field_name!r}"
            )

        for field_name in sorted(set(schema.get("properties", {})) - set(schema.get("required", []))):
            if field_name not in payload:
                continue
            without_optional = dict(payload)
            del without_optional[field_name]
            if list(validator.iter_errors(without_optional)):
                continue
            try:
                optional_value = entry.model.model_validate_json(json.dumps(without_optional))
                optional_serialized = optional_value.model_dump(mode="json")
                optional_errors = list(validator.iter_errors(optional_serialized))
            except Exception as exc:  # noqa: BLE001
                problems.append(
                    f"{entry.model.__name__} rejected {entry.example} without schema-optional "
                    f"field {field_name!r}: {exc}"
                )
                continue
            for error in optional_errors:
                path = " -> ".join(str(item) for item in error.absolute_path) or "(root)"
                problems.append(f"{entry.example} without optional {field_name!r} serialized [{path}]: {error.message}")
    return problems


def check_planner_round_trips() -> list[str]:
    try:
        plan = build_template_plan("Place the red block in the tray")
        json.loads(plan.model_dump_json())
    except Exception as exc:  # noqa: BLE001
        return [f"template planner did not produce a serialisable TaskGraph: {exc}"]
    return []


def main() -> int:
    checks = [
        ("every schema registered", check_every_schema_is_registered),
        ("schema properties map to Pydantic fields", check_schema_properties_have_model_fields),
        ("examples parse as JSON", check_examples_parse),
        ("required fields are defined", check_required_fields_are_defined),
        ("examples satisfy required fields", check_examples_satisfy_required),
        ("jsonschema Draft-2020-12 validation", check_jsonschema_validation),
        ("Pydantic models accept examples", check_models_accept_examples),
        ("Pydantic JSON serialization satisfies schemas", check_bidirectional_model_validation),
        ("template planner round-trips", check_planner_round_trips),
    ]
    failed = False
    for label, check in checks:
        problems = check()
        if problems:
            failed = True
            print(f"FAIL  {label}")
            for problem in problems:
                print(f"        {problem}")
        else:
            print(f"ok    {label}")
    if failed:
        print("\ncontract validation failed", file=sys.stderr)
        return 1
    print("\ncontract validation passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
