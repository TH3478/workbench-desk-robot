# 机械臂驱动

厂商 SDK 封装 → `ros2_control` 硬件接口。

每个机械臂型号一个子目录，例如 `franka/`、`ur/`、`custom/`。

**待实现**：一个 `hardware_interface::SystemInterface` 子类，与真实机械臂 SDK
通信，并发出与 Gazebo 仿真相同的 `action_result`。关节限制、安全限制和
启动调试序列也放在这里。

交叉编译说明（针对 ARM 板卡）：`docs/hardware/cross_compile.md`
