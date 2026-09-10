# Workbench-1 仓库规则

## 始终遵守

- 一次只处理一个 Issue 和一个有界模块。
- 修改生产者或消费者之前，先阅读相关的 JSON Schema 与示例。
- 每个确定性行为变更都要新增或更新测试。
- 除非人类 Owner 明确批准，否则 `robot/control/` 与 `firmware/` 不参与 AI 写入任务。
- 没有命令、测试结果与证据引用，绝不声称任务完成。

## 评审边界

- `interfaces/` 变更需要三位独立人类评审批准。受影响的生产者与每一位消费者必须在合并前收到通知。
- 修改 `interfaces/` 中 schema 的 PR 必须同时更新 `libs/contracts/` 中对应的 Pydantic 模型，且 `make contract` 通过。此前 schema 与模型正是因拆分到两个 PR 而产生漂移。
- `sim/` 变更需要仿真验证；机器人运动学/控制变更需要运动验证。
- `services/world_model/` 定义状态语义与验证；它不定义 UI 或机器人控制。
- `services/agent_runtime/` 定义规划与类型化工具；它不写入 WorldState 事实。
- 构建、启动、CI 与集成配置变更需要集成评审。

## AI 任务规则

AI 写入工作需要一份任务包（Task Packet），包含允许路径、测试、证据与停止条件。机器可读示例参见 `docs/task_packets/example-001-world-reducer.json`。

## 必做检查

```bash
make test
make contract
make scenario-check
make context-check
```
