# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## 项目定位

Workbench Desk Robot（产品名 VORA）是一个**证据优先**的具身智能体运行时基础。核心信条：
「先验证，再说完成」——命令被接受不等于任务完成。任何完成声明必须由 World Model 验证器
基于证据得出 `confirmed` / `refuted` / `insufficient_evidence` 三值之一。

仓库同时包含纯软件运行时（已测试）和硬件/仿真边界（多数为契约与骨架，尚未实现）。
不要把骨架当成已实现能力；`docs/architecture/system.md` 与 `sim/README.md` 记录了当前证据边界。

## 常用命令

```bash
make bootstrap        # pip install -e ".[dev]"，Python 3.12
make check            # CI 全量：lint + test + contract + scenario + golden + context + 两个 demo
make fmt              # ruff check --fix + ruff format（本地修复，CI 只做 --check）
make lint             # ruff check + ruff format --check
make test             # pytest -v
```

CI 把关的独立检查（`make check` 已包含，也可单跑）：

```bash
make contract         # schema ↔ 示例 ↔ Pydantic 模型三方一致性（含 Draft-2020-12 全量校验）
make scenario-check   # 校验 sim/scenarios/ 下的场景清单
make golden-check     # 校验 evaluation/golden-set-*.json 中每条任务都可被模板规划器规划
make context-check    # 断言 AGENTS.md 与 docs/context/ 下 5 个必需文件存在
make demo-scripted    # 确定性脚本化演示
make demo-offline     # 离线模板规划器端到端（无网络、无 GPU）
make docs             # mkdocs build --strict
python tools/scripts/check_task_packet.py --all   # 校验 docs/task_packets/*.json
```

### 单个测试

```bash
python -m pytest tests/unit/test_agent_runtime.py -v
python -m pytest tests/unit/test_agent_runtime.py::TestPlanner::test_place_plan -v
python -m pytest -m integration            # 标记：integration / regression / benchmark
```

`testpaths` 是 `tests` 与 `libs/kernel/tests`。`tests/hardware/` 里是 KiCad/BOM/机械等硬件
制品的校验测试，同样跑在默认 pytest 里。

### 其它子系统（各自独立工具链）

```bash
make -C firmware/mcu host test-host qemu verify-no-fp size-check   # RISC-V 安全 MCU，需 gcc-riscv64-unknown-elf + qemu
make -C kernel/wbcan all load test unload                          # wbcan 内核模块，需 root 与内核头文件
make container-check                                               # docker compose 全栈冒烟
make sim-doctor / sim-list / sim-run                               # 仿真 CLI；未配置 Gazebo 时 sim-run 以 NOT_EXECUTED(2) 退出而非伪造通过
make dashboard                                                     # 只读看板 http://127.0.0.1:8080
```

`robot/control/` 是 ROS 2 Jazzy `ament_python` 包（`workbench_motion`），用 colcon + uv 构建，
不在根 pyproject 里。参见 `make container-colcon-build` / `container-colcon-test`。

## 架构

### 主数据流

```text
Simulation camera -> Perception Observation -> World Model -> Agent Runtime
                                                  ^                 |
                                                  |                 v
Dashboard <- Read-only HTTP API <- Event Store <- ActionResult <- Motion / Virtual MCU
```

只追加事件流 + 确定性 reducer 是核心机制：`WorldState` 完全由有序 `WorldEvent` 重建，
状态有规范化字节表示与 SHA-256 哈希（`canonical_world_state_bytes`），因此回放可校验。

### 职责边界（不可越界，这是本仓库的设计骨架）

| 包 | 允许 | 禁止 |
| --- | --- | --- |
| `services/perception/` | 产出 `Observation`（带 frame、置信度、来源） | 写入 WorldState |
| `services/world_model/` | reducer、验证器、事件库、回放读模型；**唯一**发出 `VerificationResult` 的地方 | 定义 UI 或机器人控制 |
| `services/agent_runtime/` | 目标 → 类型化 `TaskGraph` → `SemanticAction` | 写入 WorldState 事实；发出关节位置、速度、急停决策或完成声明 |
| `services/backend/` | `/healthz`、`/readyz`、有序事件读投影 | 派生第二份 WorldState；接受控制写入（`POST/PUT/PATCH/DELETE` 一律 `405 read_only`） |
| `robot/control/` | 接收已验证动作，产出带证据引用的 `ActionResult` | 接受来自 Agent Runtime 的原始关节命令 |

模型（LLM）的输出边界见 `docs/context/MODEL_POLICY.yaml`：本地 Ollama 提供方只返回一个
五路路由决策，随后由受信的确定性构建器生成 `TaskGraph`。模型响应永远不能成为关节、速度、
固件、急停或完成命令。

### 契约层是双写的

`interfaces/json_schema/*.schema.json` 与 `libs/contracts/workbench_contracts/models.py`
中的 Pydantic 模型必须同步，`interfaces/examples/` 下每个 schema 都要有示例。
`make contract` 会同时校验 schema 注册、示例合法性、Pydantic 双向兼容与模板规划器往返。
历史上 schema 与模型因拆成两个 PR 而产生过漂移——**同一个 PR 内改完三者**。

`libs/kernel/workbench/kernel/schema_compiler.py` 把 schema 子集编译为 Python/TypeScript
模型，只支持受限关键字集合（见文件顶部的 `ROOT_KEYWORDS` / `PROPERTY_KEYWORDS`）；
往 schema 里加新关键字前先确认编译器支持。

### 失败即拒绝的授权链

`ToolRegistry`（`tool_registry.py` + `tool_schemas.py`）是合法动作与参数形态的唯一白名单。
`PolicyValidator` 消费其结果，额外强制两条独立规则：嵌套载荷中禁止原始控制标识符；
高影响动作必须按确切 `action_id` 完成确认。验证器只产出不可变授权证据，不派发动作。

### 工具脚本的 sys.path 引导

`tools/scripts/_paths.py` 的 `enable_local_packages()` 在 import 前把 `libs/` 与 `services/`
路径插入 `sys.path`，让脚本免安装即可运行。这是 `E402`（import 不在文件顶部）在
ruff 配置里被全局豁免的原因——不要为了「修好」它而重排这些脚本的 import。

## 工作约定

`AGENTS.md` 是仓库的强制规则，开始改动前读它。其中对 AI 写入任务最关键的几条：

- 一次只处理一个 Issue 和一个有界模块。
- 改生产者或消费者之前，先读对应的 JSON Schema 与示例。
- 每个确定性行为变更都要新增或更新测试。
- **`robot/control/` 与 `firmware/` 不接受 AI 写入**，除非人类 Owner 明确批准。
- `interfaces/` 变更需要三位独立人类评审。
- 没有命令、测试结果与证据引用，绝不声称任务完成。

AI 写入工作需要一份 Task Packet（`docs/task_packets/*.json`），声明 `allowed_paths`、
`read_only_paths`、`forbidden`、`commands`、`evidence`、`stop_conditions`。
模板见 `docs/task_packets/example-001-world-reducer.json`，校验用
`python tools/scripts/check_task_packet.py <packet>`。CI 会对 PR diff 交叉校验路径边界。

### 代码风格

ruff，line-length 120，规则集刻意保持小而高信号（`F,E,I,B,BLE,UP,RUF`）——目的是抓真实缺陷，
不强加风格偏好。注释与 docstring 用中文（全角标点已加入 `allowed-confusables` 白名单）。
提交信息用 Conventional Commits + DCO `Signed-off-by`；分支命名 `feat/<issue>-<short-name>`。

### 不能当作证据的东西

- `apps/dashboard/data/` 是离线 UI/API 测试的固定装置，永不作为物理发布证据。
- 截图只能证明 UI 外观，不能证明任务完成或安全。
- 脚本化运行产物标记为 `SCRIPTED_FIXTURE`。
- Gazebo 未执行必须以 `NOT_EXECUTED` 显式退出（CI 断言退出码为 2 且输出中无 `placeholder`/`VTCR`）。
