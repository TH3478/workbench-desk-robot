# 机器人 BSP 工作区

本目录是 `docs/architecture/robot-bsp-selection-v0.1.md` 所选单 Linux 板机器人 BSP 的实现归宿。

仓库当前阶段是逻辑冻结加原型选型。它不是可启动镜像，也还不包含目标板的设备树。

规划布局：

```text
bsp/
  board-manifest.yaml       selected board, SoC, power and interfaces
  linux/                     kernel config, patches and DTS once board is frozen
  boot/                      bootloader, boot arguments and recovery notes
  rootfs/                    reproducible rootfs manifest and system services
  firmware/                  versioned MCU images and compatibility manifest
  sensors/                   selected sensor interfaces and integration boundaries
  validation/                bring-up scripts and raw evidence references
```

改动板卡、控制器或固件清单前，运行 `python bsp/validation/validate_manifests.py`。校验器检查领域身份，并在证据挂接之前让物理启动调试结果保持失败即拒绝。

公开候选基线及其来源链接记录在 [`public-candidate-selection.md`](public-candidate-selection.md)。它让下一次采购与集成决策具体化，同时不把公开文档当作供应商批准或物理证据。

原型相机基线是头上安装的一台 Intel RealSense D435，走 USB 3。Linux 使用标准 `uvcvideo`/V4L2 路径，内核之上是 `librealsense2` 与 ROS 2 的 `realsense2_camera` 包。见 `sensors/camera-head.yaml` 与 `../docs/architecture/robot-bsp-camera-v0.1.md`。精确的 JetPack 兼容包版本、序列身份、流模式、标定与物理证据仍是未闭合的启动调试闸门；腕部相机推迟到实测遮挡证明单头相机不足之后。

在从所选载板原理图与厂商文档取得出处之前，不要添加引脚号、IRQ 号、寄存器地址或生产供电限值。
