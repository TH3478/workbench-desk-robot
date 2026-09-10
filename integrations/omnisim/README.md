# OmniSim 试点

这个可选试点只与 loopback 上的 OmniSim World Harness 通信。它以轻量模式加载一个厂商世界，验证已连接的 supervisor 与已定型的非降级 Newton 后端，恢复作者设定的状态，推进受限的步数，并把原始厂商事件记录进一个原子的、带校验和的产物。

```python
from pathlib import Path

from integrations.omnisim import OmniSimClient, OmniSimPilotRunner

result = OmniSimPilotRunner(OmniSimClient()).run(
    "projects/samples/demos/worlds/showcase/warehouse_husky.omniworld",
    Path("runs/omnisim"),
)
print(result.status, result.artifact_dir)
```

运行试点前，用 `python -m omnisim harness` 启动单独安装的仿真器。每个产物固定为 `evidence_class: SIMULATION`、`physical_evidence: false`、`release_eligible: false` 与 `mapped_to_workbench_event_contract: false`。

本适配器不替代 `tools/scripts/sim_cli.py`，不翻译 Workbench 场景清单，也不把 OmniSim 事件提升进既有事件日志契约。这些需要后续带显式映射、重置隔离、机械臂、夹爪、相机与碰撞测试的兼容切片。
