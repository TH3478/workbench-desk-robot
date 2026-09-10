# 外部安装与试用反馈

每次可复现的失败或有意义的成功各用一条记录。原始日志、截图和视频私下保存；用不透明证据引用关联它们。

## 背景

- 反馈 ID：
- 参与者/伙伴引用：
- 产品版本/提交：
- 场景 ID/版本：
- 环境等级：
- 证据等级：`user_report` / `software` / `scripted_fixture` / `gazebo` / `physical`
- 报告人角色：
- 操作系统与版本：
- 硬件/型号摘要（已脱敏）：
- Python/ROS 版本（如适用）：
- 日期与时长：

## 复现

- 预期结果：
- 实际结果：
- 行为出现偏差的确切步骤：
- 复现率：
- 最小复现命令或规程：
- 证据引用：
- 相关事件库运行/回放 ID：
- 清单/策略/验证器版本：

## 分类

- 类别：`installation` / `compatibility` / `documentation` / `runtime` / `perception` / `motion` / `evidence` / `network` / `hardware` / `new_problem`
- 影响：`blocked` / `major` / `moderate` / `minor`
- 证据状态：`confirmed` / `refuted` / `insufficient_evidence` / `failed` / `not_executed` / `blocked`
- 临时变通方案：
- 疑似负责人：
- 后续动作与截止日期：

## 解决

- 根本原因（验证后）：
- 修复或文档变更：
- 验证命令/结果：
- 用户复测结果：
- 关联 Issue/PR：
- 后续决策：
- 发布资格：`false` / `true`（附上适用的闸门引用）

## 质量检查

- [ ] 环境与版本已记录。
- [ ] 预期结果与实际结果分开记录。
- [ ] 原始证据被引用但未被提交。
- [ ] 报告未混淆 ActionResult 主张与已观测的 WorldState。
- [ ] 证据不完整时失败仍然可见。
