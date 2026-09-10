# Controller PCB 工程包

用于 48 V 桌面机器人的八层控制器/配电板。本工程包定义了电气架构、接口、
保护、隔离 CAN、叠层、布局/热约束、DFM 限制与制造输出。

## 复现检查

```bash
python hardware/pcb/tools/electrical_checks.py
```

报告写入 `generated/electrical_report.json`。用 KiCad 10 打开
`kicad/controller.kicad_pro`。详细的 EVT 配套板包含 110 个受控电气元件、四个
M3 NPTH 安装孔（共 114 个封装）、八个 SMT 测试焊盘、1,250 个走线/过孔条目、
31 个填充铜区、八层铜层，以及一个物理 8 mm 的一次/二次隔离区域。
用以下命令复现受控源文件：

```bash
<kicad>/bin/python hardware/pcb/tools/generate_footprints.py
python hardware/pcb/tools/generate_kicad_schematic.py
python hardware/pcb/tools/generate_expected_connectivity.py
python hardware/pcb/tools/generate_bom.py
<kicad>/bin/python hardware/pcb/tools/generate_kicad_board.py --session hardware/pcb/kicad/controller.ses
python hardware/pcb/tools/electrical_checks.py
python hardware/pcb/tools/audit_connectivity.py
python hardware/pcb/tools/layout_audit.py
python hardware/pcb/tools/export_fabrication.py
python hardware/pcb/tools/release_readiness.py
```

默认命令是一个生产发布闸门，在 PCB 仍被阻塞时返回非零值。原型下单闸门使用
`--stage evt`，只需要仓库工程审计时使用 `--stage structure`。

`kicad/controller.ses` 是已检入的 Freerouting 2.3.0 布线会话。板级生成器在导入
1,039 段布线段和 137 个过孔之前，会针对该会话校验每个封装的位置。确定性清理
与局部补充最终产生 1,070 段线段、180 个过孔和 31 个铜区。过期的会话无法悄悄
附着到已移动的封装上。生成的原理图 UUID 使用可重置的确定性序列。板级输出
UUID 在 KiCad 保存文件后进行归一化，既保留重复引用又消除逐次运行的 UUID
噪声；布线平局裁决使用稳定的引用、焊盘与坐标键，而不是进程内存地址。

布局审计对当前 U2 的 `12V_ISO` 源和 `GND` 回流进行硬性闸门控制，要求在 THT
焊盘周围的 3 x 3、1.5 mm 间距网格上分别布置八过孔环。这些环仅用于空间与电流
路径的概念检查：U2 仍然是 `TBD_36_60V_TO_12V_240W_ISOLATED`，没有冻结的 MPN
或焊盘图形。所选转换器需要通过 ECO 替换封装和布局位置、重建布线与铺铜、重跑
DRC/连通性检查并重做热评审。审计还要求顶层零过孔的晶振布线、匹配的 CAN 过孔
数量，以及一个 `In1.Cu` 的 `GND_CAN_ISO` 铜区声明。U7 在其 5.87 mm 的板级焊盘
间隙上有一条全八层禁走线/禁过孔/禁铺铜的走廊；这保留了现有几何空间，但并不
能修复候选模组 2 mm 的爬电/电气间隙或 200 Vrms 的工作额定值。CAN 耦合、分支/
短桩几何、参考平面连续性以及 120 欧姆场求解仍然是明确的、需要人工或供应商
处理的风险，而不是从总布线长度推断出来的。报告现在包含逐网络的图度量指标、
已声明的参考铜区端点覆盖率、参考层低速占用率、晶振长度差值与负载电容接地
过孔距离，以及每个被接受的电源网络缩颈。这些测量为评审划定边界；它们并不
声称场求解阻抗、连续填充铜或热充足性。

测试接入按电气域分隔：TP1 位于 48 V 一次侧区域，TP2-TP5/TP8 在逻辑侧，TP6/TP7
位于隔离 CAN 连接器旁边。这样二次侧探针不会进入一次侧测试区域，也避免了过长
的 CAN 测试短桩。`fixture-access-plan.csv` 控制 EVT 固定装置所需的额外安全、
电源正常、故障、电流监控与域参考接入。现有连接器接入会与 `connector-pinout.csv`
交叉核对；七个缺失的专用焊盘仍是明确的 `ECO_REQUIRED` 项，且在附上实物固定
装置证据之前，每一行都保持失败即拒绝。

已检入的 ERC 与 DRC 报告包含零违规和零未连接项。Gerber 文件、钻孔、IPC-D-356、
位置数据、图纸、统计信息以及渲染的检查预览位于 `fabrication/` 下。
`kicad/controller.kicad_dru` 强制一次侧 48 V 域与每个非一次侧网络之间保持 8 mm
铜间距；即使基础 DRC 报告其他方面干净，只要缺少该规则，发布就绪检查就会失败。

## 关键设计评审修正

原任务建议（`TPS54160 + RT8059 + AMS1117`）不具备负载能力：

- TPS54160 是 1.5 A 稳压器，无法提供拟议的 12 V 电机电源轨。
- RT8059 是低电流转换器，无法提供 5 V / 8 A 的 Jetson 电源轨。
- AMS1117 无法提供 3.3 V / 5 A，且会超过其热极限。

因此基线要求一个受保护的 48 V 输入和一个隔离稳压的 36-60 V 转 12 V、240 W 级
模块，随后是一个通往 Jetson 开发者套件直流输入的受保护 12 V / 5 A 分支，以及
一个 12 V 转 3.3 V、20 W 的同步降压转换器。它刻意不通过 5 V 排针反哺开发者
套件。候选设计列在制造 BOM 中；由于元件选型超出 issue #19 的职责边界，采购
需要系统 Owner 的 AVL 签核。

U2 没有可下单的设计候选。`DCM3623T50M31C2T00` 被明确排除：Vicor 官方 PDF 注明
其输入为 16-50 V、输出为 28 V，且为九端子通孔 ChiP 封装，因此无法满足 36-60 V
转 12 V 的要求，其焊盘图形也与已检入的布局不兼容。`source-baseline.json` 中的
Vicor 来源仅作为排除证据。在真实的 MPN 和供应商焊盘图形冻结之前，已检入的
原理图、BOM、PCB 与布线会话统一使用 `NOT FOR PRODUCTION` 占位符，该占位符
不得用于组装或下单。

该板是 NVIDIA 开发者套件的配套/控制板，而不是裸的 260 针 Jetson 模组载板。
受控接口、官方来源、假设、Owner 与冻结闸门见 `interface-control.md` 和
`source-baseline.json`。本板上贴装了 J4；J7-J9 描述下游线束或子板的端点，
在版本 A 中不贴装。ISO1042 总线侧使用由 U7 提供的、彼此独立的 `5V_CAN_ISO`
与 `GND_CAN_ISO` 网络；在板级数据库中二者都不与逻辑地相连。已贴装的 J4 背板
在引脚 8 上承载外部 `JETSON_ENABLE_REQ`；它是 MCU 输入，且刻意与 U5 引脚 53
上生成的内部 `MOTOR_ENABLE_REQ` 区分开。急停路径具有彼此独立的
`MOTOR_ENABLE_REQ` 与 `MOTOR_ENABLE_SAFE` 网络、J10 双通道回路、J12 双通道手动
复位、U8 诊断隔离、力导向继电器候选 K1/K2 以及 J11 闸门输出。
`connector-pinout.csv` 冻结当前的 EVT 引脚映射。`connectors.csv` 将触点能力与
受控系统包络区分开：J2 触点是 16 A 标称接口，但在外部分支保险丝、浪涌、再生
与故障电流分析获得批准之前，板与线束被限制为 10 A 总量（12 V 下 120 W）。
`component-selection-matrix.csv` 跟踪每个在用模块、有来源支撑的候选或类别、
验证方法、Owner 与采购状态。每个矩阵 `source_id` 都必须能在
`source-baseline.json` 中解析到一个完整的 HTTPS 来源条目；排除证据只能用于
U2 的 TBD 行。`fabrication/bom.csv` 包含覆盖每个板上元件的 77 行分组清单。
`component-approval-register.csv` 为全部 68 个采购受控组定义了所需的批准角色。
`component-approval-signatures.csv` 为每个所需角色提供一行独立记录，绑定 EVT
版本与制造 BOM 的 SHA-256。只有当每个角色都记录可下单的 MPN、数据手册版本、
身份、日期和证据引用后，该组才算获得批准。仅把汇总 `decision` 改为 `APPROVED`
是不够的。`expected-connectivity.json` 与 `generated/connectivity_report.json`
独立检查全部 110 个受控元件的 458 个物理焊盘，外加输入保护、CAN 隔离、电流
检测与双通道安全不变量。`testpoint-coverage.csv` 为每个物理测试焊盘定义测量
项目、限值、仪器与所需证据。`fabrication/bringup-test-plan.csv` 覆盖 36 V、
48 V 与 60 V 运行、UV/OV、反接、分支短路、负载与再生瞬态、急停不一致、CAN
FD，以及四小时的封闭机箱热浸泡。这些是受控测试定义，不是已完成的实物证据。

在分享下单包之前运行 `python hardware/pcb/tools/release_readiness.py`。已检入的
原理图是元件级（110 个符号与 366 个连线网络标签）且 ERC 干净。EVT 原型下单
仍被 AVL、安全设计、供应商 DFM、U2/U7 与测试接入 ECO 等闸门阻塞。实物启动
调试、实测安全时序、线束执行与已验证的固定装置接入是独立的生产发布闸门，
因此原型证据不会成为循环的 EVT 下单前提。如果被排除的 U2 MPN 或封装出现在
任何受控的原理图、PCB、库、布线、网表、BOM 或位置工件中，发布审计同样会
失败即拒绝；在所需 ECO 完成之前，它一直保持
`isolated_power_mpn_and_land_pattern_frozen` 为 false。
不要仅凭工程完整性就下单贴装板。

## 发布状态

PCB1-11 具有可复现的工程证据。PCB12-18 已具备完整的下单、启动调试、可靠性
与生产规程，但仍需要实物板卡、实验室仪器、EMC 设施与生产操作人员；必须附上
而非推断的证据见验证矩阵。
