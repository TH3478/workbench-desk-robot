# 双轴牵引子板工程包

本工程包为两个底盘牵引电机定义一个可评审、可独立更换的控制器。它不控制六个
UR5e 关节或 Robotiq 夹爪；这些仍由各自的厂商控制器管理。

工程基线将牵引功率级移出主控制器 PCB。控制器板提供受限的 J2 辅助分支、双通道
安全 ECO 与隔离 CAN 接口；开关桥、再生能量处理与电机/编码器连接器保留在这块
可更换的子板上。两个电机是外部底盘组件，不是板上安装的零件。这种划分避免电机
热量、换相电流与钳位脉冲进入 Jetson 控制器布局，同时保持子板可维护。

所选定的评审基线是两台由控制器 PCB 的 J2 辅助输出供电的 12 V 有刷直流减速电机。
一颗 `DRV8962DDVR` 是驱动器候选，不是已批准的零件。Pololu 型号 4753 现在是
可追溯的电机候选（12 V、50:1 减速箱、64 CPR 编码器）；它不是经批准的 AVL 选型。
其数据手册标注/外推的 5.5 A 堵转电流在双电机下约为 11 A，超过 J2 的 10 A 总量
上限。因此在该候选方案继续推进之前，必须先制定受限的电流限制、堵转与运动轮廓
策略。量产电机 MPN、最终绕组包络、轮载与热负载仍然未定。因此即使其确定性工程
检查全部通过，本工程包也保持 `DO_NOT_ORDER`。

## 划分与功率路径

```text
controller J2, 12 V / 120 W aggregate maximum
  -> childboard branch protection and reverse-polarity protection (TBD)
  -> local bulk plus approved regenerative-energy sink (TBD)
  -> DRV8962 four half-bridges
  -> left and right brushed motors (MPNs and envelopes TBD)

VM_PROTECTED -> candidate U6 local logic regulator -> VCC_LOGIC
             -> candidate U7 isolated CAN-side converter -> GND_CAN_ISO island
J_PWR GND_MOTOR -> STAR_GND_01 -> GND_LOGIC (one intentional join only)

isolated CAN field bus -> isolated CAN interface -> local traction controller
dual hardwired safety channels -> independent nSLEEP and EN gating
```

J2 的 120 W 限值是共享的输入上限，不是单轴额定值。在 12 V 下，考虑线束压降、
转换损耗、瞬态裕量与温度降额之前即为 10 A 总量。DRV8962 数据手册中每输出 10 A
的 DDV 能力同样是 IC 限值，不是板级、连接器、电机或双轴同时工作的额定值。

隔离 12 V 转换器不被假定为可吸收再生电流。电机在减速或反拖时可能抬高本地母线
电压，因此在原理图可以发布之前，需要一套经批准的阻断、储能电容、钳位/制动开关
与能量泄放组合。受保护的 48 V 牵引母线作为备选方案保留在 `architecture-options.csv`
中；它仍被电池最大电压、浪涌/再生包络、电机选型与合适的功率级所阻塞。

子板不得将 `GND_CAN_ISO` 与 `GND_MOTOR` 相连。因此其 CAN 接口需要隔离 CAN FD
收发器与隔离侧电源；J_CAN 引脚 4 保持 `NC`，与控制器 J5/J6 一致。电缆屏蔽端接
是独立的机箱/EMC 决策，不分配到该保留引脚。

候选回流拓扑由 `net-topology.csv` 控制。`GND_MOTOR` 是大电流的 J2/PGND 回流；
`GND_LOGIC` 服务于 MCU、驱动器逻辑与安全闸门，并只在 `STAR_GND_01` 处与电机
回流相接一次。逻辑稳压器（`U6`）与隔离 CAN 转换器（`U7`）仅是功能占位符；两者
都保持 `TBD_BLOCKING`，没有经批准的 MPN。

同一拓扑表将每个电机端子闭合到指定的一个 DRV8962 输出、每个编码器电源与回流
接到逻辑域、每个正交通道接到各自的控制器输入。四个 `IPROPI` 输出也保持为四条
独立的 ADC 路径；把它们并接会掩盖半桥故障，校验器会予以拒绝。驱动器的 VM 引脚
在 `F1` 与 `Q1` 之后使用 `VM_PROTECTED`，而不是单独的或被旁路的电机供电网络。

每条稳压路径都用独立的输入行与输出行表示。因此 `U6` 有 `VCC_LOGIC_INPUT ->
U6.IN`，随后是 `U6.OUT -> VCC_LOGIC`；而 `U7` 有一次侧 `VCC_CAN_ISO_INPUT` 行与
二次侧 `VCC_CAN_ISO`/`GND_CAN_ISO` 岛。校验器会拒绝把稳压器输入与其输出混在
同一行、或把本地接地端点跨过 `U7` 隔离屏障的行。

## 安全不变量

软件可以请求扭矩，但不能创造安全许可。通道 A 必须硬件闸控 `nSLEEP`；通道 B
必须独立地闸控全部四条 `ENx` 路径。任一通道断开都会禁用两个桥，并将不一致状态
闭锁，直到上游手动复位序列完成。`MOTOR_ENABLE_REQ`、CAN 报文、MCU GPIO 与看门狗
都无法绕过任一闸门。

`U1.nFAULT` 同时是两个独立安全闸门的硬件输入，而不仅仅是 MCU 诊断。
`safety-gate-connectivity.csv` 记录分离的 A/B 防护扇出、电源正常抑制与默认低电平
状态。因此驱动器故障、本地逻辑电源轨缺失、安全回流断开或交叉故障都会使两个桥
保持禁用并闭锁。所选闸门电路、偏置值与时序仍需要经批准的详细原理图与实物故障
测试。

当前控制器 J11 只提供一个 `MOTOR_ENABLE_SAFE` 输出加诊断用 `ESTOP_SENSE`。拆分
这个单一安全信号、或把 `ESTOP_SENSE` 当作第二通道，都是被禁止的。因此，一个经
Owner 批准的、提供两个独立安全输出的控制器与线束 ECO 是发布阻塞项。

因此 `J_SAFE` 是控制器 J10/K1/K2 安全链的一个 ECO 端点，不得直接连接到当前的
四针 J11。候选的元件级接线契约见 `schematic-design.md`；它不是一份完成的 KiCad
原理图，也不改变 `DO_NOT_ORDER` 状态。

## 布局概念

`placement-plan.csv` 是功能块布局，不是焊盘图形定义。运行确定性渲染器以更新
评审图纸：

```bash
python hardware/motor_driver/tools/generate_layout_review.py
```

该概念将输入保护与能量管理回路放在电源连接器处、驱动器居中、电机连接器在对侧
边缘，CAN/控制与安全模块远离开关电流回路。确切的板卡尺寸、铜厚、驱动器焊盘
图形、散热器安装、爬电、连接器封装与安装孔位仍是阻塞性输入。

布局基准现在是明确的：`+Y_REAR` 是 `J_SAFE`、`J_CAN`、`J_ML` 与 `J_MR` 的连接器
边；编码器连接器保持在 `-Y_FRONT`，`J_PWR` 保持在 `-X_POWER` 边。
`validate_motor_driver.py` 会对照 118 x 82 mm 外形检查边坐标，使后续布局无法
悄悄旋转线束接口。SVG 评审图从同一布局表重新生成。

## 复现检查

```bash
python hardware/motor_driver/tools/validate_motor_driver.py
python hardware/manufacturing/tools/validate_harnesses.py
python -m pytest tests/hardware/test_motor_driver_package.py -v
```

通过这些检查意味着本工程包内部一致且失败即拒绝。它绝不能替代干净的详细 ERC/DRC、
经批准的 AVL、供应商 DFM、安全评审、标定波形、测功机数据或实物热验证。
