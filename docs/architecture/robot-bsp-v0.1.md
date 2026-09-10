# Robot BSP V0.1 冻结包

状态：逻辑拓扑已冻结；物理板卡与控制器选型仍是实现闸门。

本文档冻结机器人板级支持包（BSP）的逻辑所有权与安全边界。选定量是**每台机器人一块 Linux
开发板与六个控制器域**：两个机器人自有的 MCU 和四个机械臂/工具模块控制器域。它不声称物理
板卡、内核移植、设备树或生产安全审批已存在。确切的 SoC、内核版本、CAN 控制器、引脚号、IRQ
号和电气额定值保持开放，直到板卡选型闸门签署。

数量决策刻意独立于厂商部件选型：一块 Linux 板是唯一的高层计算与网关域，而六个控制器域提供
实时所有权与安全隔离。增加第二块 Linux 板或第七个控制器域需要新的架构评审，不能在 BSP 实现
PR 中隐式发生。

## 1. 系统拓扑

```text
                         Linux development board
          ROS 2 / planning / perception / navigation / services
          device management / logs / update / diagnostics
             |       |        |        |        |
          CAN-FD   Ethernet   USB     UART    debug console
             |
       isolated CAN backbone (CAN0, two physical termination points)
        |             |              |              |
   MCU-BASE    ARM-L CTRL    ARM-R CTRL    TOOL-L CTRL    TOOL-R CTRL    MCU-SAFETY
   chassis     left 7-axis   right 7-axis  left end tool  right end tool  E-stop/watchdog/
   drive/lift  module ctrl   module ctrl   module ctrl    module ctrl     safe enable

  Power path: battery -> protected power entry -> Linux and MCU rails.
  Safety path: E-stop -> MCU-SAFETY -> independent drive/arm enables.
  Linux commands are requests; they cannot create a safety permissive.
```

基线使用一块 Linux 板和六个控制器域。V0.1 中升降机构由 `MCU-BASE` 拥有。机械臂与工具域可以
是厂商控制器板或本地 MCU，但每个都独立可重置、可寻址。独立的 `MCU-LIFT` 不属于 V0.1。

## 2. MCU 所有权

| 节点 | 必需职责 | 不得拥有 |
|---|---|---|
| `MCU-BASE` | 牵引电机、轮编码器、底盘里程计、升降运动与限位 | 急停决策或 Linux 应用状态 |
| `ARM-L-CTRL` | 左七轴机械臂伺服/控制接口 | 右机械臂或全局安全锁存 |
| `ARM-R-CTRL` | 右七轴机械臂伺服/控制接口 | 左机械臂或全局安全锁存 |
| `TOOL-L-CTRL` | 左末端执行器控制、限位与遥测 | 机械臂轨迹规划或全局安全锁存 |
| `TOOL-R-CTRL` | 右末端执行器控制、限位与遥测 | 机械臂轨迹规划或全局安全锁存 |
| `MCU-SAFETY` | 双通道急停、安全使能、看门狗、故障锁存与安全输入 | 轨迹规划、视觉、普通遥测聚合 |

每个域有独立的心跳、启动标识符、故障状态与重置域。一个运动节点丢失绝不能静默清除安全锁存
或使能另一个运动节点。

## 3. Linux 板接口分配

这些是逻辑分配。数字引脚和 SoC IRQ 刻意保持未分配，直到原理图与设备树评审。

| 逻辑资源 | Linux 名称 | Owner | IRQ/DMA 规则 | 冻结证据 |
|---|---|---|---|---|
| 隔离 CAN 主干 | `can0` | 所有 MCU 节点 | 控制器 IRQ 线程化/NAPI 安全；仅控制器需要时使用 DMA | 控制器数据手册、总线时序与终端评审 |
| 服务以太网 | `eth0` | Linux 服务/更新 | 使用 SoC MAC IRQ 与文档化的 PHY 复位 GPIO | PHY 地址、复位时序与链路测试 |
| USB 主机 | `usb0` | 相机、存储、服务工具 | xHCI IRQ 由内核拥有；无安全依赖 | 功率预算与热插拔测试 |
| MCU 服务 UART | `ttyS-mcu` | Linux 到恢复控制台 | RX DMA 可选；唤醒 IRQ 必须文档化 | 波特率、电平转换与恢复记录 |
| 调试控制台 UART | `ttyS-debug` | 启动与现场恢复 | 控制台 IRQ 不得与未确认的安全 IRQ 共享 | 启动日志与恢复流程 |
| 板级管理 I2C | `i2c-bmc` | PMIC、热与风扇设备 | 控制器 IRQ 仅用于告警 GPIO；定义总线恢复 | 地址映射与卡总线测试 |
| 扩展 SPI | `spi-exp` | IMU 或未来传感器 | 每设备 CS；数据就绪 GPIO 用线程化 IRQ | 模式、频率与数据就绪所有权 |
| 非安全 GPIO | `gpio-exp` | LED、在位检测与服务输入 | GPIO IRQ 必须明确边沿与去抖策略 | pinmux 与去抖评审 |
| Linux 看门狗 | `watchdog0` | Linux 健康监督 | 文档化超时与预超时 IRQ；不能替代 MCU-SAFETY | 重启与启动状态证据 |

任何 Linux GPIO、CAN、UART、SPI 或看门狗资源都不得被描述为独立 `MCU-SAFETY` 路径的替代品。

## 4. 电源与安全边界

1. 电池输入在 Linux 或运动负载之前进入受保护的电源级。
2. Linux 电源与运动电源分开熔断，重启 Linux 不必重新使能执行器。
3. 每个运动 MCU 有明确的欠压与复位状态：输出禁用、适用处施加制动、重启后报告故障。
4. `MCU-SAFETY` 控制硬件使能链。有效的 Linux 命令可以请求运动，但不能闭合该链。
5. 急停、通道不一致、看门狗到期、驱动故障和 MCU 心跳丢失都产生锁存抑制，直到文档化的人工复位。
6. CAN 隔离、屏蔽终端与机壳接地由电气设计评审确定；软件不得推断它们。

## 5. V0.1 节点与 CAN 分配

| 节点 | ID | CAN 角色 | 重置域 |
| ---|---:|---|---|
| Linux 网关 | `0x01` | 主机网关与诊断；绝不是安全权威 | Linux 板 |
| `MCU-BASE` | `0x10` | 牵引、编码器、升降与底盘遥测 | 底座运动 |
| `ARM-L-CTRL` | `0x11` | 左七轴机械臂控制 | 左机械臂 |
| `ARM-R-CTRL` | `0x12` | 右七轴机械臂控制 | 右机械臂 |
| `TOOL-L-CTRL` | `0x13` | 左末端执行器控制 | 左工具 |
| `TOOL-R-CTRL` | `0x14` | 右末端执行器控制 | 右工具 |
| `MCU-SAFETY` | `0x1F` | 安全状态与抑制诊断 | 独立安全 |

这些节点 ID 是逻辑 V0.1 标识符，不是最终仲裁 ID。保留的 `0x1F` 安全节点必须保持 STOP 与
故障流量的最高协议优先级。完整仲裁映射、位速率和物理 CAN 控制器仍是实现闸门。

## 6. BSP 实现前需补充的多节点 CAN 契约

现有 MCU Wire V1 定义了帧语义，但没有冻结完整的多节点地址规划。因此 BSP V0.1 要求：

- 每个控制器域的唯一节点 ID 和一个保留的 Linux 网关 ID；
- STOP、命令、确认、遥测和故障帧的优先级与标识符分配；
- 每节点独立的序号空间与启动 ID；
- 发现、心跳超时、bus-off 恢复与重复命令规则；
- 描述每个节点离线或重启时安全结果的矩阵。

`kernel/wbcan` 仍是虚拟回归设备。它不能作为物理 CAN 吞吐、延迟、EMC 或总线恢复的证明。

## 7. 数量冻结后的执行阶段

1. **BSP-1 板卡选型：**选择一块 Linux 板并记录 SoC、内存、存储、电源输入、CAN 接口、内核基线和启动介质。
2. **BSP-2 控制器选型：**选择 `MCU-BASE` 和 `MCU-SAFETY` 部件，然后记录厂商机械臂/工具控制器接口、时钟、flash/RAM、CAN 外设、复位、编程和看门狗资源。
3. **BSP-3 电气与设备树：**冻结原理图网络名、pinctrl、时钟、调节器、GPIO 极性、IRQ 所有权与 CAN 终端。
4. **BSP-4 启动镜像：**为单一 Linux 板产出可复现的 bootloader、内核、DTB、rootfs、固件包与恢复镜像。
5. **BSP-5 硬件启动调试：**在真实硬件上证明启动、CAN 发现、六个心跳、STOP、MCU 复位、Linux 重启与独立急停。

## 8. 冻结闸门

只有在以下所有项都有 Owner 和证据引用时，本包才可从提案进入实现：

- Linux 板型号、SoC、启动介质与目标 Linux 内核版本；
- CAN 控制器/PHY 型号、晶振、位速率与终端；
- MCU 部件号、时钟速率、flash/RAM、重置域与编程路径；
- 原理图网络名、连接器引脚、电源轨与保险丝限值；
- 设备树资源映射、pinctrl 组、GPIO 极性与 IRQ 所有权；
- MCU 节点 ID 与 CAN 仲裁规划；
- 由安全 Owner 评审的急停与安全使能真值表；
- 可复现的交叉编译、镜像组装与恢复说明。

在物理选型与启动调试闸门关闭之前，BSP 状态为
`LOGICAL_TOPOLOGY_FROZEN_PHYSICAL_BRINGUP_PENDING`。
