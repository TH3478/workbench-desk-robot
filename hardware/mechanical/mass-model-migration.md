# Revision D 质量模型迁移

`REV-D-MASS-001` 是权威的 Revision D 分析模型。它定义于
`design-spec.json#components`；生成的报告从该来源导出总量与重心，并用 SHA-256
哈希绑定结果。

早期七行、55 kg 的计划台账保留在 `mass-ledger-legacy.csv` 中。每个遗留行都标记
为 `SUPERSEDED`，在代表某个组件时映射到当前的稳定组件 ID，并标记为 `EXCLUDED`。
它不得用于稳定性、负载、升降、跌落、采购或发布决策。

基线之前生成的 `63.5 kg` 总量及其 `[-24.1, 441.6] mm` 重心以一行被取代的汇总
记录保留在同一文件中；当前的 `77.5 kg` 模型是唯一的 Revision D 发布输入。

当前九行的 `mass-ledger.csv` 仅供评审镜像。其 ID、坐标、单位、坐标系、质量、
不确定度、版本与纳入规则都会对照权威来源校验。任何不一致、重复、缺行或过期的
生成哈希都会阻塞就绪状态。

四位 Owner 的批准记录在 `mass-model-approval-register.csv` 中。在 Product、
Mechanical、Hardware 与 Safety Owner 签署受控版本之前，它们保持 `REQUIRED`；
分析估算不构成批准或实物验证。
