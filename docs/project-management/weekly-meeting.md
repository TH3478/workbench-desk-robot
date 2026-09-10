# 每周交付评审

时间盒：45 分钟。会议做决策并清除阻断项；工作流叙述属于会前阅读材料。

## 必备会前材料

- 更新后的[状态看板](status.md)与 [`risks.csv`](risks.csv)；
- 关联提交的失败或新通过检查链接；
- 里程碑偏差与容量变更；
- 需要人类 Project Owner 决策的事项。

## 议程

| 分钟 | 议题 | 必需产出 |
|---:|---|---|
| 0-5 | 闸门与安全阻断项 | 明确的 Red/Amber/Green/Unknown 状态 |
| 5-15 | 关键路径里程碑 | 偏差、负责人、恢复日期 |
| 15-25 | 主要风险与触发条件 | 缓解/应急预案决策 |
| 25-35 | 跨团队依赖与容量 | 负责人之间交接与截止日期 |
| 35-42 | 决策 | 决策记录或 ADR 负责人 |
| 42-45 | 行动项与复述 | 负责人、日期、预期证据 |

## 会议纪要模板

```text
Date / facilitator / attendees:
Baseline commit:
Gate state: P1 __ / P2 __ / P3 __

Evidence reviewed:
- claim:
  reference:
  eligible: yes/no and why:

Decisions:
- D-YYYY-NNN / decision / owner / date / affected risks:

Actions:
- A-YYYY-NNN / action / owner / due / acceptance evidence:

Escalations:
- risk / trigger / contingency / decision needed by:
```

没有负责人、截止日期或验收证据，该项目就不算行动项。没有合格证据，状态就保持 Amber、Red 或 Unknown。
