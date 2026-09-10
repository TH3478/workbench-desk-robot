# Robot BSP 头部相机 V0.1

注重成本的原型在头部使用一个 Intel RealSense D435 RGB-D 相机。它在遮挡研究证明有必要之前，
无需购买两个腕部相机，即可提供工作区观测与深度。

D435 是 USB 设备。Linux BSP 不需要新的机器人专用内核驱动：内核边界是 `uvcvideo`/V4L2，
深度处理使用 `librealsense2`，ROS 2 集成使用 `realsense2_camera`。精确包版本必须匹配选定的
JetPack 版本，在该兼容性测试运行之前不冻结。

## 集成链

```text
RealSense D435 -> USB 3 -> uvcvideo / V4L2 -> librealsense2
              -> realsense2_camera -> validated observation adapter
              -> evidence/event path
```

相机节点发布传感器数据；它不直接产生经验证的任务完成。标定、时间戳质量、帧身份与观测验证
仍是显式边界。

D435 是复合 USB 设备，暴露多个视频接口。因此 BSP 通过其 librealsense 序列号选择它，并使用
冻结包中的厂商 udev 规则。禁止为整个相机设置单一的 `/dev/video*` 别名，因为多个接口会争抢
同一符号链接。

## 启动调试闸门

1. 确认采购序列号、USB 3 拓扑、线缆保持与持续带宽，与其他 Jetson USB 设备一起。
2. 对照头部支架确认 D435 物理包络。现有模拟 `camera_body` 几何不是供应商图纸。
3. 记录彩色/深度模式、丢弃计数、时间戳来源与 CPU/GPU 成本。
4. 生成真实内参与机器人外参；绝不复制 Gazebo 内参。
5. 在使用帧作为物理证据前验证低光照、反光表面、最小距离、遮挡与相机掉线。
6. 只有当测量任务覆盖在头部运动与单一 D435 下无法达到验收阈值时，才增加腕部相机。

在这些闸门关闭之前，状态为
`RECOMMENDED_SELECTION_PHYSICAL_CAMERA_EVIDENCE_NOT_EXECUTED`。
