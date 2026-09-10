# ADR-0005: 统一硬件设备运行时与 DDS 边界

状态：**proposed（提案中）**（Issue #230；需 Linux、Integration、Motion 与 Perception Owner 评审）

日期：2026-08-22

## 背景

仓库有若干规划中的硬件输入输出：SocketCAN、相机与触控设备、机械臂驱动，以及硬件安全桥。它们
各自的 README 目前描述相互独立的适配器，而运行时与后端只有仿真/事件库路径。worker 生命周期、
队列上界、取消、溯源或对外暴露都没有共享契约。Fast DDS 不是当前项目依赖。

内核 `wbcan` 模块是虚拟 SocketCAN 故障注入装置。它不能被改造成硬件框架，DDS 路径也不能替代
独立急停、看门狗或安全使能电路。

## 决策

在稍后、单独评审的实现切片中引入传输中立的 `DeviceRuntime` 边界：

```text
physical device / Linux driver
  -> DeviceAdapter (CAN, camera, touch, arm, safety monitor)
  -> bounded DeviceRuntime (lifecycle, workers, queues)
  -> typed ROS 2 topic/service/action boundary
  -> selected RMW (Fast DDS in the target deployment)
  -> validation + provenance envelope
  -> World Model / ActionResult / evidence sink
  -> read-only external projection
```

### 适配器与运行时所有权

- `DeviceAdapter` 拥有阻塞设备 I/O，并把它翻译成类型化信封。它不写 WorldState、不调 HTTP、
  不暴露原始句柄。
- `DeviceRuntime` 拥有 `configure -> activate -> deactivate -> cleanup`、取消、worker join、
  队列限制、背压与健康计数。
- ROS 2 封装应使用 `rclcpp_lifecycle::LifecycleNode`（或等价的生命周期 API），使生命周期转换
  作为一个幂等操作创建与销毁适配器 worker 和 DDS 实体。失败的激活不得留下半活动的 publisher
  或设备句柄。
- 每台设备的可变状态有一个写者。回调把工作交给有界队列，而不是并发变更状态。
- 桥接、设备与触控实现是同一端口的插件；它们的硬件特定代码留在公共运行时之外。

### 线程与队列

- 每台设备使用一个阻塞 I/O worker 或文档化的有界 worker 池；绝不按消息创建无限线程。
- ROS 2 回调组与固定大小 `MultiThreadedExecutor` 可以分发回调，但 executor 本身不是队列上界。
  实现必须记录其线程数、回调组分配、DDS History/Depth 与适配器队列容量；DDS 回调不得阻塞在
  硬件 I/O 上。
- 命令/ACK、遥测与健康/溯源使用独立的有界数据平面。高频率相机或触控流不得消耗接收驱动 ACK
  所需的队列或 executor 容量。每个平面有固定容量和显式策略：拒绝命令、有界重试，或丢弃旧
  遥测并带深度、丢弃、期限错过与最旧年龄计数。队列满、链路丢失、重复与迟到帧的结果都可观测。
- 关闭先取消生产者，按端口契约排空或拒绝，以期限 join worker，然后销毁 DDS 参与者、定时器与
  文件描述符。超时是错误，不是静默守护进程。

### DDS 与 ROS 2 边界

核心依赖类型化 ROS 2 接口与注入的传输端口，而不是 Fast DDS 符号。部署可以选择 Fast DDS
`RMW_IMPLEMENTATION=rmw_fastrtps_cpp`；另一个受支持的 RMW 必须保持为可能的测试后端。桥接
Task Packet 必须记录 domain ID、topic 或 service 名称、QoS、安全/权限与发现行为。

默认 QoS 策略是显式的，并按数据类别划分：

| 数据类别 | 默认策略 | 失败含义 |
| --- | --- | --- |
| sensor/touch 遥测 | 有界尽力而为，keep-last | 丢弃样本被计数，新鲜度可以过期 |
| command/ACK | 可靠、有界深度、期限与关联 | 无 ACK 即超时/故障，绝不是成功 |
| health/provenance | 可靠、有界且序号检查 | 缺失或倒退的记录视为未就绪 |
| E-stop/watchdog | 硬件/带外权威 | DDS 仅监视，绝不是唯一停止路径 |

### 验证与对外暴露

运行时在发布消费者可见记录之前验证帧类型、来源/接口身份、序号、时钟、载荷与新鲜度。它保留
分发状态、设备状态、验证状态与证据之间的既有区分。只有经过验证的只读遥测与健康才允许直接
框架暴露。命令继续经由受信任的 Motion、MCU 与 Safety Owner；看板与 HTTP API 永不获得设备
写者。

信封在入口之后不可变，包含来源/接口身份、每来源序号、单调时间戳、墙上时钟观测时间、健康状态
与证据引用。时钟回退、序号重置/重启、重复与迟到帧规则是显式的适配器错误或丢弃结果；它们不被
静默归一化。外部投影使用遥测与健康字段的 allowlist。原始 DDS topic、设备句柄与任意
`device_state` 对象绝不直接序列化进 HTTP。

硬件与仿真适配器必须发出相同的消费者契约。模拟传输与 ROS 回环可以证明排序、生命周期与元数据，
但不能证明物理 CAN、执行器、触控安全或硬实时行为。

## 推行

1. 定义传输中立端口与信封，不改变冻结的公共 schema。
2. 增加一个模拟适配器和一个 SocketCAN 适配器，带生命周期、饱和、取消、重复/迟到帧与回调
   失败测试。
3. 增加 ROS 2 桥接与 Fast DDS 部署配置，记录 QoS 与安全设置。
4. 独立增加相机/触控与机械臂插件；不要把它们的启动调试或安全证据与 CAN PR 合并。
5. 只把已验证记录喂给现有事件/证据路径，并通过现有只读投影暴露它们。

## 被否决的替代方案

- **设备直连 HTTP。**绕过验证、溯源与只读边界。
- **每个适配器都用 Fast DDS 类型。**把硬件代码耦合到一种中间件，并使确定性模拟测试更难。
- **一个全局无限线程/队列。**隐藏背压，让嘈杂传感器饿死命令或安全诊断。
- **DDS 作为安全通道。**发现、调度与网络故障不能替代硬件急停/看门狗路径。

## 待决事项

实现 PR 必须与具名 Owner 敲定具体的 ROS 2 消息包、DDS 域/安全 profile、executor 规模、队列
容量与物理设备权限。在此之前，本 ADR 是设计提案，不声称 Fast DDS 或物理硬件已实现。
