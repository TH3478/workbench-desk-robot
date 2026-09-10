# 电气设计评审

## 架构与保护序列

```text
48 V battery
  -> 10 A fuse -> reverse-polarity MOSFET -> 58 V TVS
  -> hot-swap/inrush controller (UV 34 V, OV 62 V, 8 A limit)
  -> TBD isolated regulated 36-60 V-to-12 V / 240 W-class module
       -> protected 12 V motor auxiliary output
       -> protected 12 V / 5 A branch -> Jetson developer-kit DC input
       -> 3.3 V / 5 A synchronous buck -> MCU, sensors, isolated CAN logic

CH32V307 <-> reinforced digital isolation <-> CAN FD transceiver
Dual-channel E-stop + manual reset -> K1/K2 force-guided relay candidates
  -> MOTOR_ENABLE_SAFE; U8 provides isolated channel diagnostics only
MCU supplies MOTOR_ENABLE_REQ and observes state but cannot bypass K1/K2
```

电源正常时序为 `12V_ISO`、`JETSON_12V`、`3V3_LOGIC`，最小间隔 10 ms。设计目标
是：任何输入 UV/OV、热插拔故障、通道不一致或急停断言都能在 1 ms 内禁用
`MOTOR_ENABLE_SAFE`，同时逻辑电源轨保持工作；仍需提供实测的跳闸时间证据。
急停后的恢复需要回路复原外加一次独立的物理复位。U8 仍只是一个接口载板：
在 Safety Owner 冻结安全架构、诊断覆盖率、复位电路、实现与失效模式分析之前，
下单发布被阻塞。

U2 是一个未解决的需求包络，而不是已选定的元件。`DCM3623T50M31C2T00` 因其官方
数据手册而被排除：其 16-50 V 输入、28 V 输出与九端子通孔封装不满足本设计。
所要求的 U2 状态是 `TBD_36_60V_TO_12V_240W_ISOLATED`；可下单的 MPN 与供应商
焊盘图形均未冻结。

## 信号完整性与接地

- 八层叠层将 L2/In1 与 L5/In4 用于接地参考与低速布线任务。L7/In6 承载受控逻辑
  与安全信号；L3/In2、L4/In3 与 L6/In5 承载彼此分离的一次侧、受保护 Jetson、
  逻辑与隔离 CAN 配电，以及已布线的引出线段。已发布的 Gerber 文件（而非笼统的
  层标签）才是铜层的真实来源。
- CAN 目标是 120 欧姆差分。`CANH_RAW/CANL_RAW` 是顶层点对点走线，每个网络使用
  两个匹配的 F.Cu 到 In3.Cu 盲孔，长度分别为 12.210/13.942 mm（总差值 1.732
  mm）。现场侧 `CANH/CANL` 树状走线位于顶层且零过孔，总长度 72.352/72.073 mm
  （差值 0.279 mm）；两个网络都分支到两个连接器、保护、终端与测试点。
- 隔离 CAN 走廊在 `In1.Cu` 上声明了一个相邻的 `GND_CAN_ISO` 铜区。线对耦合、
  短桩几何、分支对应、参考平面连续性、供应商场求解与阻抗测试条仍然是发布
  闸门。
- SPI 串联终端封装已贴装；回流路径几何必须对照已发布的 Gerber 文件与供应商的
  最终叠层进行评审。
- 自定义 DRC 规则强制一次侧与二次侧域之间保持 8 mm 铜间距。成品板的爬电与
  污染等级仍需要供应商与安全评审。
- U7 的逻辑/现场焊盘排之间有 5.87 mm 的板级铜间隙，并受到全八层禁走线/禁过孔/
  禁铺铜规则区域保护。MEJ1S0305SC 候选模块本身仍不适合作为增强绝缘的证据，
  因为其记录的爬电/电气间隙为 2 mm，工作额定值为 200 Vrms。

受控阻抗值必须根据所选制板厂的实际介质表重新计算。任何通用走线宽度都不能
作为阻抗保证发布。

## 热设计计划

15 W、25 W 与保守的 40 W Jetson 负载工况位于本板之外；受保护的 12 V 分支与
线束按 5 A 连续电流筛选。Jetson 散热方案传导至机箱，并与 PCB 分开验证。
配电在 L1/L3/L6/L8 上使用 2 oz 铜。当前 U2 概念在 THT 焊盘周围的 3 x 3、1.5
mm 间距网格上布置了分离的八过孔源极环与回流环，使用 0.8 mm 过孔、0.4 mm
钻孔。这一几何只能证明候选的电流传输方案能够容纳得下；它不是已发布的封装
或散热方案。在真实 MPN 与焊盘图形冻结后，必须通过 ECO 重做封装、布局位置、
布线、铺铜、间距、DRC、连通性审计与热分析。填充或盖帽仍取决于所选定的组装
工艺。铜面积是否充足是热评审项，不能从 DRC 推断。热验收标准为：在 35 C 环境
温度下、生产机箱闭合时测量，转换器结温低于 110 C，Jetson 模组低于 80 C。

U3 的裸露焊盘有一个 3 x 3、0.4 mm 间距的 0.45/0.15 mm F.Cu 到 In1.Cu 激光
微孔阵列，从焊盘中心偏移 0.25 mm 以避开 PGTH 走线。其 5 A 输出通过四个并联的
0.8/0.4 mm 通孔进入 In3.Cu Jetson 铺铜层。激光微孔填充、盖帽、平坦化、对位、
钢网开孔与空洞控制仍是供应商 DFM 闸门，而不是 DRC 推断。

## EMI 预合规

- CAN 共模扼流圈与 TVS 布置在现场连接器之前。
- 由 LISN 数据驱动的任何输入滤波改动都需要 ECO，并重新进行 DRC/热评审。
- 开关节点铜保持在 L1 上且面积最小，其回流紧贴正下方 L2。
- 只有在完成 CAN 与电源轨噪声对比之后，才允许使用任何扩频模式。
- 预先扫描 150 kHz-30 MHz 的传导发射与 30 MHz-1 GHz 的辐射发射。
- 在认证之前，在可触及的连接器上测试 ESD，并在电池输入端测试 EFT/电快速瞬变
  脉冲群。

## 制造与组装发布检查清单

1. KiCad ERC 零错误；所有豁免均已签署并收录。
2. DRC 采用 0.15 mm 线宽/间距、0.30 mm 成品钻孔、0.15 mm 环宽，以及 8 mm 隔离爬电。
3. 输出 Gerber X2、Excellon PTH/NPTH、IPC-356 网表、钻孔图、叠层、制造图纸与板卡 PDF。
4. 导出带制造商料号与 AVL 状态的 BOM；任何 `HOLD` 行都不得下单。
5. 导出质心数据、组装图纸、锡膏层与极性图。
6. 独立比对 Gerber 与 PCB 的网络，并检查所有铺铜、间距、标签与 1 脚标记。
7. 冻结 U2 可下单的 MPN 与供应商焊盘图形，完成其布局/热 ECO，并证明所有已发布
   工件中都不含被排除的 DCM3623 数据。
