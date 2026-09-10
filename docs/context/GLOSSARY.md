# 术语表

| 术语 | 含义 |
|---|---|
| Observation | 原始或经处理的传感器报告；不属于 WorldState 事实。 |
| WorldState | 由有序事件推导出的确定性状态。 |
| TaskGraph | 类型化的高层步骤；绝不是关节指令。 |
| SemanticAction | 经验证的动作请求，例如 `grasp` 或 `place`。 |
| ActionResult | 来自 Motion / Robot Runtime 的执行结果。 |
| VerificationResult | 有证据支撑的 World Model 决策。 |
| Scenario Manifest | 带版本的场景、种子、任务与故障配置。 |
| Oracle | 仿真器的真值；不计入传感器指标。 |
