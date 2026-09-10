"""EvidenceSink 接口与 FakeEvidenceSink 的单元测试。

证明阶段 0 验收标准：``append()`` 返回稳定且唯一的引用，且 Motion 不持有持久化实现
（接口上没有 ``get``）。
"""

from __future__ import annotations

import json
from dataclasses import FrozenInstanceError

import pytest
from workbench_motion.evidence import EvidenceSink, ExecutionEvent, FakeEvidenceSink


def _event(action_id: str = "a-1") -> ExecutionEvent:
    return ExecutionEvent(
        event_type="trajectory_completed",
        run_id="run-1",
        action_id=action_id,
        payload={"controller": "succeeded"},
    )


def test_fake_sink_satisfies_interface() -> None:
    sink = FakeEvidenceSink()
    # runtime_checkable Protocol：该假实现结构上实现了 EvidenceSink。
    assert isinstance(sink, EvidenceSink)


def test_append_returns_reference() -> None:
    sink = FakeEvidenceSink()
    ref = sink.append(_event())
    assert isinstance(ref, str)
    assert ref


def test_reference_is_unique_even_for_identical_events() -> None:
    sink = FakeEvidenceSink()
    ref_a = sink.append(_event())
    ref_b = sink.append(_event())  # 相同 payload/id
    assert ref_a != ref_b
    assert len(set(sink.refs)) == len(sink.refs) == 2


def test_reference_is_stable_and_maps_to_its_event() -> None:
    sink = FakeEvidenceSink()
    ref_first = sink.append(_event("a-1"))
    ref_second = sink.append(_event("a-2"))
    # 追加返回的引用不会变化，并持续指向为其签发时的同一事件位置。
    assert sink.refs == [ref_first, ref_second]
    assert sink.events[0].action_id == "a-1"
    assert sink.events[1].action_id == "a-2"


def test_events_are_immutable() -> None:
    sink = FakeEvidenceSink()
    sink.append(_event())
    archived = sink.events[0]

    with pytest.raises(FrozenInstanceError):
        archived.action_id = "mutated"  # type: ignore[misc]


def test_payload_is_deeply_immutable_from_dict_input() -> None:
    """归档事件在任何嵌套深度都不可篡改。

    覆盖评审关切：早前的修复只是浅层的——修改原始 dict（顶层或嵌套），或经由
    ``event.payload`` 在顶层或嵌套 dict/list 中写入，都必须无法触及归档。
    """
    original = {"status": "ok", "nested": {"count": 1}, "items": [1, 2]}
    event = ExecutionEvent(event_type="t", run_id="r", action_id="a", payload=original)
    sink = FakeEvidenceSink()
    sink.append(event)
    archived = sink.events[0]

    # 1. 修改原始输入（任何深度）都不得触及归档。
    original["status"] = "corrupted"
    original["nested"]["count"] = 999
    original["items"].append(3)
    assert archived.payload["status"] == "ok"
    assert archived.payload["nested"]["count"] == 1
    assert archived.payload["items"] == (1, 2)

    # 2. 在顶层经 event.payload 写入必须抛错。
    with pytest.raises(TypeError):
        archived.payload["status"] = "x"  # type: ignore[index]

    # 3. 写入嵌套 dict 也必须抛错（这正是浅层修复失败之处）。
    with pytest.raises(TypeError):
        archived.payload["nested"]["count"] = 0  # type: ignore[index]

    # 4. 嵌套序列变为元组——无法 append/修改。
    assert isinstance(archived.payload["items"], tuple)
    with pytest.raises(AttributeError):
        archived.payload["items"].append(4)  # type: ignore[attr-defined]


def test_payload_is_isolated_from_mappingproxy_input() -> None:
    """传入已有的 MappingProxyType 绝不能泄漏其底层 dict。

    覆盖评审关切：早前的修复在输入已是 MappingProxyType 时跳过拷贝，持有底层 dict 的
    调用方仍可修改归档事件。_freeze 无论输入如何都重建，堵上了该漏洞。
    """
    from types import MappingProxyType

    backing = {"k": {"n": 1}}
    proxy = MappingProxyType(backing)
    event = ExecutionEvent(event_type="t", run_id="r", action_id="a", payload=proxy)
    sink = FakeEvidenceSink()
    sink.append(event)
    archived = sink.events[0]

    # 修改构建该 proxy 的底层 dict。
    backing["k"]["n"] = 999
    backing["added"] = True

    assert archived.payload["k"]["n"] == 1
    assert "added" not in archived.payload


def test_as_serializable_returns_plain_json_ready_dict() -> None:
    """冻结 payload 不可 JSON 序列化；as_serializable() 将其解冻。

    覆盖序列化回归关切：json.dumps 必须能处理输出，且嵌套结构以普通 dict/list 返回。
    """
    event = ExecutionEvent(
        event_type="grasp_done",
        run_id="run-1",
        action_id="a-1",
        payload={"controller": "succeeded", "joints": [0.1, 0.2], "meta": {"ok": True}},
    )
    data = event.as_serializable()

    # 普通类型，经 JSON 往返不出错。
    assert isinstance(data["payload"], dict)
    assert isinstance(data["payload"]["joints"], list)
    assert isinstance(data["payload"]["meta"], dict)
    restored = json.loads(json.dumps(data, allow_nan=False))
    assert restored["payload"]["controller"] == "succeeded"
    assert restored["payload"]["joints"] == [0.1, 0.2]
    assert restored["event_type"] == "grasp_done"


def test_set_payload_is_deeply_immutable_and_json_ready() -> None:
    original = {3, 1, 2}
    event = ExecutionEvent(
        event_type="controller_failed",
        run_id="run-1",
        action_id="a-1",
        payload={"fault_codes": original, "nested": {"labels": frozenset({"b", "a"})}},
    )
    sink = FakeEvidenceSink()
    sink.append(event)
    archived = sink.events[0]

    original.add(4)
    assert archived.payload["fault_codes"] == frozenset({1, 2, 3})
    assert archived.payload["nested"]["labels"] == frozenset({"a", "b"})
    with pytest.raises(AttributeError):
        archived.payload["fault_codes"].add(5)  # type: ignore[attr-defined]

    serialized = archived.as_serializable()
    assert serialized["payload"]["fault_codes"] == [1, 2, 3]
    assert serialized["payload"]["nested"]["labels"] == ["a", "b"]
    assert json.loads(json.dumps(serialized)) == serialized


@pytest.mark.parametrize("value", [float("nan"), float("inf"), float("-inf")])
def test_non_finite_float_payloads_fail_closed(value: float) -> None:
    with pytest.raises(ValueError, match="float values must be finite"):
        ExecutionEvent(
            event_type="controller_failed",
            run_id="run-1",
            action_id="a-1",
            payload={"position_error": value},
        )


def test_invalid_payload_types_fail_closed() -> None:
    class MutableValue:
        pass

    with pytest.raises(TypeError, match="payload must be a mapping"):
        ExecutionEvent(event_type="t", run_id="r", action_id="a", payload=[])  # type: ignore[arg-type]

    with pytest.raises(TypeError, match="mapping keys must be strings"):
        ExecutionEvent(event_type="t", run_id="r", action_id="a", payload={1: "bad"})  # type: ignore[dict-item]

    with pytest.raises(TypeError, match="unsupported payload value type: MutableValue"):
        ExecutionEvent(
            event_type="t",
            run_id="r",
            action_id="a",
            payload={"bad": MutableValue()},
        )


def test_recursive_payload_fails_closed() -> None:
    recursive: dict[str, object] = {}
    recursive["self"] = recursive

    with pytest.raises(TypeError, match="must not contain recursive containers"):
        ExecutionEvent(event_type="t", run_id="r", action_id="a", payload=recursive)


def test_sink_append_error_propagates_without_minting_reference() -> None:
    sink = FakeEvidenceSink(append_error=RuntimeError("durable append failed"))

    with pytest.raises(RuntimeError, match="durable append failed"):
        sink.append(_event())

    assert len(sink) == 0
    assert sink.refs == []


def test_motion_side_holds_no_read_api() -> None:
    # 强化边界：Motion 对话的接口只暴露 append，绝不暴露 get/查询。
    # Motion 不能变成第二个事件库。
    assert hasattr(EvidenceSink, "append")
    assert not hasattr(EvidenceSink, "get")
