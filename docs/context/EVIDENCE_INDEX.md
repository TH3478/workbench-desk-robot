# 证据索引

| 声明 | 所需证据 | Owner |
|---|---|---|
| 任务完成 | 观测 + ActionResult + 验证器规则 | World Model |
| 动作安全 | 规划器/控制器/MCU 记录 | Motion + MCU |
| 场景有效 | 清单 + 验证器输出 | Simulation |
| 指标通过 | 原始事件 + 固定计算脚本 | Product Owner + World Model |
| 本地模型可用 | 黄金集 + `runs/performance/local-model-plan.json` + 延迟/资源报告 | Runtime + Perception |
| 启动目标通过 | `runs/performance/startup-cold.json` 与缓存的启动报告 | Integration |
| 发布可复现性 | SPDX SBOM + 含镜像仓库摘要的 `release-manifest.json` | Product Owner + Integration |
| 真实硬件时序 | 统一硬件 JSONL + 经哈希校验的操作员清单 | Hardware Owner |
| 外部冷启动 | 三份已填写的参与者记录与验证器输出 | Product Owner |
