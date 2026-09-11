# Workbench-1 仓库规则

## 项目与导航

VORA 是产品名，Workbench 是工程仓库与证据优先运行时。主链路为目标、受限规划、语义动作、执行结果、World Model 验证与回放；命令被接受不代表任务完成。

- `interfaces/json_schema/` 与 `interfaces/examples/`：跨模块契约及示例；对应 Python 模型位于 `libs/contracts/workbench_contracts/`。
- `services/world_model/`：状态归约、验证、事件存储与回放；`services/agent_runtime/`：受限规划和类型化工具。
- `services/perception/`：感知观测；`services/backend/` 与 `apps/dashboard/`：只读 API、状态与证据展示。
- `libs/kernel/`、`libs/hardware/`、`libs/application/`：内核、硬件适配与应用运行时；`tasks/`：具体任务模块。
- `sim/`：场景、固定装置与仿真工具；`tests/` 和 `libs/kernel/tests/`：默认 pytest 测试入口。
- `robot/`、`firmware/`、`bsp/`、`hardware/`：机器人、MCU、板级支持与硬件工程资料，遵守下述写入和验证边界。
- `tools/scripts/`：校验与离线运行脚本；`docs/context/CONTEXT_MANIFEST.md`：上下文加载入口；`docs/architecture/system.md`：系统职责与证据链。

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

开始任务时确认当前目录、分支及 `git status --short`，保留已有改动。按 `docs/context/CONTEXT_MANIFEST.md` 加载上下文，再阅读所属模块、相关契约、示例与测试。

任务包格式见 `tools/schemas/task-packet-v1.schema.json`。使用 `make task-check PACKET=docs/task_packets/<任务包>.json` 校验；格式通过不代表人工批准，也不代表测试已执行。需要核对 Git 可见改动范围时，使用 `python3 tools/scripts/check_task_packet.py --base <基准提交> docs/task_packets/<任务包>.json`；该检查也会包含工作区已有的未跟踪文件，应先辨明归属。

## 环境与开发命令

项目要求 Python 3.12 或更高版本（见 `pyproject.toml`），建议在虚拟环境中安装开发依赖：

```bash
python3 -m venv .venv
. .venv/bin/activate
python -m pip install -e ".[dev]"
```

Makefile 默认使用 `python3`，可通过 `make PYTHON=<解释器路径> <目标>` 指定环境。

- `python3 -m pytest tests/unit/<测试文件>.py -v`：针对当前模块验证。
- `make lint`：Ruff 静态检查与格式检查；`make fmt` 会改写整个仓库，使用前确认任务允许范围。
- `make demo-scripted`、`make demo-offline`：脚本与离线运行；产物不能充当真实硬件成功证据。
- `make check`：聚合 lint、测试、契约、场景、golden set、上下文检查与两项离线 demo，具体目标以 Makefile 为准。

## 必做检查

```bash
make test
make contract
make scenario-check
make context-check
```

执行结果须包含确切命令、通过/失败/未执行状态及证据路径。`scenario-check` 校验场景数据；脚本固定装置、Gazebo 仿真与真实运动是不同的验证层级。缺少依赖或运行环境时应明确报告，不能声称检查通过。

## 提交与评审

遵循 `CONTRIBUTING.md`：编码前落实带 Owner 与验收标准的 Ready Issue；新分支使用 `feat/`、`fix/`、`test/`、`docs/` 或 `chore/` 加 Issue 与短名称。提交使用 Conventional Commits 并附 DCO 签署。每个 PR 只处理一件事，附命令、结果和证据，并邀请模块 Owner 与受影响消费者评审；严禁强推或直接向受保护的 `main` 提交。
