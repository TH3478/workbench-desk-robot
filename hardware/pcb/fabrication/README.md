# 制造发布候选

使用 KiCad 10.0.5 从 `kicad/controller.kicad_pcb` 生成。

- `gerbers/`：八层铜层、锡膏层、阻焊层、丝印层、板边切割、PTH/NPTH 钻孔、
  F.Cu-In1/In2/In3 盲孔/微孔钻孔以及图文件。
- `controller.d356`：IPC-D-356 电气网表。
- `positions.csv`：元件布局位置数据。
- `drawings/assembly.pdf`：带焊盘轮廓的自动缩放制造/组装视图。
- `drawings/routing-review.pdf`：自动缩放的顶层铜与丝印评审视图。
- `drawings/controller-schematic.pdf`：受控的元件级原理图。
- `fabrication-notes.csv`：受控的板卡版本、表面处理、厚度、HDI、阻抗与 U2
  搁置要求。
- `board-stats.json`：机器可读的板卡统计信息。
- `board-preview.png`：渲染的板卡检查图像。

`stackup.csv` 中 1.60 mm 标称板厚由 1.18 mm 介质与 0.42 mm 铜组成，工程估算按
每盎司 0.035 mm 计算。受控要求为成品层压板与铜总厚 1.60 +/- 0.16 mm，不含阻焊
层。因此 KiCad 统计在两侧各加标称 0.01 mm 阻焊后报告 1.62 mm。供应商必须用一套
可获得的合格材料替换标称介质值，计入成品铜厚与电镀，并用测试条闭环 120 欧姆
CAN 几何；分析层面的厚度匹配并不能关闭供应商 DFM 或阻抗闸门。

受控板卡版本为 EVT1，表面处理为 ENIG。这些值嵌入在 PCB 标题栏与 Gerber 作业
元数据中。它们并不豁免下单发布闸门或供应商 CAM 批准。

U3 使用一个 3 x 3 的 0.45/0.15 mm 激光微孔阵列，从其 F.Cu 上的裸露焊盘连到
In1.Cu 接地参考。这九个微孔需要选择性填铜、盖帽与平坦化；板级默认的
`filling no` 与 `capping no` 只适用于普通过孔，不能覆盖 FAB-003。供应商必须在
发布前批准对位、钢网开孔与组装空洞控制。本工程包中其他位置的 0.30 mm 最小
钻孔是机械钻孔限值，不描述这一 HDI 特征。U3.17/U3.18 通过四个并联的 0.8/0.4
mm 通孔扇出到 Jetson 12 V 铺铜层；布局审计对这两种过孔结构均进行硬性闸门控制。

U6 与 U7 的隔离走廊是全部八层铜层上的规则区域。U6 提供 8.1 mm 的焊盘边缘铜
间距。U7 保留了候选封装的 5.87 mm 板级间隙，但候选模组 2 mm 的爬电/电气间隙
与 200 Vrms 工作额定值仍是一个未解决的安全与供应商闸门。

KiCad DRC 与 ERC 报告位于 `../generated/`；两者均为零违规。本工程包适合用于
供应商 DFM 报价与裸板制造评审。77 行分组 BOM 覆盖全部 110 个电气元件与四个
安装孔。全部 68 个采购受控组保持阻塞，直到所需 Owner 在
`component-approval-signatures.csv` 中完成各自的独立行，填入绑定到本 BOM 哈希的
MPN、数据手册版本、身份、日期与证据。

不要从本目录下单 PCB 或组装。原理图与 PCB 是详细的工程候选，但 U2 是一个可见
的 `DO NOT FIT` 占位符；U2、RPL 与 SFM4 的焊盘图形仍需供应商图纸闭环。运行
`python hardware/pcb/tools/release_readiness.py`；当前预期结果是
`PRODUCTION_RELEASE_BLOCKED`，其下嵌套 `EVT_PROTOTYPE_ORDER_BLOCKED`。人工、
供应商、U2/U7 与测试接入设计闸门阻塞 EVT 下单；实物启动调试与固定装置证据
仍是下游的生产闸门。
