# 安全工程基线

安全控制保护代码、依赖、证据完整性、部署与机器人授权。它们不能替代功能、仿真或物理安全验证。

## 安全任务图

| 任务 | 控制 / 产物 | 状态 |
|---|---|---|
| SEC1 代码审计标准 | [安全评审标准](secure-review-standard.md) 加 CodeQL | 工作流合并后 ACTIVE |
| SEC2 SBOM 生成 | 固定的 Anchore 工作流与[供应链策略](supply-chain.md) | ACTIVE；下一次发布证明待定 |
| SEC3 依赖扫描 | PR 依赖评审与 Dependabot 配置 | 工作流合并后 ACTIVE |
| SEC4 渗透测试 | [授权测试计划](penetration-test-plan.md) | NOT_EXECUTED |
| SEC5 安全加固 | [加固基线](hardening.md) | PARTIAL；按部署评审 |
| SEC6 事件响应 | [事件响应](incident-response.md) | DEFINED；演练待定 |
| SEC7 安全文档 | `SECURITY.md` 加本手册 | DEFINED |
| SEC8 合规评审 | [控制矩阵](compliance-matrix.md) | READINESS_ONLY；未经认证 |

## 发布阻断规则

- 未解决的严重/高危代码、依赖、密钥、容器或授权边界问题；
- 模型或公共接口获得原始控制、急停、发布或完成授权；
- 必需 SBOM/溯源缺失或无法关联到已发布摘要；
- 在无合格证据的情况下声称渗透、合规或物理结果；
- 响应动作会破坏事件或安全证据。

Security Owner 负责分诊问题。模块 Owner 实施修复。人类 Project Owner 决定发布与有期限的风险接受。
