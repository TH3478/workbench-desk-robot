# Hardware (#14) - 嵌入式控制层

## 概述

嵌入式工程师 P1 模块。当前实现 HW1 的展开 URDF 参数提取；HW2-HW7 仍是后续任务。

## 组件

- **HW1**：URDF Parser（已实现）
- **HW2**：受限 CAN 传输适配器（主机侧，经 fake-transport 验证）
- **HW3**：Motor Feedback Parser（规划中）
- **HW4**：PID Controller（规划中）
- **HW5**：Sensor Simulator（规划中）
- **HW6**：Realtime Executor（规划中）
- **HW7**：Integration Test Framework（规划中）

## 架构

```
Kernel (ROS 2) 
    ↓ (versioned messages)
Hardware Layer
    ├─ URDF Parser → Motor Config
    ├─ DeviceRuntime
    │   └─ CAN DeviceAdapter ↔ injected CAN Wire V1 transport
    ├─ Motor Feedback ← Encoders
    ├─ PID Controller → Motor Command
    ├─ Sensor Simulator ← Gazebo
    └─ Realtime Executor (100Hz)
```

## SocketCAN 入口（Issue #229）

主机适配器可以由标准库 `workbench.hardware.SocketCANTransport` 支撑，它拥有一个 Linux `AF_CAN`/`CAN_RAW` 描述符。它使用内核过滤器、受限的 `poll`/`recvmsg`、Classic CAN 帧校验、`SO_TIMESTAMPNS` 与 `SO_RXQ_OVFL` 元数据。`DeviceRuntime` 仍是生命周期、worker 与受限数据面的唯一 Owner；适配器不新增队列或 worker。

验证通过的入站 ACK、STOP_ACK 与遥测帧产生不可变的只读 `CanExternalRecord` 值。这些记录包含来源/接口、入口序列、DLC、原始 ID 标志、时间戳、协议字段、健康与证据引用。无效、重复、迟到、无关联、错误与停机后的帧在运行时活跃期间仍通过接收结果与诊断可观测，但不能进入外部队列或成为对外暴露的完成事件。

可复现的虚拟探针是：

```bash
python3 kernel/wbcan/test_socketcan_ingress.py wbcan0 \
  --report /tmp/wbcan-socketcan-ingress-report.json
```

它输出 `PASS`、`FAIL` 或 `NOT_EXECUTED`，并把物理 CAN、MCU、执行器与硬实时证据标记为 `NOT_EXECUTED`。见 [`SocketCAN 架构契约`](../../docs/architecture/host-can-transport-v1.md) 与 [物理启动调试流程](../../docs/hardware/can_bring_up.md)。

## HW1 用法

解析器消费展开后的 URDF XML，而非 Xacro 源码。生成官方 UR5e 描述并提取六个臂关节：

```bash
source /opt/ros/jazzy/setup.bash
xacro /opt/ros/jazzy/share/ur_description/urdf/ur.urdf.xacro \
  ur_type:=ur5e name:=ur5e > /tmp/ur5e.urdf
python3 libs/hardware/urdf_to_motor_config.py /tmp/ur5e.urdf \
  --joint shoulder_pan_joint \
  --joint shoulder_lift_joint \
  --joint elbow_joint \
  --joint wrist_1_joint \
  --joint wrist_2_joint \
  --joint wrist_3_joint
```

`max_torque_nm`、速度与位置限位来自每个 URDF `<limit>`。`mechanical_reduction` 仅在显式 URDF transmission 声明时填充。`null` 表示受控输入未声明减速比；不得用猜测的物理齿轮箱速比替换它。

经审计的官方来源提取命令、包版本、hash 与生成的电机配置记录在 [`docs/hardware/hw1-ur5e-extraction.md`](../../docs/hardware/hw1-ur5e-extraction.md)。

## P1 交付物

- HW1：来自展开 URDF 的电机配置（已实现）
- HW2：由统一 `DeviceRuntime` 承载的受限主机 CAN `DeviceAdapter`（已实现；不声称物理传输）
- HW3：CAN 反馈解析（规划中）
- HW4：PID 控制器（规划中）
- HW5-6：传感器仿真 + 100Hz 实时控制回路（规划中）
- HW7：完整集成测试（规划中）

CAN 适配器不创建自己的 worker 或生命周期。`DeviceRuntime` 拥有 `configure -> activate -> deactivate -> cleanup`、取消、单一 I/O worker、受限的命令/ACK、遥测与健康面，以及订阅者快照。`SafeCANBus` 的兼容生命周期方法委托给该 Owner。精确的所有权与证据边界见 [`host-can-transport-v1.md`](../../docs/architecture/host-can-transport-v1.md)。
