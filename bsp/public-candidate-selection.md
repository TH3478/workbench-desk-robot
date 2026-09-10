# 公开候选选型包

状态：候选基线基于公开厂商与上游来源调研；不是已批准的 AVL、安全认证或物理启动调试结果。

## 候选矩阵

| 领域 | 公开候选 | 集成决策 | 证据与未闭合项 |
|---|---|---|---|
| Linux 板与载板 | NVIDIA Jetson Orin Nano Super Developer Kit 8GB 及其官方载板 | 原型启动调试使用厂商载板；推迟定制载板 | [Jetson Linux](https://developer.nvidia.com/embedded/jetson-linux-r3640)。在 DTS 冻结前，从板卡手册确认所购板卡版本、电源输入、pinmux 与 IRQ 映射。 |
| 原型 Linux 供电 | 官方载板的厂商推荐稳压 DC 输入 | Linux 供电与运动供电分开熔断 | 仍需要板卡手册、实测持续负载与热证据；任何功率额定值不得提升为生产规格。 |
| CAN 主机接口 | PEAK PCAN-USB FD，隔离式 USB CAN-FD 适配器 | 通过厂商 Linux 驱动使用 SocketCAN；`can0` 作为逻辑总线 | 采购前确认精确 SKU、驱动版本、隔离额定值、比特率、终端与线束。 |
| JetPack/L4T | JetPack 6.2.1 / Jetson Linux 36.4.4 | 钉定其为原型软件候选 | NVIDIA 发布页确认映射关系；下载 hash、模组修订与内核配置 hash 仍待提供。 |
| 内核 | NVIDIA L4T 36.4.4 厂商内核基线 | 仅在精确源码包钉定后合并 `bsp/linux/robot_bsp.config` | 内核源码归档、工具链摘要、生成的 `.config`、Image/modules 与 DTB hash 仍待提供。 |
| 主机 rootfs | 适用于 L4T 36.4.4 的 NVIDIA Jetson Linux rootfs | 以已入库的 systemd 服务集启动于运动抑制状态 | 包锁、固件捆绑包、恢复镜像与回滚记录仍待提供。 |
| ROS 2 | JetPack Ubuntu 22.04 主机上的 ROS 2 Humble；Ubuntu 24.04 仿真/容器任务保留 Jazzy | 在受支持镜像得到证明之前，避免声称 Jetson 主机上有原生 Jazzy 二进制 | 按部署镜像冻结 ROS 发行版，并对 `xarm_ros2` 运行兼容性测试。 |
| 七轴机械臂 | 带厂商控制器与 `xarm_ros2` 的 UFACTORY xArm 7 | 两个臂域的候选；以厂商控制器为模块边界 | 精确 xArm 7 SKU、固件、供电、安全 I/O、网络模式与供应商报价仍未闭合。上游 ROS 2 包为 BSD-3-Clause。 |
| 末端执行器 | Robotiq 2F-85 描述与厂商控制器 | 工具控制置于 `TOOL-L-CTRL`/`TOOL-R-CTRL` 之后 | 与供应商确认夹爪 SKU、安装、供电、控制协议与安全停止行为。 |
| PCB U2 | Mean Well RSD-300-12 作为 300 W 级候选 | 在精确输入范围、隔离、封装、爬电距离、热与 EMI 评审通过之前，不要替换原理图占位 | [公开数据手册](https://www.meanwell.com/Upload/PDF/RSD-300/RSD-300-SPEC.PDF)。系统总线电压必须匹配精确的 RSD-300 变体；输入范围、降额、安装、保护与 AVL 批准仍未闭合。 |
| 安全硬件 | 既有独立 STM32G0B1 `MCU-SAFETY` 边界加双通道外部抑制链 | 保持硬件权威在 Linux 与 ROS 之外 | 仍需安全 Owner 评审、危害分析、认证组件、布线以及实测跳闸时间证据。 |

## 第三方状态

- NVIDIA JetPack/L4T 是厂商发行版，发布镜像前必须按适用的再分发条款评审。
- `xArm-Developer/xarm_ros2` 在公开 `humble` 分支上为 BSD-3-Clause：
  <https://github.com/xArm-Developer/xarm_ros2>。
- RLSOK 在公开仓库中为 Apache-2.0：
  <https://github.com/realitywarden/rlsok>。
- RLSOK 文档记载的官方集成是 Ubuntu 24.04/Jazzy 上的 UR5e；xArm 7 支持是通用协议集成，未经厂商认证。
- 当前证据集中 MonoSim 没有可验证的公开仓库或许可证；在其维护者提供链接与书面条款之前，它仍是受邀的外部集成。

## 闭合规则

这些候选让下一步工程动作具体化，但它们不闭合现有发布闸门。在挂接所指证据之前，不要把 `REVIEW_REQUIRED`、`BLOCKED` 或 `NOT_EXECUTED` 状态改为 `PASS`。

逐步证据闸门维护在 [`validation/bringup-closure-checklist.md`](validation/bringup-closure-checklist.md)。
