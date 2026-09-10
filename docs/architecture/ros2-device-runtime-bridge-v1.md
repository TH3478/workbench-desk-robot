# ROS 2 DeviceRuntime 桥接 V1

状态：Issue #230 的**已实现的首个切片**。桥接是围绕既有主机硬件运行时的一条
有界、只读 ROS 2 边界；它不是物理 CAN、MCU、执行器或硬实时验证。

## 范围与所有权

首个切片使用 Issues #55 与 #229 中已合并的 `SafeCANBus` 与
`SocketCANTransport` 实现：

```text
SocketCANTransport (one synchronous AF_CAN descriptor)
        -> SafeCANBus (CAN Wire V1 validation and correlation)
        -> one DeviceRuntime (worker, cancellation and bounded planes)
        -> RuntimeBridgeCore (bounded read-only drain and allowlist)
        -> ROS 2 LifecycleNode / String topics
        -> selected RMW (Fast DDS in the deployment profile)
```

`SafeCANBus` 暴露它既有的 `DeviceRuntime`，因此桥接复用该对象，绝不把它包进
第二个运行时。测试支持注入一个不带运行时的普通 `DeviceAdapter`；此时桥接以
配置好的容量恰好创建一个 `DeviceRuntime`。适配器仍然负责阻塞设备 I/O，而运行时
仍是其 worker、取消、生命周期调用与数据平面的唯一 Owner。

桥接没有命令 publisher、service、action server、socket 句柄、WorldState 写入者
或 HTTP 路径。它只能发布经过验证的遥测、被接受的 ACK/动作结果记录与有界健康
记录。急停、看门狗、安全使能与直接运动权威仍归 DDS 之外的硬件、MCU、Safety 与
Motion Owner。

## 生命周期行为

`create_lifecycle_node()` 只在工厂被调用时才导入 `rclpy`；在普通 Python 环境中
导入 `workbench.hardware` 仍然合法。节点使用一个 `LifecycleNode`、三个生命周期
publisher 和一个已取消的定时器：

| 转换 | 桥接动作 | 资源规则 |
| --- | --- | --- |
| configure | 构造适配器/运行时，创建 publisher 与定时器 | 不打开 SocketCAN，也没有 worker |
| activate | 调用既有运行时 `start(background=True)` 并重置定时器 | 每个适配器实例一个运行时 worker |
| deactivate | 取消定时器，调用有界运行时关闭，丢弃该实例 | 超时返回转换失败 |
| cleanup | 释放非活动运行时并销毁 ROS 实体 | 回到未配置状态 |
| shutdown | 执行终结的有界清理并销毁 ROS 实体 | 终结成功是幂等的 |

由于当前的 `DeviceRuntime` 刻意采用终结清理语义，之后的 `inactive -> active`
转换会通过工厂创建全新的适配器与运行时。这防止陈旧的关联窗口、队列、文件
描述符与 worker 被复用。失败的启动或关闭被保留为失败的转换；绝不静默报告为
成功。

`ingress_sequence` 的作用域是一次适配器/运行时激活。全新激活有意重置底层适配器
的来源序号；本切片不声称跨重启的序号连续性或全局唯一证据引用。消费方必须把
生命周期重配置视为新的来源纪元，且不得从重复的序号推断连续性。

节点不构造自动定容的 executor。调用方使用 `create_bounded_executor(config)`，
它创建一个具有配置的固定线程数（默认 `2`，最大 `8`）的 `MultiThreadedExecutor`。
定时器位于互斥回调组中，其回调只排空已产出的记录；它从不执行硬件 I/O 或等待
ACK。

## 有界数据与发布平面

来源 `DeviceRuntime` 已经拥有有界的命令/ACK、遥测、健康与外部投影平面。桥接不
增加适配器本地 worker 或无界队列。它的定时器每个 tick 最多处理
`max_records_per_tick` 条记录（默认 `32`，最大 `1024`），并在路由外部投影之前给
健康一个小配额。

被接受的外部记录被路由到独立的 ROS 发布平面：

| 平面 | Topic | QoS | 含义 |
| --- | --- | --- | --- |
| telemetry | `/workbench/device/telemetry` | best-effort, keep-last, depth 16, volatile | 面向新鲜度的设备遥测；来源丢弃仍被计数 |
| ACK/action result | `/workbench/device/ack` | reliable, keep-last, depth 16, volatile, 100 ms deadline | 仅接受已关联的 ACK/STOP_ACK；缺失 ACK 绝不算成功 |
| health/provenance | `/workbench/device/health` | reliable, keep-last, depth 32, volatile | 有界运行时诊断与桥接生命周期/投影失败 |

来源适配器在条目进入外部投影之前完成验证与关联。桥接防御性地拒绝一切不是
不可变、已暴露 `CanExternalRecord` 的记录，且只接受已知的入站帧类型（`ACK`、
`STOP_ACK` 与 `TELEMETRY`）。运行时健康通过带身份游标的快照读取，这样诊断不会
在每次定时器调用时重复发布。桥接本地失败使用各自的有界健康记录容量，绝不允许
递归消耗产生它们的同一个定时器预算。

指标快照报告来源队列深度、可取得时间戳时的最旧年龄、遥测/健康/外部丢弃计数、
桥接健康丢弃、逐平面发布计数、不受支持的记录、序列化失败、publisher 失败、
定时器预算命中与 worker 存活情况。publisher 异常只消耗当前记录并成为一条有界
健康事件；它不能终止运行时 worker。

## 投影与溯源策略

临时 ROS 载体是包含确定性 JSON 的 `std_msgs/msg/String`。它有意不作为冻结的
公共接口的替代。本切片不改动 `interfaces/` 下的任何文件。

外部 JSON 具有 `schema_version` 与 `record_type` 元数据，以及显式的
`EXTERNAL_PROJECTION_ALLOWLIST`。它包含经过验证的来源、接口、入口序号、时钟、
帧身份、协议字段、健康、暴露状态与证据引用。载荷字节渲染为 `data_hex`；socket、
文件描述符、回调、可变设备对象与任意 `device_state` 字段都不能被序列化。健康
JSON 有一份独立的显式 allowlist，覆盖诊断码、观测时间、详情、命令 ID、来源与
有界序号。

序列化使用排序键、紧凑分隔符与 `allow_nan=False`。无效、重复、迟到、无关联、
畸形与未暴露的记录不会作为外部事件发布。它们仍通过来源运行时的有界诊断与接收
结果保持可观测。

## SocketCAN 与部署设置

`create_socketcan_adapter_factory(interface, ...)` 在每次激活时构造新的
`SocketCANTransport` 与 `SafeCANBus`。构造无副作用；描述符只在运行时激活时才
打开。工厂把桥接容量与关闭时序映射进既有的 `CanTransportConfig`，为边界保留
单一事实来源。

部署快照记录：

- ROS domain ID：默认 `42`；由部署设置 `ROS_DOMAIN_ID`；
- RMW 选择：默认 `rmw_fastrtps_cpp`；
- topic 名称与各项 reliability/history/depth/deadline；
- 固定的 executor 线程数及其最大值；
- 命令、遥测、健康与外部容量；以及
- 一份由部署管理的安全 profile。

Fast DDS 由部署选定（`RMW_IMPLEMENTATION` 与既有容器 profile），而不是通过导入
厂商 API 或更改 Python 契约。桥接不修改这两个环境变量中的任何一个，也不声称
安全 profile、DDS 发现会话或网络隔离已经过物理验证。只要保持同样有界的 ROS
边界，测试就可以使用受支持的替代 RMW。

## 验证边界

聚焦单元测试覆盖配置边界、延迟的 ROS 导入、allowlist 序列化、队列饱和、健康
去重、publisher 失败隔离、SafeCANBus 运行时复用、生命周期失败、全新激活与终结
清理。可选的 ROS 测试在 `/opt/ros/jazzy` 可用时演练 Jazzy LifecycleNode 转换
路径与定时器排空。

聚焦测试包括一条本地 ROS topic 发布/接收回环与一条虚拟 SocketCAN 构造路径。它们
为这些路径确立了软件排序、有界生命周期行为、来源元数据与清理。它们不确立：

- 物理 CAN 仲裁或 USB-CAN 电气完整性；
- MCU 接受、看门狗或安全使能行为；
- 执行器或驱动运动；
- 相机/触控、机械臂或安全插件集成；
- PREEMPT_RT 调度或硬实时期限；或
- 生产部署的 CPU/RSS/负载结果。

这些条目保持 `NOT_EXECUTED` 或 Owner 把关状态，直到各自的原始抓包、内核/DDS
配置、标定参考与独立 Owner 评审被记录。相机/触控与驱动/机械臂适配器仍为单独的
后续切片；本桥接不合并它们的硬件代码。
