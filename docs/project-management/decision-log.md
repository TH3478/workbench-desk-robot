# 决策日志

架构决策保留在 `docs/decisions/` 中。本索引补充交付与闸门决策，不重复其论证。

| ID | 日期 | 决策 | 状态 | 证据 / 记录 | 复审触发条件 |
|---|---|---|---|---|---|
| ADR-0001 | existing | 保持 P0 范围证据优先且受限 | accepted | `docs/decisions/ADR-0001-p0-scope.md` | 确定性路径无法满足闸门 |
| ADR-0002 | existing | 保持许可证决策明确 | pending review | `docs/decisions/ADR-0002-license-pending.md` | 代码/模型/资产发布组成发生变化 |
| ADR-0003 | existing | 使用 RISC-V QEMU 作为 MCU 基线 | accepted | `docs/decisions/ADR-0003-mcu-riscv-qemu.md` | 选定物理 MCU 发生变化 |
| ADR-0004 | existing | 使用 UR5e 加 Robotiq 基线 | accepted | `docs/decisions/ADR-0004-arm-selection.md` | 供应商包或可达性推翻该选择 |
| D-2026-001 | 2026-08-11 | 保留 `contents: read` 并禁用隐式 SBOM 发布资产上传 | 已在合并的 PR #21 中实现；发布验证待完成 | issue #20、失败运行 `31406815969`、合并 `889f699` | 下一次人类主导的 tag 运行 |

## 新决策记录

```text
ID / date / owner:
Decision needed:
Options considered:
Decision and rationale:
Affected milestones, risks, contracts, and owners:
Evidence reviewed:
Revisit trigger/date:
Human approver:
```

AI 可以起草本记录。只有指定的人类审批人可以接受范围、风险、发布、许可、采购或物理安全决策。
