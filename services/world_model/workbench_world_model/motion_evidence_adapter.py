"""在 World Model 存储中持久化 Motion 的动作结果执行事件。

该适配器在结构上兼容 Motion 的仅追加 EvidenceSink，但有意不在运行时
导入独立打包的 workbench_motion 模块。
"""

from __future__ import annotations

import json
from typing import Any, Protocol

from workbench_contracts import WorldEvent, WorldEventType

from .event_payloads import (
    WorldEventPayloadValidationError,
    normalize_action_result_payload,
)
from .event_store import SQLiteEventStore

REFERENCE_PREFIX = "world-event:"


class MotionEvidenceValidationError(ValueError):
    """该执行事件无法表示为有效的 ActionResult 事件。"""


class SerializableExecutionEvent(Protocol):
    def as_serializable(self) -> dict[str, Any]:
        """以可直接用于严格 JSON 的分离数据形式返回事件。"""
        ...


class MotionEvidenceAdapter:
    """由 World Model 拥有的 Motion EvidenceSink 持久化实现。"""

    def __init__(self, store: SQLiteEventStore) -> None:
        self._store = store

    def append(self, event: SerializableExecutionEvent) -> str:
        serialized = event.as_serializable()
        if not isinstance(serialized, dict):
            raise MotionEvidenceValidationError("ExecutionEvent.as_serializable() must return a mapping")

        json.dumps(
            serialized,
            allow_nan=False,
            ensure_ascii=False,
            separators=(",", ":"),
            sort_keys=True,
        )

        if serialized.get("event_type") != "action_result":
            raise MotionEvidenceValidationError("only action_result execution events can be persisted")

        try:
            result = normalize_action_result_payload(
                serialized.get("payload"),
                event_run_id=serialized.get("run_id"),
                expected_action_id=serialized.get("action_id"),
            )
        except WorldEventPayloadValidationError as error:
            raise MotionEvidenceValidationError(
                "execution event payload is not a valid correlated ActionResult"
            ) from error

        event_id = f"motion-result:{result.run_id}:{result.result_id}"
        stored = self._store.append_allocated(
            event_id=event_id,
            run_id=result.run_id,
            event_type=WorldEventType.ACTION_RESULT,
            occurred_at=result.ended_at,
            payload=result.model_dump(mode="json"),
            evidence_refs=list(result.evidence_refs),
        )
        return f"{REFERENCE_PREFIX}{stored.event_id}"

    def resolve(self, reference: str) -> WorldEvent | None:
        if not reference.startswith(REFERENCE_PREFIX):
            return None
        event_id = reference.removeprefix(REFERENCE_PREFIX)
        if not event_id:
            return None
        return self._store.get_event(event_id)
