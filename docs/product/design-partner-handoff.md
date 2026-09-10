# Design Partner 交接与物理证据边界

状态：为 Issue #318 提出的交接政策。本文档定义产品试用与工程或安全验收之间的边界；它不授予场景、伙伴、模型、看板或适配器对机器人的控制权。它是一份文档契约，不是新的公共接口，也不是可信运行时与物理安全规程的替代品。

## 1. 决策词汇表

每项交接决策都对应一条具名主张、一个场景版本和一个证据等级。它必须指明一名负责人、一个后续动作和一个评审日期。

| 决策 | 含义 | 必需的后续动作 |
|---|---|---|
| `continue` | 所声明的主张得到其当前证据闸门的支持，且下一步受限动作可以安全排期。 | 记录下一个环境、负责人和证据产物。 |
| `change` | 用户问题有价值，但任务、动作、成功条件或证据请求模糊或超出边界。 | 修订交接并重新执行适用的就绪检查。 |
| `defer` | 范围可接受，但依赖项、负责人批准、环境或合格证据来源缺失。 | 保持主张开放，并记录阻塞输入与日期。 |
| `reject` | 请求将越过安全、隐私、契约或权限边界，或证据规则无法做到真实。 | 保留理由，并在存在可行方案时提供一个受限的语义替代方案。 |

`continue` 本身从不意味着“机器人是安全的”或“任务在物理上已完成”。产品决策可以在物理或安全主张仍为 `insufficient_evidence`、`not_executed` 或 `blocked` 时继续软件试用。

## 2. 语义动作契约

场景层描述用户任务。它不描述控制器应当如何运动。当前公开的语义动作集合为：

`observe`、`grasp`、`place`、`ask_confirm`、`express`、`stop` 和 `navigate`。

场景只能在动作已注册且版本兼容时引用该动作。它可以提供：

- 场景 ID 与版本、受限目标，以及不透明的 `target_id`；
- 已注册的语义动作名称和一组封闭的类型化策略选择器；
- 所需的适配器能力名称、证据策略与验证器引用，以及受限恢复策略引用；
- 预期的起始状态和可观测的成功条件。

这些名称仅描述交接词汇表。它们不是向当前公开 Schema 添加字段的指令；任何新的公开动作或清单字段仍需经过常规的生产者/消费者批准流程。

以下内容永远不是场景字段或 Design Partner 输入：

- 关节位置、笛卡尔位姿、轨迹、速度、加速度、力矩、力/扭矩限制、控制器目标、规划器内部实现或 TF 命令；
- CAN 仲裁 ID、原始 CAN 帧、重试/序列内部实现、设备句柄、引脚/IRQ/DMA 分配、固件命令或供应商寄存器值；
- E-stop、安全使能、看门狗、制动器、电源、复位或紧急恢复写入；
- 凭据、具有写入权限的网络端点、任意可执行参数或无界的 JSON 属性包。

适配器能力是一个名称和一个带版本的归属引用，而不是暴露其实现参数的许可。适配器根据负责人批准的配置解析该引用，并在目标、版本、能力或策略未知时失败即拒绝。Design Partner 可以描述任务和可观测目标；它不能在场景清单中就控制器目标或安全限制进行协商。

### 允许请求示例

以下是交接的伪代码，而不是新的公开 JSON Schema：

```yaml
scenario_id: partner-parcel-001
scenario_version: "1"
goal:
  entity: parcel-17
  observed_condition: placed_in_slot
actions:
  - type: observe
    target_id: parcel-17
  - type: grasp
    target_id: parcel-17
    policy: standard_grasp_v1
evidence_policy: parcel-placement-observation-v1
verifier: parcel-slot-verifier-v1
recovery_policy: bounded-observation-retry-v1
```

上述策略名称解析到受控定义。它们不允许调用方添加位姿、力、速度、关节、CAN 或控制器字段。

## 3. 运动适配器结果边界

运动适配器接收一个经过验证的语义动作和一个已批准的上下文。它返回带类型的 `ActionResult` 和独立的健康/故障证据；它不写入 `WorldState`，也不决定用户目标是否已被观测。

### ActionResult 最低限度解读

现有结果契约包含以下区分：

| 字段/组 | 它证明了什么 | 它不能证明什么 |
|---|---|---|
| `action_id`、`run_id`、时间戳与时钟 | 处理的是哪个请求与时间区间。 | 物理现场已处于期望状态。 |
| `outcome` | 适配器的受限执行结果，如 `completed`、`failed`、`timeout` 或 `safe_stop`。 | 任务级成功或用户价值。 |
| `dispatch_state` | 请求是否已离开可信主机边界。 | 控制器已正确执行了它。 |
| `device_state` | 适配器持有设备确认状态时的该状态。 | 感知已观测到预期结果。 |
| `error_code`、`error_reason`、重试计数 | 稳定的诊断信息和受限的重试历史。 | 无限重试或绕过安全的许可。 |
| `evidence_refs` | 可在何处找到支撑性的原始或派生记录。 | 被引用验证器尚未接受的记录的有效性。 |

特别地，`ActionOutcome.COMPLETED` 和 `DeviceState.CONFIRMED` 是执行/确认事实。它们不是已被观测的 WorldState 事实。产品任务验收除了 ActionResult 之外，还要求相关的观测、来源、新鲜度/冲突检查以及世界模型验证器结果。缺失或矛盾的观测保持为 `insufficient_evidence`；它永远不会由一次成功的动作补全。

### 健康与故障记录

对于产品交接，运动适配器必须暴露（或引用）一份受限的、只读的健康/故障记录。这可以是现有的由负责人控制的产物；它不要求新的公开 schema。它包含：

- 适配器/来源标识、接口以及配置/提交哈希；
- 就绪与生命周期状态、队列深度/容量、丢弃/超时计数，以及用于该观测的单调时间戳/时钟；
- 稳定的故障码、严重级别、锁存/非锁存状态、首次/末次观测时间、受影响的能力，以及是否允许受限恢复；
- 原始采集或证据引用，不含原始控制器句柄或伙伴私有数据。

健康状态不是安全许可。健康的适配器不能清除 E-stop、使能驱动器，或把未经验证的 ActionResult 变成已验证的任务。

## 4. 交接检查清单

交接负责人将本表复制到私有试用记录中，并只向仓库提交匿名化 ID 和不透明证据引用。

| 负责人 | 试用前所需输入 | 试用后所需输出 | 无权授权 |
|---|---|---|---|
| Product / Project Owner | 问题卡、伙伴引用、用户任务、已声明的主张、成功条件、测试窗口、隐私/同意选择 | `continue`/`change`/`defer`/`reject`、产品反馈记录、下一位负责人/日期 | 运动参数、E-stop 复位、发布或物理安全主张 |
| Motion Owner | 带版本的语义动作、已知目标/能力、已批准的上下文与预检结果 | ActionResult、适配器健康/故障记录、执行/证据引用、停止结果 | 原始场景控制器目标、独立安全放行、观测到的任务事实 |
| MCU / Safety Owner | 固件/配置哈希、节点/生命周期状态、看门狗与安全链就绪状态、已批准的复位规程 | ACK/故障/心跳证据、安全抑制状态、复位/停止处置 | 产品完成、自动清除锁存的 E-stop、任意场景字段 |
| Runtime / Integration Owner | 运行时版本、策略/验证器版本、队列/生命周期限制、事件/证据接收端与只读暴露配置 | 关联的运行/事件 ID、取消/关停结果、来源与回放引用 | 硬件 E-stop 权限、原始设备写入访问、验证器覆盖 |
| Site / Partner Operator | 环境访问权限、经过培训的操作员、已知起始状态、安全简报、设备与校准引用 | 操作员记录、观测到的任务反馈、原始证据交接、事件/中止报告 | 更改产品契约或批准不安全的复位 |
| Evidence / QA Reviewer | 已声明的证据等级、验收规则、产物 schema 与哈希规程 | 独立证据评审、状态分类、差距清单与处置 | 将较低证据等级提升为物理或发布证据 |

最低交接输入包括：不透明的伙伴 ID、场景/版本、具名主张、环境等级、负责人矩阵、起始状态、成功与失败条件、预检/中止规则、隐私决策，以及一名证据负责人。当其中任何一项被默然假定为已有时，试用不得开始。

## 5. 证据阶梯

证据等级彼此独立。更高等级要求自身的合格产物；通过较低等级不是晋升凭证。

| 等级 | 可支持 | 最低证据要求 | 明确限制 |
|---|---|---|---|
| `software` | 契约、解析器、策略、生命周期和确定性单元行为 | 带版本的源码/配置、确定性测试输出和可复现的命令 | 不含用户试用、物理、执行器、线缆或安全主张 |
| `scripted_fixture` | 使用受控固定装置的产品流程与故障路由行为 | 固定装置/清单哈希、运行 ID、回放/验证器输出、故障语料和隐私安全记录 | 默认 `release_eligible: false`；不是 Gazebo 或物理证据 |
| `gazebo` | 已声明的仿真器任务行为与仿真观测 | 仿真器/世界/插件/配置哈希、种子/时钟、原始运行包、验证器输出和明确的仿真器状态 | 不是物理 CAN、执行器、E-stop、校准或现场验收 |
| `physical` | 仅限明确声明的硬件/现场主张 | 序列化单元与修订版本、已批准的配置哈希、经校准的仪器、经过培训的操作员/评审人、安全预检、原始采集哈希和可重复的规程 | 不会自动证明不同的任务、设备、现场或发布主张 |

证据阶梯按主张生效，而不是按对话生效。Design Partner 的口头认可、访谈或视频可以是有用的产品反馈，但它们本身都不是执行证据或物理证据，也不能证明物理任务完成。每当证据缺失、过时、矛盾或来自错误等级时，交接记录必须保留较弱的状态：

- `confirmed`：该主张所声明的证据规则已通过；
- `failed`：所声明的运行或任务结果未达成；
- `refuted`：所声明的产品假设或主张被推翻；
- `insufficient_evidence`：该运行无法确立该主张；
- `not_executed`：所需的运行或环境未发生；
- `blocked`：某个前置条件或负责人闸门阻止执行。

对于 `physical`，现场规程还必须在通电前明确独立的 E-stop/安全使能权限、电源与运动抑制状态、校准引用、中止标准以及原始采集位置/哈希。如果任何必需项缺失，物理结果保持为 `not_executed` 或 `blocked`。

## 6. 中止、停止、恢复与确认权限

这些控制被有意地分开：

| 控制 | 权限 | 正常行为 | 失败行为 |
|---|---|---|---|
| `safe_stop` | 由可信运行时请求；Motion/MCU-Safety 与硬件链执行停止 | 取消后续动作分发并驱动受限停止路径；记录关联关系与结果 | 任何超时、被拒绝的停止或缺失的确认都属于故障，并保持运动处于抑制状态 |
| `abort` | 可信运行时/编排器拥有运行决策；现场操作员可以请求中止 | 结束运行、阻止新的语义分发、保留事件与证据，并在运动可能处于活动状态时请求 `safe_stop` | 绝不报告完成；让运行保持失败、已停止或证据不足状态 |
| E-stop | 物理现场操作员与独立的 MCU-Safety/安全回路 | 通过带外链路切断/抑制危险能量 | 软件、DDS、看板或 Design Partner 无法覆盖或静默复位它 |
| 恢复 | 运行时只能选择有限的、经策略批准的恢复；Motion/MCU/Safety 负责人控制复位与重新使能条件 | 每次尝试后都要求新的、来源有效的证据 | 锁存的安全故障与不确定状态需要负责人人工处置；不得自动使能 |
| 手动确认 | 经授权的人员/现场负责人，附身份引用与时间记录 | 在已批准的规程内确认操作员决策或就绪观测 | 模型回答、伙伴偏好或截图不能替代所要求的安全证据 |

因此，`safe_stop` 是可信请求边界，而不是场景自有的实现。即使运行时镜像了它们的状态，E-stop 与安全使能仍然保持独立。恢复不能在没有新证据的情况下把失败的验证变为成功，任何手动确认也不能免除所要求的 Safety Owner 闸门。

## 7. 产品验收与运动/安全验收

在交接中将这些记录为单独的行：

| 主张 | 产品验收要求 | 工程/安全验收要求 |
|---|---|---|
| 用户价值 | 指定的评估者是否执行了受限任务并理解了结果？ | 任务是否通过已批准的接口运行并被可复现地记录？ |
| 动作行为 | 交互是否可理解，且所声明的动作结果是否有用？ | 适配器是否执行了策略、限制、关联、超时与清理？ |
| 已观测目标 | 所声明的目标是否可度量并得到新证据支持？ | 规范的观测/验证器路径是否确立或推翻了它？ |
| 安全 | 操作员是否收到了清晰的停止/中止指示？ | 独立的安全链、看门狗、E-stop 与复位规程是否通过了各自的闸门？ |
| 发布 | 该限定范围的产品主张是否值得继续？ | 所有必需的负责人批准与合格的发布产物是否齐备？ |

产品侧可以为脚本化或仿真的学习循环选择 `continue`，而工程侧将物理主张记录为 `not_executed`。不得将后者发布为产品成功。

## 8. 被拒绝的请求示例

以下请求必须由交接负责人和 Safety Owner 标记为 `reject`：

> “在伙伴的场景清单中加入 `joint_angles`、`velocity`、`torque_limit`、`can_frame` 和 `emergency_stop: false`，以便操作员可以直接从看板上调校任务。”

它要求场景和 UI 变成控制器与安全写入者，绕过 Motion/MCU/Safety 的归属，并把看板上的一个设置包装成 E-stop 决策。受限的替代方案是：指定一个已注册的语义动作和不透明目标、把限制保留在负责人控制的配置中、展示经过验证的只读健康/证据，并在适当时通过可信运行时请求 `safe_stop`。即使这会让演示更易于操作，Safety Owner 也必须拒绝原始请求。

## 9. 可复制的交接记录

```text
Handoff ID / scenario ID and version:
Partner reference (opaque) / organization type:
Named claim and evidence class:
Decision: continue | change | defer | reject
Decision owner / Motion owner / MCU-Safety owner / site owner:
Test window and known starting state:
Allowed semantic actions and adapter capabilities:
Forbidden fields/authority confirmed:
Product acceptance condition:
Motion acceptance inputs and outputs:
MCU/Safety preflight, stop, and reset inputs/outputs:
Runtime lifecycle, queue, cancellation, and provenance evidence:
Required evidence artifacts and hashes:
Privacy/consent/retention reference:
Abort conditions and immediate stop procedure:
Missing evidence or blockers:
Next action / owner / due date / review date:
Release eligible: false (change only with the applicable release gate)
```

使用 [Design Partner 场景记录](design-partner-scenario-template.md) 记录基础试用信息，使用本文档记录权限、证据与交接决策。通用记录有意保持轻量；本政策提供了在排期真实任务之前所需的更严格边界。

## 10. 后续步骤与决策记录

交接完成后，按以下顺序执行。每一行都需要一名具名负责人和一条带日期的记录；日期为空是阻塞项，而不是继续的许可。

| 步骤 | 负责人 | 必需动作 | 输出/状态 |
|---|---|---|---|
| 1. 准备 | Product / Project Owner | 附上问题卡、不透明伙伴引用、具名主张、场景/版本和建议的环境。 | 含隐私与同意决策的交接记录。 |
| 2. 评审 | Motion、MCU/Safety、Runtime/Integration 与现场负责人 | 确认语义动作、适配器能力、预检、停止/中止边界与失败矩阵。 | `continue`、`change`、`defer` 或 `reject`；未解决的分歧保持 `defer`/`reject`。 |
| 3. 排期 | Product Owner 与现场负责人 | 设定测试窗口、已知起始状态、操作员、设备、校准引用与原始证据目的地。 | 针对所声明证据等级的试用启动批准。 |
| 4. 执行 | 现场操作员加可信运行时与相关适配器负责人 | 只执行已批准的任务；在任何已声明条件下停止或中止，并保留运行。 | 运行 ID、ActionResult/健康记录、观测与原始证据引用。 |
| 5. 评审证据 | Evidence/QA Reviewer 加各领域负责人 | 检查哈希、来源、新鲜度、冲突、安全记录与特定等级的验收规则。 | 主张状态；差距保持为 `insufficient_evidence`、`not_executed` 或 `blocked`。 |
| 6. 决定下一步 | Product / Project Owner | 记录产品决策和下一位负责人/动作/日期，且不提升证据等级。 | 后续 Issue/PR，或明确的停止/推迟决策。 |

因此，最低限度的后续步骤记录为：`owner`、`action`、`due date`、`review date`、`evidence reference` 和 `status`。一句口头的“看起来不错”不是已完成的交接。
