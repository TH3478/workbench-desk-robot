# 产品证据层

本目录是 Workbench 的产品管理与用户证据层。它补充工程证据链；不替代共享运行时、场景注册表（Scenario Registry）、事件库（Event Store）、验证器、回放或发布闸门。

## 运营原则

```text
user problem -> real task -> scenario contract -> implementation
             -> evidence -> verification -> product decision
```

每个被提出的场景都应回答两个独立的问题：

1. 这是一个真实且重复出现的用户问题吗？
2. 运行时能否在不混淆动作主张与已观测状态的情况下证明任务结果？

## 文档

- [产品简报](product-brief.md)：当前假设、目标用户、待完成工作、非目标与验证计划。
- [90 天执行](90-day-plan.md)：源自产品经理计划、以证据为闸门的运营节奏。
- [问题卡模板](problem-card-template.md)：把访谈转化为可追溯的问题证据。
- [Design Partner 场景模板](design-partner-scenario-template.md)：定义受限的真实任务协作。
- [Design Partner 交接边界](design-partner-handoff.md)：在试用前分离产品验收、Motion/MCU/Safety 权限与证据等级。
- [反馈记录模板](feedback-record-template.md)：让外部安装与试用失败可复现。
- [指标与决策日志](metrics-and-decision-log.md)：区分活动指标与产品及证据结果。

## 证据流转

使用能保留决策轨迹的最轻量产物：

1. **问题卡**记录用户问题、原话、近期事例、频率、影响和证伪条件。
2. **Design Partner 场景**记录一项受限的真实任务、职责、安全边界和证据约定。
3. **交接边界**在排期试用前记录允许的语义接口、负责人输入/输出、停止权限和证据等级。
4. **反馈记录**记录可复现的安装、试用或任务结果，含版本与运行引用。
5. **工程产物**记录场景注册表条目、事件库运行、验证器结果与回放哈希。
6. **决策日志**记录是 `continue`、`change`、`defer` 还是 `reject`，并附上此前的证据链接。

不要从一次对话直接跳到功能 Issue。只有在问题卡、成功条件、非目标和证据负责人明确之后，功能才算就绪。只有在适用的工程与证据闸门通过之后，场景才与发布相关。

## 共享状态词汇

| 状态 | 含义 | 它不能意味着什么 |
|---|---|---|
| `hypothesis` | 团队认为某个问题或能力可能重要 | 用户验证 |
| `observed` | 存在一个具体的用户或运行事例 | 重复的需求或普遍成功 |
| `repeated` | 独立证据显示同一模式 | 物理安全或发布就绪 |
| `confirmed` | 该主张所声明的证据规则得到满足 | 更强的证据等级，例如物理验证 |
| `insufficient_evidence` | 证据缺失、过时或矛盾 | 凭假设认定的成功或失败 |
| `failed` / `refuted` | 所声明的结果或假设未达成 | 未经决策的永久产品否决 |
| `not_executed` / `blocked` | 测试未运行或无法完成 | 成功结果 |

同一个词在产品记录、运行产物、看板视图和发布报告中必须保持相同含义。存疑时保留较弱的状态。

## 证据与隐私边界

仓库中只能包含匿名化的参与者 ID、组织类型、场景 ID、脱敏的问题陈述和证据引用。不得提交姓名、邮箱地址、电话号码、预算、私有日志、原始视频、访问令牌或可识别客户身份的材料。把这些保存在受访问控制的私有系统中，只链接不透明的证据引用。

用户陈述、产品假设、脚本化固定装置、Gazebo 证据与物理证据是不同证据等级。它们中的任何一个都不得悄然提升另一等级。特别是，除非适用的发布闸门另有说明，脚本化固定装置保持 `release_eligible: false`。用户反馈可以促成产品决策，但除非存在适用的运行与验证产物，它不是执行证据或物理证据。

## 归属边界

- 产品负责问题定义、用户证据、优先级与验收意图。
- 运行时与架构负责人决定共享契约与安全边界。
- 任务/场景负责人实现领域规则，且不重复共享的验证或回放。
- 运动、感知、导航、MCU 与硬件负责人实现适配器和物理验证。
- 人类 Project Owner 负责 Go/No-Go、范围、发布与对外主张。

## 相关执行 Issue

- 多场景 Epic：[#309](https://github.com/Quchaosheng/workbench-desk-robot/issues/309)
- 契约与归属：[#308](https://github.com/Quchaosheng/workbench-desk-robot/issues/308)
- Definition of Ready：[#310](https://github.com/Quchaosheng/workbench-desk-robot/issues/310)
- 能力矩阵：[#311](https://github.com/Quchaosheng/workbench-desk-robot/issues/311)
- 阶段闸门：[#314](https://github.com/Quchaosheng/workbench-desk-robot/issues/314)

## 每周回顾

每周产品回顾只应记录：

- 新的用户证据与来源引用；
- 重复出现的问题与受影响的场景；
- 启动、变更、推迟或拒绝工作的决策；
- 外部安装或任务结果；
- 被阻塞的证据与下一名负责人/动作/日期。

模板与口头更新永远不会把工程或发布闸门变为绿色。
