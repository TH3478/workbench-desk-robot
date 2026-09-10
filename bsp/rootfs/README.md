# Rootfs 与服务

rootfs 必须让单 Linux 板启动到非动作状态。ROS 2、CAN 网关、诊断与更新服务是独立的 systemd 单元，带显式依赖与受限的重启行为。

必需的服务边界：

- `robot-can-gateway`：SocketCAN 入口/出口与六域健康；
- `robot-safety-monitor`：`MCU-SAFETY` 的只读镜像，绝不是安全权威；
- `robot-device-manager`：枚举、固件兼容性与诊断；
- `robot-camera-head`：串口绑定的 D435 ROS 2 输入，无控制权威；
- `robot-evidence-logger`：带轮转限制的仅追加本地证据；
- `robot-update-agent`：签名捆绑包验证与回滚。

任何服务不得在启动期间使能执行器。MCU 固件捆绑包缺失或不兼容时，必须让所有运动域保持抑制。

`camera-head-deployment.yaml` 是失败即拒绝的相机安装清单。入库的 systemd 单元是模板，不是目标就绪的断言。冻结 JetPack/L4T、ROS 2、`librealsense2` 与 `realsense2_camera` 版本，并在使能前替换为已购单元的序列号。D435 是多接口 USB 设备，因此通过 librealsense 序列选择器与厂商 udev 规则绑定它；不要为它的 `/dev/video*` 节点创建一个共享符号链接。
