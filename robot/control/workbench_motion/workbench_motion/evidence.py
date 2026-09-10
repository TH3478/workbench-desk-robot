"""执行事件结构与 EvidenceSink 接口。

设计边界：

- Motion 不拥有事实库。本模块定义执行事件*长什么样*以及 Motion 与之对话的*仅追加*
  sink 接口。它不实现持久化，并且刻意不暴露任何 ``get``/查询。
- Motion 调用 ``append(event)`` 并收到一个**稳定引用**，可放入 ActionResult 的
  ``evidence_refs``。生产持久化由 World Model 侧（事件库适配器）提供；该适配器是
  跨模块依赖，目前假定尚不存在。
- 单元测试使用 :class:`FakeEvidenceSink`。

为什么用事件而不是日志行：``evidence_refs`` 必须指向具有稳定 id 的对象（MCU 帧号，
或带稳定 ``event_id`` 的结构化事件）。日志行是给人看的，不是可引用的证据。
"""

from __future__ import annotations

import json
import math
import uuid
from collections.abc import Mapping, Sequence, Set
from dataclasses import dataclass, field
from types import MappingProxyType
from typing import Any, Protocol, runtime_checkable

# 稳定、不透明的引用，由 sink 持久记录事件后返回。Motion 将其视为不透明令牌，
# 仅存入 evidence_refs。
EvidenceRef = str


def _freeze(value: Any, *, _ancestors: set[int] | None = None) -> Any:
    """递归地把 ``value`` 重建为深层不可变结构。

    - ``dict``/``Mapping`` -> 冻结元素组成的 ``MappingProxyType``。
    - ``list``/``tuple``（非 str 的 Sequence）-> 冻结元素组成的 ``tuple``。
    - ``set``/``frozenset`` -> 冻结元素组成的 ``frozenset``。
    - 严格 JSON 标量值原样返回；NaN/Infinity 被拒绝。
    - 不支持的值、非字符串映射键、循环结构一律失败即拒绝。

    由于容器是从零重建的，结果与调用方输入不共享任何可变对象——传入已有的 dict 或
    MappingProxyType 后再修改原对象，都无法触及冻结副本。这正是单独 ``frozen=True``
    给不了你的保证（它只阻止字段重新赋值，不阻止字段所指向的 dict 被修改）。
    """
    if isinstance(value, float):
        if not math.isfinite(value):
            raise ValueError("payload float values must be finite")
        return value
    if value is None or isinstance(value, str | int | bool):
        return value

    ancestors = set() if _ancestors is None else _ancestors
    identity = id(value)
    if identity in ancestors:
        raise TypeError("payload must not contain recursive containers")

    if isinstance(value, Mapping):
        ancestors.add(identity)
        try:
            frozen: dict[str, Any] = {}
            for key, item in value.items():
                if not isinstance(key, str):
                    raise TypeError("payload mapping keys must be strings")
                frozen[key] = _freeze(item, _ancestors=ancestors)
            return MappingProxyType(frozen)
        finally:
            ancestors.remove(identity)

    if isinstance(value, Set):
        ancestors.add(identity)
        try:
            return frozenset(_freeze(item, _ancestors=ancestors) for item in value)
        finally:
            ancestors.remove(identity)

    if isinstance(value, Sequence) and not isinstance(value, str | bytes | bytearray):
        ancestors.add(identity)
        try:
            return tuple(_freeze(item, _ancestors=ancestors) for item in value)
        finally:
            ancestors.remove(identity)

    raise TypeError(f"unsupported payload value type: {type(value).__name__}")


def _json_sort_key(value: Any) -> str:
    """为从无序集合解冻的值返回稳定键。"""
    return json.dumps(
        value,
        allow_nan=False,
        ensure_ascii=False,
        separators=(",", ":"),
        sort_keys=True,
    )


def _thaw(value: Any) -> Any:
    """:func:`_freeze` 的逆操作：为序列化重建普通 ``dict``/``list``。

    ``MappingProxyType`` 不可 JSON 序列化，会破坏 ``json.dumps`` /
    ``dataclasses.asdict``。需要持久化事件的消费方调用
    :meth:`ExecutionEvent.as_serializable`（内部经由此函数）来取回普通的、可直接
    JSON 的结构。冻结集合变成确定性排序的列表，使重复序列化产生稳定的证据字节。
    """
    if isinstance(value, Mapping):
        return {key: _thaw(item) for key, item in value.items()}
    if isinstance(value, tuple):
        return [_thaw(item) for item in value]
    if isinstance(value, frozenset):
        return sorted((_thaw(item) for item in value), key=_json_sort_key)
    return value


@dataclass(frozen=True)
class ExecutionEvent:
    """Motion 发出的一条已观测执行事实。

    归档事件必须防篡改。单独 ``frozen=True`` 不够：它阻止字段重新赋值，但不阻止字段
    所指向的 ``dict``/``list`` 被修改。因此 ``__post_init__`` 递归地把 ``payload``
    重建为深层不可变结构（``MappingProxyType``/``tuple``，见 :func:`_freeze`），与
    调用方不共享任何可变对象——嵌套值无法修改，输入引用（dict 或 MappingProxyType）
    事后也无法触及存储的副本。

    由于该结构不可 JSON 序列化，:meth:`as_serializable` 把它解冻回普通
    ``dict``/``list``，供持久化的 EvidenceSink 使用。

    携带 ``run_id``/``action_id`` 以便与（独立的）人类日志流关联，以及一个对单调时钟
    友好的 ``clock_id`` 以保持时间戳一致。阶段 0 刻意保持最小；更丰富的字段随适配器
    在后续阶段落地。
    """

    event_type: str
    run_id: str
    action_id: str
    payload: Mapping[str, Any] = field(default_factory=dict)
    clock_id: str = "monotonic"

    def __post_init__(self) -> None:
        # 深度冻结 payload。frozen=True 阻止重新赋值，故通过 object.__setattr__ 设置。
        # _freeze 重建每个容器，因此无论 payload 以 dict 还是 MappingProxyType 传入，
        # 这样设置都是安全（且正确）的。
        if not isinstance(self.payload, Mapping):
            raise TypeError("payload must be a mapping")
        object.__setattr__(self, "payload", _freeze(self.payload))

    def as_serializable(self) -> dict[str, Any]:
        """返回本事件的普通、严格 JSON 就绪的 dict（payload 已解冻）。

        用于持久化/序列化——``json.dumps`` 与 ``dataclasses.asdict`` 无法直接处理
        冻结的 ``MappingProxyType`` payload。
        """
        return {
            "event_type": self.event_type,
            "run_id": self.run_id,
            "action_id": self.action_id,
            "payload": _thaw(self.payload),
            "clock_id": self.clock_id,
        }


@runtime_checkable
class EvidenceSink(Protocol):
    """Motion 写入执行事件的仅追加 sink。

    Motion 需要的*唯一*操作。实现必须返回满足以下条件的引用：
      - **稳定**：准确标识所追加的那个事件，供存储拥有方日后查找。
      - **唯一**：每次追加都不同，即使 payload 相同。

    Motion 自身不持有持久化，也从不回读——这里刻意没有 ``get``（不设第二个事件库）。

    ``ExecutionEvent`` 刻意存储深层不可变的 payload，因此不能直接 JSON 序列化。
    持久化实现必须序列化 :meth:`ExecutionEvent.as_serializable`；不得使用
    ``json.dumps(event)`` 或 ``dataclasses.asdict(event)``。

    校验、序列化或持久写入的失败必须抛给调用方。实现不得吞掉失败，也不得为未被持久
    记录的事件签发证据引用。
    """

    def append(self, event: ExecutionEvent) -> EvidenceRef:
        """持久记录 ``event.as_serializable()`` 并返回其引用。"""
        ...


class FakeEvidenceSink:
    """实现 :class:`EvidenceSink` 的内存测试替身。

    仅供单元测试。保留已追加事件以便测试断言，并为每次追加签发稳定且唯一的引用。
    它不是生产存储——它存在是为了让 Motion 测试永不依赖 World Model 适配器。
    ``append_error`` 为调用方测试提供确定性的故障注入。
    """

    def __init__(self, *, append_error: Exception | None = None) -> None:
        self._events: list[tuple[EvidenceRef, ExecutionEvent]] = []
        self._append_error = append_error

    def append(self, event: ExecutionEvent) -> EvidenceRef:
        if self._append_error is not None:
            raise self._append_error
        ref: EvidenceRef = f"evt:{uuid.uuid4()}"
        self._events.append((ref, event))
        return ref

    # --- 仅供测试的检查辅助（不属于 EvidenceSink 接口） ---

    @property
    def events(self) -> list[ExecutionEvent]:
        """按追加顺序排列的事件。"""
        return [event for _, event in self._events]

    @property
    def refs(self) -> list[EvidenceRef]:
        """按追加顺序排列的引用。"""
        return [ref for ref, _ in self._events]

    def __len__(self) -> int:
        return len(self._events)
