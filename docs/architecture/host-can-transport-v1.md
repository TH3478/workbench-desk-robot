# Host CAN 传输适配器 V1

状态：Issue #55 与 #229 的**有界主机适配器与 SocketCAN 入口契约**。

`SafeCANBus` 是 ADR-0005 定义的统一 `DeviceRuntime` 的 CAN `DeviceAdapter`。它拥有注入的
阻塞传输端口与 CAN Wire V1 协议状态，但不拥有生命周期、worker、队列或订阅者注册表。
`SocketCANTransport` 是该端口基于标准库 `AF_CAN`/`CAN_RAW` 的实现：它拥有一个原始文件
描述符，应用内核过滤器，一次翻译一个有界的 `struct can_frame`。构造绝不打开 SocketCAN 或
任何硬件设备，成功的排队或写入也绝不意味着 MCU 接受了命令。

## 统一运行时所有权

`DeviceRuntime` 是以下事项的唯一 Owner：

```text
configure -> activate -> one I/O worker -> deactivate -> cleanup
                     |                    |
             cancellation + join     bounded data planes
```

遗留的 `SafeCANBus.start()`、`service_once()` 和 `shutdown()` 方法是对该运行时的兼容性委托。
`CanLinkState` 只报告 CAN 链路与协议健康；它不是第二个生命周期状态机。适配器实现
`configure`、`activate`、`poll`、`deactivate` 和 `cleanup`，并在更新协议关联状态时使用运行时
的一把锁。

运行时拥有四个独立的有界平面：

| 平面 | Owner 与策略 | 可观测边界 |
| --- | --- | --- |
| command/ACK | 固定命令队列；容量满时拒绝普通命令；STOP 优先并抢占 | 类型化背压、关联与超时诊断 |
| telemetry | 固定入口队列；重复/过期帧在分发前被拒绝 | 遥测深度与丢弃计数 |
| health/provenance | 固定诊断队列，最旧记录淘汰 | 错误与健康丢弃计数 |
| external projection | 不可变只读记录，最旧记录淘汰 | 外部深度与丢弃计数 |

订阅者注册与回调快照分发也属于运行时。回调异常被隔离并成为健康记录。关闭时先取消生产者，
拒绝新命令，清空排队工作，按期限 join 唯一 worker，然后才让适配器关闭端口。join 超时会让
清理保持挂起并返回 `False`；后续调用可以重试。终止清理结果被保留，因此生命周期标签不会被
误认为成功清理。

## 安全与所有权边界

- `firmware/mcu/core/frame_codec.[ch]` 仍是二进制 Wire V1 布局的权威。Python 适配器只复制
  帧跨主机边界前所需的有界入口验证；它不改变线上数字或逻辑协议。
- 适配器只接受标准 11 位、非远程、非错误、DLC-8、且带已知 Wire V1 标识符和完整跨字段语义
  的帧。
- 普通命令有一条有界的在途请求。成功的本地传输分发开始确认期限；它不是命令完成。
- 重试尝试保持命令关联并递增线上重试计数。重试预算有限。耗尽后清空普通流量并发出关联的
  STOP 请求。
- STOP 抢占排队中和挂起的普通流量。STOP 确认超时、STOP 被拒绝、bus-off 与链路丢失使适配器
  在显式成功恢复之前无法发送普通命令。
- 恢复清空排队中和挂起的故障前帧。它从不回放过期流量。调用方必须建立任何更高层的会话/启动
  闸门。
- 遥测使用协议半区间排序规则。重复与过期快照被忽略，而有效故障遥测在显式传输恢复之前禁用
  普通流量。

每个有效的入站结果携带不可变的 `CanTransportEnvelope`，含 `source`、`interface`、适配器入口
序号、单调与墙上时钟观测时间、当前链路健康、经过验证的线上帧和不可变证据引用。SocketCAN
记录还保留原始 CAN ID、DLC、内核 `SO_TIMESTAMPNS` 时间戳、提供时的 `SO_RXQ_OVFL` 计数器，
以及主机观测时钟。适配器入口序号绝不会被链路恢复重置；独立的 MCU 遥测序号遵循 Wire V1
半区间排序规则。非有限或倒退的时钟观测是可观测的失败即拒绝入口错误，不会被静默归一化。
桥接器可以把信封映射到稍后的类型化 ROS 2/事件契约，而不暴露原始设备句柄。

## SocketCAN 入口契约

`SocketCANTransport` 在 `decode_can_frame()` 运行之前执行以下边界检查：

1. 恰好打开一个 `AF_CAN`、`SOCK_RAW`、`CAN_RAW` socket 并绑定到配置的接口。不创建适配器
   本地 worker、队列、重试循环或第二个生命周期状态机。
2. 安装类型化的 `CAN_RAW_FILTER` 条目。过滤器在其掩码中包含选定的 standard/extended/RTR/
   error 标志位，因此帧类型变体不可能意外匹配。可选的 CAN 错误掩码保持与协议帧过滤器分离。
3. 启用有界的接收缓冲、`SO_RXQ_OVFL` 以及（默认）`SO_TIMESTAMPNS`。使用 `poll()` 和非阻塞
   `recvmsg()`，每次调用一条记录。`MSG_TRUNC`、`MSG_CTRUNC`、畸形辅助数据、短记录、CAN-FD
   尺寸记录、无效 DLC 和矛盾的 raw-ID 标志成为可观测的帧拒绝。
4. 在不可变的 `CanFrame` 中保留 standard、extended、RTR 与 error 标志。错误帧绝不进入
   Wire V1 解码；bus-off 错误使适配器进入 `BUS_OFF` 并清空挂起工作。
5. 只有标准、非 RTR、非错误、DLC-8、且带已知 Wire V1 ID、版本、保留字节和有效跨字段的帧
   才进入协议/运行时边界。ACK、STOP_ACK 和遥测是仅有的入站类型。

`CanExternalRecord` 是唯一面向外部观察者的投影。它包含来源/接口身份、入口序号、帧元数据、
时间戳与来源、事件/协议字段、健康、证据引用，以及该帧是否有效并被暴露。它只包含不可变的
`bytes`；绝不包含 socket、文件描述符、回调、debugfs 路径或写句柄。被接受的 ACK/STOP_ACK/
遥测记录可以被暴露。重复、迟到、无关联、畸形、错误与关闭后的记录保持可观测拒绝（运行时活动
期间通过 `CanReceiveResult` 和有界诊断），但被刻意排除在外部投影队列之外，且不能声称完成。

## 并发与背压

运行时的数据平面、订阅者注册表、错误计数器和适配器的关联窗口由一把共享锁保护。订阅者分发
使用不可变快照，因此回调可以在不改变当前迭代的情况下订阅或退订。回调异常被隔离并记录。

命令、遥测、健康、外部投影、订阅者与关联容量由 `CanTransportConfig` 固定。队列饱和返回
类型化背压结果并递增有界丢弃计数。`shutdown()` 停止运行时 worker，清空挂起工作与外部记录，
按调用方期限 join worker，然后才关闭注入的端口。与关闭竞争的接收返回的帧不被分发或发布。
不存在适配器本地后备 worker 或无限回调路径。

## 证据与恢复边界

`kernel/wbcan/test_socketcan_ingress.py` 是确定性虚拟探针。它使用真实 SocketCAN fd 与第二个
对端 socket 发送 Wire V1 命令、返回 ACK、返回遥测、回放重复 ACK，并发送错误 DLC 帧。它写出
带 `PASS`、`FAIL` 或 `NOT_EXECUTED` 的 `socketcan-ingress-report-v1`、精确外部记录和清理
检查。`--require-pass` 只适用于具备虚拟前置条件的特权 CI 任务。

恢复刻意分两阶段：CAN 管理员或监督者恢复接口，然后配置的传输恢复探针确认该操作。
`SafeCANBus.recover()` 清空故障前命令与关联状态；绝不回放它们。物理适配器、固件/MCU、
执行器、电气总线、PREEMPT_RT 调度器或硬实时期限在本虚拟探针范围之外，必须保持
`NOT_EXECUTED`，直到物理 HIL 流程记录所需原始证据并通过 Owner 评审。

## 证据限制

模拟传输与模拟时钟测试只证明适配器状态转换、验证、有界重试和生命周期行为。SocketCAN 探针
在返回 `PASS` 时证明虚拟 Linux SocketCAN 入口与投影路径。两类证据都不证明物理 CAN 仲裁、
MCU 接受、执行器运动、PREEMPT_RT 行为、电气完整性或硬实时期限。
