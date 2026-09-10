# Kernel (#13) - ROS 2 系统层

## 概述

内核工程师的 P1 完整实现 (K1-K10)

## 组件

- **K1-K2**：Schema Compiler（JSON Schema → TypeScript + Python）
- **K3**：Version Registry（schema 版本管理）
- **K4-K5**：Communication Layer（版本化消息 + schema 校验）
- **K6-K7**：Event Store（带回放与检查点的仅追加 SQLite 日志）
- **K8**：ROS 2 Lifecycle（created → configured → active → deactivated → finalized）
- **K9-K10**：System Startup（bootstrap + checklist）

## 用法

```python
from workbench.kernel.schema_compiler import SchemaCompiler
from workbench.kernel.event_store import EventStore, migrate_jsonl
from workbench.kernel.lifecycle import LifecycleManager

# Schema compilation
compiler = SchemaCompiler(schemas_dir)
compiler.load_schemas()
compiler.compile_all(py_output_dir, ts_output_dir)

# Event logging
store = EventStore("runs/events.sqlite3")
store.append(
    {
        "event_id": "event-1",
        "run_id": "run-1",
        "sequence_no": 0,
        "event_type": "observation",
        "occurred_at": "2026-08-13T00:00:00Z",
        "payload": {"entity_id": "red_block"},
    }
)
checkpoint = store.create_checkpoint()
replayed_events = store.replay(from_checkpoint=checkpoint)

# One-time migration from a strict JSONL log:
migrate_jsonl("runs/events.jsonl", "runs/events.sqlite3")

# Backup/restore verifies a SHA-256 manifest before replacement:
store.backup("runs/events.snapshot.sqlite3")
restored = EventStore.restore("runs/events.snapshot.sqlite3", "runs/events-restored.sqlite3")

# System lifecycle
manager = LifecycleManager()
node = manager.create_node("kernel")
manager.startup_sequence()
```

SQLite 后端是推荐的运行时存储。它持久化检查点并保留完整的事件 JSON，因此未知的扩展字段能在迁移中幸存。`.jsonl` 仍是兼容后端；仅在迁移期间用 `EventStore(path, legacy_objects=True)` 打开旧式对象日志。PostgreSQL 刻意不内置：当部署提供 PostgreSQL DSN 时，再添加已批准的 DB-API 驱动与适配器。

快照使用 SQLite 的在线备份 API 加一个 SHA-256 sidecar 清单。恢复前关闭目标存储；在原子替换前检查校验和、schema 版本、数据库完整性与事件计数。保留每日快照 30 天。初始本地目标是 RPO <= 24 小时与 RTO <= 30 分钟；部署 Owner 必须依据实测恢复演练收紧这些值。

## 测试

```bash
python tests/test_k1_k10.py
```

所有 K1-K10 测试通过。
