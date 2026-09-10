# MCU CAN HAL 边界 V1

状态：Issue #180 的**平台无关桥接已实现；物理目标传输 NOT_EXECUTED**。

该边界把 `firmware/mcu/core/hal.h` 中的原始 CAN 控制器信封连接到已冻结的 MCU CAN Wire V1
编解码器。它消除了 CAN 仲裁标识符与逻辑命令标识符之间此前的歧义，而不改变任何 Wire V1 数字
或载荷字节。

## 标识符边界

两个标识符有不同的宽度和权威：

| 名称 | 宽度 | 位置 | 含义 |
| --- | ---: | --- | --- |
| `arbitration_id` | 11 位 | CAN 信封 | 选择一种 Wire V1 帧类型与仲裁优先级。 |
| `command_id` | 16 位 | 载荷字节 1..2 | 关联普通命令或 STOP，并选择其不相交的 ID 分区。 |

因此 STOP 使用仲裁 ID `0x080`，而其载荷命令 ID 是 `0x8000..0xffff`。目标 HAL 绝不能把逻辑
命令 ID 写入 CAN 仲裁寄存器。

`hal_can_frame` 包含：

- `arbitration_id`，至多为 `0x7ff`；
- `dlc`；
- 一个显式的 flags 字节，用于 extended-ID、RTR、error 与 CAN FD 元数据；以及
- 八个 Classic CAN 数据字节。

flags 不是 C 位域。每个目标都必须把其控制器状态显式翻译成声明的掩码，避免编译器特定布局。

## 唯一编码权威

`mcu_can_bridge_encode()` 调用 `mcu_frame_encode()`，然后构造原始标准 Classic CAN 数据信封。
`mcu_can_bridge_decode()` 在调用 `mcu_frame_decode()` 之前验证原始信封。HAL 中不存在第二个
帧类型或载荷映射。

只有全部以下条件通过时解码器才发布逻辑帧：

1. flags 恰为 `HAL_CAN_FRAME_FLAG_NONE`；
2. `arbitration_id <= 0x7ff`；
3. DLC 恰为 8；
4. 仲裁 ID 是五个冻结 Wire V1 ID 之一；并且
5. 版本、保留字节、枚举值、ID 分区和跨字段语义全部通过现有编解码器。

Extended、RTR、error、CAN FD、未知标志、越界 ID、未知 ID、错误 DLC 和畸形 Wire V1 输入产生
有界桥接拒绝记录。它们不分发状态机事件、不创建 ACK、不进入回放历史，也不刷新软件看门狗。
若执行已激活，畸形流量无法维持它；只有有效的序号为新普通命令才能刷新现有期限。

## MCU 入口方向与路由

MCU 入口只接受 `STOP` 与普通 `COMMAND` 类型：

```text
raw HAL envelope
  -> strict envelope validation
  -> mcu_frame_decode()
  -> STOP: mcu_watchdog_receive_stop()
  -> COMMAND: mcu_command_dedup_receive()
  -> response: mcu_frame_encode() -> hal_can_send()
```

STOP 分支在普通命令处理之前测试。它绕过可信普通会话闸门和固定回放窗口；普通去重对象缺失或
损坏也无法压制该路径。ACK、STOP_ACK 和遥测帧是有效的 MCU 发出编码，但若被本 MCU 入口接收
则作为错误方向流量拒绝，绝不触及安全状态。

`mcu_can_bridge_poll()` 每次调用至多消费一个 HAL 帧。目标是桥接器、状态机、看门狗和去重对象
的单一写者；它必须串行化 ISR/任务所有权并配置控制器过滤器/FIFO 策略，以保持 STOP 优先级。
核心不创建隐藏或无限的接收队列。

## 响应交接

`hal_can_send() == true` 只表示完整的编码帧已交给目标传输。它不证明仲裁、线上送达、远端
接收、电机状态或物理停止。

对于被接受的 STOP，成功的 HAL 交接通过 `mcu_watchdog_confirm_stop_ack()` 确认现有的有界
STOP_ACK 槽位。若 HAL 拒绝发送，ACK 保持挂起，现有的 STOP 超时/重试策略保持活动。普通 ACK
结果保留在去重缓存中，因此主机重试可以请求相同的语义响应而不再次执行命令。

## 优先级证据

Wire V1 保留严格的数字顺序：

```text
STOP 0x080 < STOP_ACK 0x081 < COMMAND 0x100 < ACK 0x101 < TELEMETRY 0x180
```

有界 Host 模拟器接受一组挂起帧，并先暴露最低仲裁 ID，相同 ID 保持插入顺序。一个回归测试在
普通命令之前排队 STOP，证明在普通会话关闭的情况下 STOP 仍先被分发和交接。这只是确定性逻辑
证据；不是总线时序、控制器 FIFO、ISR 延迟或电气仲裁证据。

## 六域 BSP 兼容性闸门

BSP V0.1 为 `MCU-BASE`、两个机械臂控制器、两个工具控制器和 `MCU-SAFETY` 分配逻辑节点 ID，
但其具体的多节点仲裁位分配仍是实现闸门。Wire V1 目前把全部 11 个仲裁位分配给五个精确帧类型
ID，且在八字节载荷中不携带节点字段。因此不能通过静默地把节点 ID OR 进这些值来把多个独立
响应域放到一条共享总线上：那会改变冻结契约并可能产生冲突 ACK。

本桥接器在 Protocol、Firmware、Linux 和 Electrical Owner 于独立的版本化决策中批准以下选项
之一之前，仍是单一逻辑 Wire V1 端点：

- 带显式来源/目标所有权的新仲裁布局；
- 保留现有 ID 的物理分离总线；或
- 带有界节点寻址的新载荷/传输版本。

Issue #180 的平台无关部分不选择或推断任何选项。

## 目标证据矩阵

| 目标 | 桥接/编解码逻辑 | CAN 传输 | 证据边界 |
| --- | --- | --- | --- |
| Host fake | `PASS` | 有界模拟队列 `PASS` | 无控制器、线上或物理时序。 |
| RISC-V QEMU | `PASS` | `NOT_EXECUTED` | `hal_can_init/send/recv` 仍是返回 false 的桩。 |
| 遗留 CH32V307 目标 | 构建源码缺失 | `NOT_EXECUTED` | 无板卡 HAL、链接/启动包或板卡运行。 |
| BSP STM32H563 / STM32G0B1 | 目标缺失 | `NOT_EXECUTED` | 无获批的时钟、引脚、收发器、过滤器、IRQ、链接器或厂商 HAL 输入。 |

Host/QEMU 测试证明信封验证、编解码复用、方向闸门、STOP 优先分发和传输交接状态语义。它们不
证明真实 STM32 外设、六域仲裁、位速率、bus-off 恢复、物理 CAN、执行器、急停行为或硬实时
期限。
