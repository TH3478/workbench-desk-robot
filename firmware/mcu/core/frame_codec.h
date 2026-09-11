/* frame_codec.h —— 固件自有的 Classic CAN Wire V1 编解码器接口。
 *
 * 职责：冻结的仲裁标识符、载荷布局、数字注册表、逻辑帧结构与
 * 编解码状态码的公共类型。逻辑协议语义仍以 mcu-protocol-v1.md 为准。
 *
 * 契约文档：docs/architecture/mcu-wire-v1.md（「仲裁标识符」「载荷布局」
 *   「数字注册表」「规范黄金向量」）。
 *
 * 编译目标：host、qemu、ch32v307 三目标共源；core/ 不含厂商或平台头文件
 *   （firmware/mcu/README.md「唯一规则」）。
 */
#ifndef MCU_FRAME_CODEC_H
#define MCU_FRAME_CODEC_H

#include <stdint.h>

/* 固件自有的 CAN Wire V1。逻辑协议仍为版本 1.0；
 * 0x10 是其在线上传输的紧凑表示。 */
#define MCU_WIRE_VERSION_V1 0x10u
/* 每种帧类型的 DLC 都恰为 8：Wire V1 载荷布局固定
 * （mcu-wire-v1.md「传输边界」「载荷布局」），帧长从不随字段变化，
 * 接收侧无需长度协商，只需比较长度即可拒绝错误 DLC。 */
#define MCU_WIRE_DLC 8u

/* CAN 标识符数值较小者在仲裁中胜出，因此 STOP 流量优先于
 * 普通命令、确认与遥测流量。五个冻结 ID 的语义与方向
 * （mcu-wire-v1.md「仲裁标识符」）：
 *   0x080 STOP      主机 → MCU，最高协议优先级；
 *   0x081 STOP_ACK  MCU → 主机，关联的安全响应；
 *   0x100 COMMAND   主机 → MCU，普通命令流量；
 *   0x101 ACK       MCU → 主机，普通关联响应；
 *   0x180 TELEMETRY MCU → 主机，最低协议优先级。
 * 仲裁 ID 恰好选择一种帧类型；其余标准标识符一律被编解码器拒绝。 */
#define MCU_CAN_ID_STOP 0x080u
#define MCU_CAN_ID_STOP_ACK 0x081u
#define MCU_CAN_ID_COMMAND 0x100u
#define MCU_CAN_ID_ACK 0x101u
#define MCU_CAN_ID_TELEMETRY 0x180u

/* 逻辑 16 位 command_id 分区：最高位是命令类别——普通命令
 * 0x0000..0x7fff，STOP 0x8000..0xffff；两类序号历史相互独立
 * （mcu-protocol-v1.md「关联、重试与回绕语义」）。该分区防止
 * 普通 ACK 被误认为停止确认。 */
#define MCU_COMMAND_ID_MAX 0x7fffu
#define MCU_STOP_ID_MIN 0x8000u

typedef enum {
    MCU_WIRE_FRAME_COMMAND = 0,
    MCU_WIRE_FRAME_ACK,
    MCU_WIRE_FRAME_TELEMETRY,
    MCU_WIRE_FRAME_STOP,
    MCU_WIRE_FRAME_STOP_ACK,
    MCU_WIRE_FRAME_KIND_COUNT
} mcu_wire_frame_kind_t;

/* 0 被保留，使未初始化/全零填充的操作码无法执行。 */
typedef enum {
    MCU_WIRE_OPCODE_RESERVED = 0,
    MCU_WIRE_OPCODE_MOVE = 1,
    MCU_WIRE_OPCODE_GRIP_OPEN = 2,
    MCU_WIRE_OPCODE_GRIP_CLOSE = 3,
    MCU_WIRE_OPCODE_HOLD = 4,
    MCU_WIRE_OPCODE_STOP = 5,
    MCU_WIRE_OPCODE_HEARTBEAT = 6,
    MCU_WIRE_OPCODE_COUNT
} mcu_wire_opcode_t;

typedef enum {
    MCU_WIRE_RESULT_ACCEPTED = 0,
    MCU_WIRE_RESULT_REJECTED = 1,
    MCU_WIRE_RESULT_COUNT
} mcu_wire_result_t;

/* 取值遵循冻结的逻辑故障注册表。ACK_TIMEOUT 与 STOP_TIMEOUT 是仅主机侧
 * 诊断，虽然其数值在此保留，但在解码后的 MCU 帧中会被拒绝。 */
typedef enum {
    MCU_WIRE_FAULT_NONE = 0,
    MCU_WIRE_FAULT_ACK_TIMEOUT = 1,
    MCU_WIRE_FAULT_STOP_TIMEOUT = 2,
    MCU_WIRE_FAULT_STOP_REJECTED = 3,
    MCU_WIRE_FAULT_LINK_LOST = 4,
    MCU_WIRE_FAULT_DUPLICATE_FRAME = 5,
    MCU_WIRE_FAULT_WATCHDOG_EXPIRED = 6,
    MCU_WIRE_FAULT_MALFORMED_FRAME = 7,
    MCU_WIRE_FAULT_COUNT
} mcu_wire_fault_t;

typedef enum {
    MCU_WIRE_MODE_IDLE = 0,
    MCU_WIRE_MODE_MOVING = 1,
    MCU_WIRE_MODE_HOLDING = 2,
    MCU_WIRE_MODE_STOPPED = 3,
    MCU_WIRE_MODE_FAULTED = 4,
    MCU_WIRE_MODE_COUNT
} mcu_wire_device_mode_t;

typedef enum {
    MCU_CODEC_OK = 0,
    MCU_CODEC_INVALID_ARGUMENT,
    MCU_CODEC_BUFFER_TOO_SMALL,
    MCU_CODEC_INVALID_LENGTH,
    MCU_CODEC_UNSUPPORTED_ID,
    MCU_CODEC_INVALID_VERSION,
    MCU_CODEC_NONZERO_RESERVED,
    MCU_CODEC_INVALID_FIELD
} mcu_codec_status_t;

/* 帧种类中不存在的字段必须保持为 0。这样即使结构体被意外复用，
 * 也会失败即拒绝，而不是静默丢弃过期字段。
 * 线上字段宽度与字节位置（mcu-wire-v1.md「载荷布局」）：
 *   command_id  16 位，命令/STOP/ACK/STOP_ACK 载荷字节 1..2（大端）；
 *   sequence_no 32 位，遥测载荷字节 1..4（大端，可回绕）；
 *   opcode      8 位，载荷字节 3；
 *   retry_count 8 位，载荷字节 4（0..255）；
 *   result_code 8 位，ACK/STOP_ACK 载荷字节 5（0=accepted，1=rejected）；
 *   fault_code  8 位，ACK/STOP_ACK 载荷字节 6、遥测载荷字节 5；
 *   device_mode 8 位，ACK/STOP_ACK 载荷字节 7、遥测载荷字节 6。 */
typedef struct {
    /* 帧类型；由仲裁 ID 一一映射（0x080/0x081/0x100/0x101/0x180）。 */
    mcu_wire_frame_kind_t kind;
    /* 16 位关联标识符；线上位于载荷字节 1..2（大端），
     * 不进入 11 位仲裁寄存器。 */
    uint16_t command_id;
    /* 32 位遥测序号；线上位于遥测载荷字节 1..4（大端），
     * 可能回绕，消费者不得将其用作命令 ID。 */
    uint32_t sequence_no;
    /* opcode；线上位于载荷字节 3，宽度 8 位。 */
    mcu_wire_opcode_t opcode;
    /* 重试计数；线上位于载荷字节 4，宽度 8 位（0..255），
     * ACK 回显请求的计数。 */
    uint8_t retry_count;
    /* 结果码；线上位于 ACK/STOP_ACK 载荷字节 5（0=accepted，1=rejected）。 */
    mcu_wire_result_t result_code;
    /* 故障码；线上位于 ACK/STOP_ACK 载荷字节 6、遥测载荷字节 5。 */
    mcu_wire_fault_t fault_code;
    /* 设备模式；线上位于 ACK/STOP_ACK 载荷字节 7、遥测载荷字节 6。 */
    mcu_wire_device_mode_t device_mode;
} mcu_wire_frame_t;

/* 失败时输出保持不变。编码器在写入前校验容量，
 * 成功时始终恰好发出 MCU_WIRE_DLC 字节。
 * 字节级布局由 tests/frame_codec_tests.c 的黄金向量 ①–⑧ 固定，
 * 其中 ⑦ 是 interfaces/examples/mcu-frame-stop-ack.json 的线上表示。 */
mcu_codec_status_t mcu_frame_encode(const mcu_wire_frame_t *frame,
                                    uint16_t *arbitration_id,
                                    uint8_t *destination,
                                    uint8_t destination_capacity,
                                    uint8_t *encoded_length);

/* 解码器在读取 source 之前校验 encoded_length，且仅当每个字节与
 * 跨字段不变量全部通过后才发布帧。黄金向量 ①–⑧ 在共享 Host/QEMU
 * 语料中逐一往返校验本解码路径。 */
mcu_codec_status_t mcu_frame_decode(uint16_t arbitration_id,
                                    const uint8_t *source,
                                    uint8_t encoded_length,
                                    mcu_wire_frame_t *frame);

#endif /* MCU_FRAME_CODEC_H */
