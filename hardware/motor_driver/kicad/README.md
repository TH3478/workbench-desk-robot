# KiCad 发布搁置

在 `CONCEPT-B` 阶段不输出任何可下单的 KiCad 原理图或 PCB。电机 MPN 与电流包络、
再生能量路径、双通道安全接口、控制器/协议、电源保护、连接器焊盘图形、叠层与
散热方案都是未解决的发布输入。用占位封装做出一块看似干净的板子只会掩盖这些
缺口。

`../placement-plan.csv`、`traction-childboard-concept.kicad_pcb` 与
`../generated/placement-review.svg` 提供一个确定性的 118 x 82 mm 机械/功能评审，
带 108 x 72 mm 四孔安装图。概念板不含任何电气封装、铜或布线，并明确标记为
`DO NOT ORDER`；它不声称焊盘图形或电路已完成。在 `MTR-MOTOR`、`MTR-POWER`、
`MTR-REGEN`、`MTR-SAFETY`、`MTR-CONTROL` 与 `MTR-DRV` 关闭之后，电气 KiCad 工件
成为必需。它们随后必须通过 ERC、DRC、连通性、电流路径、热与供应商 DFM 检查，
`MTR-SCHEMATIC` 与 `MTR-LAYOUT` 闸门才可通过。

未来原理图必须实现的候选网络与安全契约是 `../net-topology.csv`、
`../safety-gate-connectivity.csv` 与 `../schematic-design.md`。它们明确保留一个
单一的 `STAR_GND_01` 汇接点、一个隔离 CAN 电源岛与到两个安全闸门的独立 `nFAULT`
扇出；这些契约都不是可下单的电气设计。
