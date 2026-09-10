# RLSOK Shadow 试点

这个可选适配器调用 RLSOK 的独立 `shadow` 命令，然后调用其 `verify-evidence` 命令。它把被阻止的决策当作真实结果接受，但要求命令摘要与每条证据条目中同时满足 `controllerGoalsAttempted: 0` 与 `hardwareSignalSent: false`。

```python
from pathlib import Path

from integrations.rlsok import RlsokShadowRunner

result = RlsokShadowRunner().run(
    Path("integrations/rlsok/examples/workbench-release.shadow.yaml"),
    Path("integrations/rlsok/examples/workbench-pick-place-proposal.json"),
    Path("runs/rlsok/evidence.json"),
)
print(result.decision, result.evidence_ref)
```

适配器绝不调用 `rlsok run`、ROS 控制器或 Workbench 的 `ActionAdapter`。独立 Shadow 是兼容性与证据格式探针；它不是 Hosted Cloud 审批流程，也不能证明本机器人的控制器绑定。官方正式路径仍需要 Ubuntu 24.04、ROS 2 Jazzy、Fast DDS、受支持的控制器图、Cloud 配对与独立审批。RLSOK 不替代 E-stop、看门狗、控制器限位、运动规划或物理验证。

入库的 Workbench 固定装置绑定当前规划器、契约、policy 验证器、执行控制器、机械臂 Xacro、控制器配置，以及一条已记录的五测试集成结果。其发布状态是 `tested` 而非 `approved`，因此当前 RLSOK 运行时会在考虑派发之前以 `release_not_approved` 阻止它。它还刻意省略执行配置绑定，这在未来任何独立审批之后仍是第二个阻塞项。
