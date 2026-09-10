# 状态看板

快照：2026-08-12。每周及每次发布阻断风险触发时刷新本页。

## 状态规则

| 状态 | 含义 |
|---|---|
| GREEN | 验收证据存在且其验证器通过 |
| AMBER | 工作或证据不完整，但存在受限的恢复路径 |
| RED | 发布阻断项现役，或必需的外部证据缺失 |
| UNKNOWN | 该指标尚未被合格来源测量 |

## 工作流状态

| 工作流 | 状态 | 证据 | 下一步行动 |
|---|---|---|---|
| 确定性基础 | GREEN | CI 运行 `31606342075` 在 `main` 的 `f557069` 上通过 | 保持每个 PR 的必需检查绿色 |
| 脚本化回归 | GREEN | 夜间运行 `31423360868`；输出明确标记为非发布 | 仅保留为契约/回归证据 |
| 发布自动化 | AMBER | tag 运行 `31406815969` 在 SBOM 发布资产上传阶段失败；最小权限修复 PR #21 已合并为 `889f699` | 声明发布就绪前，在下一个人类主导的 tag 上验证合并后的修复 |
| 正式 Gazebo 评估 | RED | `docs/evaluation/failure-cases.md` 说明仍需 36 次真实 Gazebo 运行与独立审计 | 集成、执行并保留原始外部运行器日志 |
| 外部冷启动 | RED | `docs/context/EVIDENCE_INDEX.md` 要求三份参与者记录 | 招募三名唯一参与者并保留失败记录 |
| 硬件发布 | RED | `hardware/release/generated/release_readiness_report.json` 报告 12 个阻断项与 `RELEASE_BLOCKED` | 用有引用的证据关闭每个外部/商业/物理闸门 |
| 采购 | RED | `hardware/procurement/generated/procurement_report.json` 报告 `ORDER_RELEASE_BLOCKED` | 获取注明日期的报价、AVL 批准与来料检验证据 |
| 看板 / 只读 UI | GREEN | 后端行为测试与已提交固定装置回放在 CI 中通过 | 在生产可用性声明前开展有记录的可用性研究 |
| 安全计划 | AMBER | 安全基线 PR #25 已合并为 `23ac229`；CodeQL 在运行 `31606342103` 中通过；安全作业尚不是分支保护要求 | 决定必需检查策略并保持发现项分诊 |

## 发布指标

| 指标 | 目标 | 合格当前结果 | 状态 |
|---|---:|---:|---|
| 虚假完成 | 0 | UNKNOWN - 无正式 Gazebo 审计 | UNKNOWN |
| 碰撞或限位违规 | 0 | UNKNOWN - 无正式 Gazebo 审计 | UNKNOWN |
| 固定脚本抓取成功率 | >=90% | UNKNOWN - 脚本化固定装置不是物理运行 | UNKNOWN |
| 已验证任务完成率 | >=80% | UNKNOWN - 缺少正式 36 次运行集 | UNKNOWN |
| 外部冷启动成功率 | >=2/3 | 0 份合格参与者记录 | RED |
| 硬件发布阻断项 | 0 | 12 | RED |
| 基础 CI | 通过 | 运行 `31606342075` 通过 | GREEN |

## 未来七天

| 优先级 | 行动 | 负责人 | 预期证据 |
|---|---|---|---|
| P1 | 在人类主导的 tag 上演练已合并的最小权限 SBOM 修复 | Integration + 人类发布负责人 | 带保留 SPDX 制品的成功发布工作流 |
| P1 | 冻结正式 Gazebo 运行命令与输出布局 | Simulation + Integration | 外部运行器命令、原始日志样本、哈希/索引 |
| P1 | 决定 CodeQL 与依赖评审是否成为必需检查 | Security + Integration | 已记录的分支保护决策与绿色安全运行 |
| P1 | 为所有未关闭风险分配负责人与截止日期 | Project Owner / PMO | 更新后的 `risks.csv` 评审 |
