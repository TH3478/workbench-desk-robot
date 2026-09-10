# MCU CAN Wire V1

状态：Issue #54 的**固件拥有的二进制契约**，由 Issue #180 连接到原始 HAL 信封。

本文档把冻结的逻辑 MCU 协议 v1.0 映射进一个 Classic CAN 数据帧。逻辑契约对帧含义仍是规范性
的。Wire V1 只定义传输编码，不改变 `interfaces/json_schema/mcu_protocol.schema.json` 或导出
的 Pydantic 模型。

## 传输边界

- 带标准 11 位标识符的 Classic CAN 2.0 数据帧。
- 每种帧类型 DLC 都恰为 8。远程、extended-ID 和 CAN FD 帧在本编解码器之外。
- 多字节整数使用网络字节序（大端）。
- 字节 0 是紧凑协议版本。逻辑版本 `"1.0"` 是 `0x10`。
- CAN 的帧 CRC 是传输完整性检查；Wire V1 不增加载荷校验和。

原始控制器边界与拒绝顺序定义在 `docs/architecture/mcu-can-hal-boundary-v1.md`。特别是，
`hal_can_frame.arbitration_id` 是下表所示 11 位值，而逻辑 16 位 `command_id` 保留在载荷字节
1..2 内。

逻辑 `frame_id`、`sent_at_us` 和 `clock_id` 字段是适配器/证据元数据，不在八字节 CAN 载荷中
传输。桥接器拥有它们的本地生成与保留。它们绝不会被重构为远端时间戳，或用作跨设备新鲜度
证据。

## 仲裁标识符

| CAN ID | 帧类型 | 方向 | 优先级理由 |
| --- | --- | --- | --- |
| `0x080` | `stop` | 主机到 MCU | 最高协议优先级。 |
| `0x081` | `stop_ack` | MCU 到主机 | 关联的安全响应。 |
| `0x100` | `command` | 主机到 MCU | 普通命令流量。 |
| `0x101` | `ack` | MCU 到主机 | 普通关联响应。 |
| `0x180` | `telemetry` | MCU 到主机 | 最低协议优先级。 |

较低标识符赢得 CAN 仲裁。ID 恰好选择一种帧类型；所有其他标准标识符被编解码器拒绝。

## 载荷布局

所有偏移从零开始，每个保留字节必须是 `0x00`。

### Command 与 STOP

| 字节 | 字段 |
| --- | --- |
| 0 | version (`0x10`) |
| 1..2 | `command_id`，无符号 16 位大端 |
| 3 | opcode |
| 4 | `retry_count` |
| 5..7 | 保留零 |

`command` 只接受 ID `0x0000..0x7fff` 和普通 opcode。`stop` 只接受 ID `0x8000..0xffff` 和
opcode `stop`。

### ACK 与 STOP_ACK

| 字节 | 字段 |
| --- | --- |
| 0 | version (`0x10`) |
| 1..2 | `command_id`，无符号 16 位大端 |
| 3 | opcode |
| 4 | 回显的 `retry_count` |
| 5 | `result_code` |
| 6 | `fault_code` |
| 7 | `device_mode` |

`ack` 接受普通 ID/opcode 分区。`stop_ack` 接受 STOP 分区与 opcode。结果、故障与模式组合必须
满足冻结逻辑协议；仅编码一个数字枚举值是不够的。

### 遥测

| 字节 | 字段 |
| --- | --- |
| 0 | version (`0x10`) |
| 1..4 | `sequence_no`，无符号 32 位大端 |
| 5 | `fault_code` |
| 6 | `device_mode` |
| 7 | 保留零 |

遥测没有命令 ID、opcode、重试计数或结果码，从不确认命令。

## 数字注册表

| Opcode | 值 |
| --- | --- |
| reserved | `0x00` |
| `move` | `0x01` |
| `grip_open` | `0x02` |
| `grip_close` | `0x03` |
| `hold` | `0x04` |
| `stop` | `0x05` |
| `heartbeat` | `0x06` |

| Result | 值 |
| --- | --- |
| accepted | `0x00` |
| rejected | `0x01` |

| Fault | 值 | 有效 MCU 帧 |
| --- | --- | --- |
| `none` | `0x00` | 按 result/mode 约束的 ACK、STOP_ACK、遥测 |
| `ack_timeout` | `0x01` | 绝不；仅主机诊断 |
| `stop_timeout` | `0x02` | 绝不；仅主机诊断 |
| `stop_rejected` | `0x03` | 仅失败 STOP_ACK |
| `link_lost` | `0x04` | 仅故障遥测 |
| `duplicate_frame` | `0x05` | 仅失败普通 ACK |
| `watchdog_expired` | `0x06` | 仅故障遥测 |
| `malformed_frame` | `0x07` | 仅失败普通 ACK |

| Device mode | 值 |
| --- | --- |
| `idle` | `0x00` |
| `moving` | `0x01` |
| `holding` | `0x02` |
| `stopped` | `0x03` |
| `faulted` | `0x04` |

所有未列出的枚举值都是保留且无效的。

## 规范黄金向量

已提交的 `interfaces/examples/mcu-frame-stop-ack.json` 描述命令 ID 32769（`0x8001`）、零
重试、无故障、已停止模式的成功 STOP 确认。其 Wire V1 表示是：

```text
CAN ID: 0x081
DLC:    8
DATA:   10 80 01 05 00 00 00 03
```

JSON 的 `frame_id`、`sent_at_us` 和 `clock_id` 如上所述仍是适配器元数据。共享的 Host/QEMU C
测试语料固定该字节向量。

## 失败即拒绝行为与限制

当 ID、DLC、版本、保留字节、枚举值、ID 分区或跨字段结果语义无效时，解码器在发布输出前拒绝
整个帧。编码器在写出任何输出字节前验证完整的逻辑线上对象和目标容量。

Wire V1 不实现命令去重、看门狗调度、主机传输分发、CAN 控制器寄存器、bus-off 恢复、多节点
寻址或电气验证。Issue #180 桥接器验证原始信封并路由解码帧，但真实目标驱动与物理证据仍是
独立的 Owner 把关工作。
