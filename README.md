# Workbench Desk Robot

> **先验证，再说完成。**
>
> 面向移动家务机器人的证据优先基础：受限动作、可回放事件，以及一个能够明确说出
> **confirmed（已确认）**、**refuted（未满足）**或
> **insufficient_evidence（证据不足）**的验证器。

[![Python](https://img.shields.io/badge/Python-3.12-3776AB?logo=python&logoColor=white)](pyproject.toml)
[![License](https://img.shields.io/badge/license-Apache--2.0-2ea44f)](LICENSE)
[![Release](https://img.shields.io/github/v/release/Quchaosheng/workbench-desk-robot?display_name=tag)](https://github.com/Quchaosheng/workbench-desk-robot/releases/latest)
[![CI](https://github.com/Quchaosheng/workbench-desk-robot/actions/workflows/ci.yml/badge.svg)](https://github.com/Quchaosheng/workbench-desk-robot/actions)

![VORA 家务机器人](docs/assets/workbench-home-robot-market-v18.png)

<p align="center"><img src="docs/assets/vora-logo.svg" alt="VORA" width="360"></p>

**产品名：** VORA Home Robot<br>
**工程仓库 / 运行时：** Workbench Desk Robot（`workbench-desk-robot`）

VORA 是面向用户的产品品牌；Workbench 继续作为工程仓库和证据优先运行时的名称。
VORA 标志采用开放角度与偏心轨道，在保持识别度的同时，不把品牌身份绑定到某一种机器人用途上。

[简体中文镜像](README.zh-CN.md) · [可交互 3D 视图](docs/assets/premium-product-render.html)

## 为什么做 Workbench？

很多机器人演示把「命令已接受」当作「任务已完成」。Workbench 把证据放回主链路：

```text
goal -> bounded planner -> semantic action -> trusted executor
                                      \-> event store -> verifier -> replay/dashboard
```

| 层 | 职责 |
| --- | --- |
| Intent（意图） | 从一个小而类型化的动作词汇表中选择 |
| Execution（执行） | 通过受信运行时代码派发 |
| Verification（验证） | 检查动作后的证据；绝不臆断成功 |
| Replay（回放） | 从只追加的事件流重建状态 |

## 快速开始

要求：Python 3.12。离线运行时不要求 GPU。

```bash
git clone https://github.com/Quchaosheng/workbench-desk-robot.git
cd workbench-desk-robot
python -m pip install -e ".[dev]"
python tools/scripts/sim_cli.py doctor
python tools/scripts/sim_cli.py run normal-001 --runner scripted --output-dir runs/demo
```

运行完整可移植检查：

```bash
python -m pytest -q
python -m ruff check .
```

脚本化运行器会写出一个可回放的产物（manifest、scene、events、logs、metadata
以及 SHA-256 校验和），标记为 `SCRIPTED_FIXTURE`。

## 包含内容

- 证据优先验证，结论为 `confirmed`、`refuted`、`insufficient_evidence` 三值之一。
- 严格的 JSON Schema 与配套的 Pydantic 契约。
- 只追加的 SQLite 事件存储，支持完整性校验的回放。
- 对受限语义工具执行 fail-closed（失败即拒绝）策略校验。
- 只读看板与确定性的仿真固定装置（fixture）。
- MCU、CAN、Motion 与 BSP 边界的纯软件基础。

## 可选：OmniLink 知识层

[OmniLink AI](https://github.com/vivekmaru/omnilink-ai) 是一个独立服务，用于搜索维护
笔记、ADR、issue 以及 Workbench 运行摘要。它不是规划器、执行器、验证器，也不属于
机器人控制的依赖项。

```bash
git clone https://github.com/vivekmaru/omnilink-ai.git
cd omnilink-ai
npm install
npm run dev                 # 通常位于 http://127.0.0.1:3000
```

Workbench 使用 [`integrations/omnilink/`](integrations/omnilink/) 中的标准库适配器：

```python
from integrations.omnilink import OmniLinkClient

client = OmniLinkClient("http://127.0.0.1:3000")
results = client.search("gripper calibration")
answer = client.ask("Which calibration notes mention the gripper?")
```

只有受限的运行摘要可以导出。原始 JSONL、`TaskGraph`、`SemanticAction`、动作结果、
相机数据与安全状态一律留在 Workbench 内。请捕获 `OmniLinkError`，使不可用的知识
服务不会阻塞离线运行。部署与安全要求参见
[集成指南](integrations/omnilink/README.md)。

## 诚实的状态说明

离线运行时与软件边界已通过测试。端到端 Gazebo 世界、真实感知、语义运动执行以及
实体硬件证据尚不属于发布声明。当前证据边界参见 [`docs/architecture/`](docs/architecture/)
与 [`sim/README.md`](sim/README.md)。

## 文档

- [用户指南](docs/user-guide/index.md)
- [产品证据层](docs/product/README.md)
- [系统架构](docs/architecture/system.md)
- [仿真边界](sim/README.md)
- [运动基础](robot/control/README.md)
- [安全 MCU](firmware/mcu/README.md)
- [机器人 BSP](bsp/README.md)
- [部署](docs/deployment/multi-host.md)
- [安全策略](SECURITY.md)

## 许可证

Apache-2.0。参见 [LICENSE](LICENSE)。
