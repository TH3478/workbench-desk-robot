# Linux BSP 输入

所选原型目标是 Jetson Orin Nano Super 8GB。载板确认后，本目录将存放目标内核配置、设备树源码、补丁与构建元数据。

物理镜像被称为可复现之前所需文件：

- 带精确 JetPack/L4T 内核版本的 `kernel-config`；
- 供电、pinctrl、CAN、UART、I2C、SPI、USB 与以太网的设备树源码；
- 带上游基线与校验和的补丁清单；
- 编译器/工具链清单；
- 构建命令与输出 hash。

在载板原理图提供引脚与 IRQ 号之前，任何 DTS 节点不得声称物理 GPIO、时钟、稳压器或 CAN 控制器。
