#ifndef MCU_FRAME_CODEC_H
#define MCU_FRAME_CODEC_H

#include <stdint.h>

/* 固件自有的 CAN Wire V1。逻辑协议仍为版本 1.0；
 * 0x10 是其在线上传输的紧凑表示。 */
#define MCU_WIRE_VERSION_V1 0x10u
#define MCU_WIRE_DLC 8u

/* CAN 标识符数值较小者在仲裁中胜出，因此 STOP 流量优先于
 * 普通命令、确认与遥测流量。 */
#define MCU_CAN_ID_STOP 0x080u
#define MCU_CAN_ID_STOP_ACK 0x081u
#define MCU_CAN_ID_COMMAND 0x100u
#define MCU_CAN_ID_ACK 0x101u
#define MCU_CAN_ID_TELEMETRY 0x180u

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
 * 也会失败即拒绝，而不是静默丢弃过期字段。 */
typedef struct {
    mcu_wire_frame_kind_t kind;
    uint16_t command_id;
    uint32_t sequence_no;
    mcu_wire_opcode_t opcode;
    uint8_t retry_count;
    mcu_wire_result_t result_code;
    mcu_wire_fault_t fault_code;
    mcu_wire_device_mode_t device_mode;
} mcu_wire_frame_t;

/* 失败时输出保持不变。编码器在写入前校验容量，
 * 成功时始终恰好发出 MCU_WIRE_DLC 字节。 */
mcu_codec_status_t mcu_frame_encode(const mcu_wire_frame_t *frame,
                                    uint16_t *arbitration_id,
                                    uint8_t *destination,
                                    uint8_t destination_capacity,
                                    uint8_t *encoded_length);

/* 解码器在读取 source 之前校验 encoded_length，且仅当每个字节与
 * 跨字段不变量全部通过后才发布帧。 */
mcu_codec_status_t mcu_frame_decode(uint16_t arbitration_id,
                                    const uint8_t *source,
                                    uint8_t encoded_length,
                                    mcu_wire_frame_t *frame);

#endif /* MCU_FRAME_CODEC_H */
