# 资源计划

这是一份职责与容量计划，不是声明所列人员已被聘用或可用。

| 阶段 | 关键工作流 | 问责角色 | 支持角色 | 容量规则 |
|---|---|---|---|---|
| P1 | 确定性集成与正式 Gazebo 评估 | Integration | Simulation、Motion、World Model、Perception | 为世界/机械臂调参保护至少两名协同负责人；不要拆分不可追溯的参数变更 |
| P1 | 发布与安全基线 | Integration | Security、Project Owner | 代码作者、人类评审人与人类发布决策三者分离 |
| P1 | 外部复现 | Project Owner | Documentation、Integration | 三名唯一参与者；维持协议定义的支持边界 |
| P2 | 任务/场景扩展 | Runtime | World Model、Perception、Evaluation | 仅在插件/契约冻结后按任务族并行 |
| P2 | 可观测性与性能 | Integration | Performance、Backend | 采集代码一名负责人，业务指标评审人另设 |
| P2 | 硬件输入冻结 | Hardware Owner | Procurement、Quality、Compliance | 商业/物理证据不能被软件容量替代 |
| P3 | 物理适配器集成 | Hardware Owner | Motion、MCU、Linux、Safety | 人类负责人控制 `robot/control` 固件与急停边界 |
| P3 | 硬件评估 | Project Owner | Hardware、World Model、Evaluation | 闸门评审前安排好操作员与独立审计容量 |

## 再分配顺序

当关键工作延期时，依次从表达打磨、回放视觉打磨、可选自然语言工作、可选本地模型工作抽调容量。绝不通过削弱虚假完成、碰撞、权限、评估或外部复现闸门来换取进度恢复。
