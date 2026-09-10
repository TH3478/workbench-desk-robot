# 制造工程包

面向 20 台工程试产及其后续生产就绪评审的受控流程基线。该流程路线涵盖收货、
配料、PCB 组装、机械组装、与固件无关的电气检查、功能测试、检验、包装与
可追溯性。

```bash
python hardware/manufacturing/tools/validate_route.py
```

每台设备都有一个序列号与流程随工单。每个质量闸门都记录操作员、固定装置 ID、
校准状态、结果、缺陷代码、返工循环次数与时间戳。安全或隔离检查一旦失败，
绝不允许绕过。

生成的受控记录与图纸包括：

- `generated/quality-traveller.csv`：一台序列化设备的十四个质量闸门。
- `generated/pilot-log.csv`：二十个预分配的 EVT 序列号，初始均为 `NOT_BUILT`。
- `generated/unit-cost-inputs.csv`：可审计的成本输入与证据来源。
- `generated/line-layout.svg`：带围栏 MRB/隔离区的单向 U 型单元。
- `generated/fixture-drawings.svg`：基准定位巢与防护型电气固定装置的尺寸。
- `generated/packaging-drawing.svg`：运输包装剖面与防护要求。
- `harness-spec.csv`：受控的电池、电源轨、数据、CAN 与安全线束包络。
- `harness-component-selection.csv`：连接器壳体、触点、线缆候选、线规兼容性、
  屏蔽端接以及每条线束的剩余审批项。
- `generated/harness_report.json`：计算出的电压降、电流密度、弯曲半径与发布闸门。

线束规范保留 H01-H08 控制器基线，并新增 H09-H14 牵引子板端点（`J_SAFE`、
`J_CAN`、`J_ML`、`J_MR`、`J_ENC_L`、`J_ENC_R`）。H02 是控制器 J2 到子板 J_PWR
的共享电源线束，并纳入显式的集成视图。现在每一行都声明源/目的端点、引脚映射、
有效电平语义、屏蔽语义与泄放线端接语义。H11/H12 端接于外部 M1/M2 电机端子；
H13/H14 端接于外部编码器引脚。生成的报告保留旧有的 `results` 分区，并新增
`traction_results`、`integration_results` 与 `traction_engineering_checks`；所有
对插件与实物导通/拉拔证据仍然是发布阻塞项。每一行都同时携带声明值与要求的
最小弯曲半径。电机引线 H11/H12 要求 20 mm，校验器会拒绝声明值低于要求的半径，
而不是把偏小的数值当作通过。同一报告读取候选电机包，并将 11 A 双堵转需求与
J2 的 10 A 总量上限对比，记录为 `BLOCKED_CANDIDATE_EXCEEDS_J2_LIMIT`。
H01-H03 使用并联 18 AWG 导线，使受控电流包络与候选的 Micro-Fit 18-20 AWG 触点
系列保持兼容。H11/H12 使用 16 AWG 电机引线与 Mini-Fit Jr 候选连接器，取代此前
14 AWG 的空间主张。这些是候选兼容性决策，不是供应商压接批准。CAN 线束使用
三根信号导线加一根独立的屏蔽泄放线；编码器线束使用四根信号导线加一根独立的
屏蔽泄放线。两条泄放线都不分配到保留的 NC 连接器引脚。电缆屏蔽在源端机箱
入口处单点端接：H04-H10 在控制器入口，H13/H14 在子板入口。远端与机箱及信号
回流绝缘。

`J_SAFE` 是仅供 ECO 使用的控制器端点，用于两个独立的硬接线安全通道。它不是
当前的四针 J11；J11 的 H08 线束仍只是单一的 `MOTOR_ENABLE_SAFE` 加诊断端点，
且明确不适合作为子板的 A/B 安全输入。

MFG1-8 与 MFG10-14 具有完整的可复现工程文档。MFG9 具有序列化随工单、试产日志、
缺陷分类与分析方法，但在 20 台设备走完整个流程之前，不能声称已实际执行。
