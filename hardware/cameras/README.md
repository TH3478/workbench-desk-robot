# 相机

相机集成，为经过验证的 `observation.schema.json` 生产者提供数据。

已选原型基线：
- 一个头部安装的 Intel RealSense D435，经 USB 3 连接；
- Linux `uvcvideo`/V4L2 内核边界；
- `librealsense2` 用户空间深度处理；
- ROS 2 `realsense2_camera` 设备节点；
- 确切的序列号、码流模式、兼容 JetPack 的包版本和物理标定在硬件到位前
  保持 `NOT_EXECUTED`。

其他集成：
- USB 相机（V4L2 / OpenCV）
- 事件相机（为将来预留的桩）

**待实现**：一个 ROS 2 节点，从设备读取数据，并以与 Gazebo 仿真相同的
`observation.schema.json` 契约发布 `/observations`。

标定文件放在 `cameras/calibration/`。Gazebo 的内参与坐标系几何不是有效的
物理标定。BSP 选型和闸门定义于 `bsp/sensors/camera-head.yaml` 和
`docs/architecture/robot-bsp-camera-v0.1.md`。
