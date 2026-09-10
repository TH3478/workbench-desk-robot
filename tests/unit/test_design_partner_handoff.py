from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
HANDOFF = ROOT / "docs/product/design-partner-handoff.md"
SCENARIO_TEMPLATE = ROOT / "docs/product/design-partner-scenario-template.md"
PRODUCT_README = ROOT / "docs/product/README.md"
MKDOCS = ROOT / "mkdocs.yml"


def _text(path: Path) -> str:
    return path.read_text(encoding="utf-8").casefold()


def test_handoff_answers_the_required_owner_response_sections() -> None:
    text = _text(HANDOFF)
    required_sections = (
        "## 1. 决策词汇表",
        "## 2. 语义动作契约",
        "## 3. 运动适配器结果边界",
        "## 4. 交接检查清单",
        "## 5. 证据阶梯",
        "## 6. 中止、停止、恢复与确认权限",
        "## 7. 产品验收与运动/安全验收",
        "## 8. 被拒绝的请求示例",
        "## 9. 可复制的交接记录",
        "## 10. 后续步骤与决策记录",
    )
    for section in required_sections:
        assert section in text


def test_handoff_preserves_semantic_and_safety_boundaries() -> None:
    text = _text(HANDOFF)
    required_terms = (
        "continue",
        "change",
        "defer",
        "reject",
        "语义动作",
        "actionresult",
        "worldstate",
        "safe_stop",
        "e-stop",
        "mcu-safety",
        "关节位置",
        "原始 can 帧",
        "控制器目标",
        "安全使能",
        "不授予",
        "必须拒绝",
    )
    for term in required_terms:
        assert term in text


def test_handoff_keeps_evidence_classes_and_incomplete_statuses_distinct() -> None:
    text = _text(HANDOFF)
    for term in (
        "software",
        "scripted_fixture",
        "gazebo",
        "physical",
        "release_eligible: false",
        "confirmed",
        "failed",
        "refuted",
        "insufficient_evidence",
        "not_executed",
        "blocked",
    ):
        assert term in text


def test_generic_scenario_template_points_to_the_stricter_handoff() -> None:
    template = _text(SCENARIO_TEMPLATE)
    readme = _text(PRODUCT_README)
    mkdocs = _text(MKDOCS)
    assert "design partner 交接边界" in template
    assert "交接决策" in template
    assert "design-partner-handoff.md" in readme
    assert "product/design-partner-handoff.md" in mkdocs
