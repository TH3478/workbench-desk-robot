/* frame_codec.c —— 固件自有的 Classic CAN Wire V1 无分配编解码器实现。
 *
 * 职责：在 mcu_wire_frame_t 逻辑帧与八字节 CAN 载荷之间做唯一双向映射，
 * 并在编码/解码前强制执行 Wire V1 的全部不变量（仲裁标识符、版本、
 * 保留字节、枚举值、ID 分区与跨字段结果语义）；任一不变量失败即拒绝，
 * 输出保持不变。
 *
 * 契约文档：docs/architecture/mcu-wire-v1.md（「仲裁标识符」「载荷布局」
 *   「数字注册表」「失败即拒绝行为与限制」）；跨字段语义以
 *   docs/architecture/mcu-protocol-v1.md（「结果语义」「故障码注册表」）为准。
 *
 * 编译目标：host、qemu、ch32v307 三目标共源；core/ 不含厂商或平台头文件
 *   （firmware/mcu/README.md「唯一规则」）。
 */

#include "frame_codec.h"

#include <stdbool.h>

/* ---- 逻辑帧跨字段不变量 ---- */

/* 普通 opcode 恰为 move、grip_open、grip_close、hold、heartbeat 五种
 * （mcu-wire-v1.md「数字注册表」）；reserved 与 stop 不在此列。 */
static bool is_ordinary_opcode(mcu_wire_opcode_t opcode)
{
    return opcode == MCU_WIRE_OPCODE_MOVE || opcode == MCU_WIRE_OPCODE_GRIP_OPEN ||
           opcode == MCU_WIRE_OPCODE_GRIP_CLOSE || opcode == MCU_WIRE_OPCODE_HOLD ||
           opcode == MCU_WIRE_OPCODE_HEARTBEAT;
}

/* 无故障模式是 idle（0）.. stopped（3）；faulted（4）除外。 */
static bool is_non_faulted_mode(mcu_wire_device_mode_t mode)
{
    return mode >= MCU_WIRE_MODE_IDLE && mode <= MCU_WIRE_MODE_STOPPED;
}

/* 遥测与响应类帧中不存在的命令字段必须保持安全中性零值，
 * 防止结构体被意外复用后静默携带过期字段（失败即拒绝）。 */
static bool absent_command_fields_are_zero(const mcu_wire_frame_t *frame)
{
    return frame->command_id == 0u && frame->opcode == MCU_WIRE_OPCODE_RESERVED &&
           frame->retry_count == 0u && frame->result_code == MCU_WIRE_RESULT_ACCEPTED;
}

/* 命令与 STOP 帧中不存在的响应字段必须保持零值。 */
static bool absent_response_fields_are_zero(const mcu_wire_frame_t *frame)
{
    return frame->sequence_no == 0u && frame->result_code == MCU_WIRE_RESULT_ACCEPTED &&
           frame->fault_code == MCU_WIRE_FAULT_NONE && frame->device_mode == MCU_WIRE_MODE_IDLE;
}

/* 普通 ACK 的结果语义（mcu-protocol-v1.md「结果语义」）：
 * 成功要求 fault_code == none 且模式无故障；失败要求
 * device_mode == faulted 且 fault_code 恰为 duplicate_frame
 * 或 malformed_frame 之一。 */
static bool ordinary_ack_is_valid(const mcu_wire_frame_t *frame)
{
    if (frame->result_code == MCU_WIRE_RESULT_ACCEPTED) {
        return frame->fault_code == MCU_WIRE_FAULT_NONE && is_non_faulted_mode(frame->device_mode);
    }
    if (frame->result_code != MCU_WIRE_RESULT_REJECTED || frame->device_mode != MCU_WIRE_MODE_FAULTED) {
        return false;
    }
    return frame->fault_code == MCU_WIRE_FAULT_DUPLICATE_FRAME ||
           frame->fault_code == MCU_WIRE_FAULT_MALFORMED_FRAME;
}

/* STOP_ACK 的结果语义：成功要求 fault_code == none 且
 * device_mode == stopped；失败要求 fault_code == stop_rejected 且
 * device_mode == faulted；其余组合均无效。 */
static bool stop_ack_is_valid(const mcu_wire_frame_t *frame)
{
    if (frame->result_code == MCU_WIRE_RESULT_ACCEPTED) {
        return frame->fault_code == MCU_WIRE_FAULT_NONE && frame->device_mode == MCU_WIRE_MODE_STOPPED;
    }
    return frame->result_code == MCU_WIRE_RESULT_REJECTED &&
           frame->fault_code == MCU_WIRE_FAULT_STOP_REJECTED &&
           frame->device_mode == MCU_WIRE_MODE_FAULTED;
}

/* 遥测语义（mcu-protocol-v1.md「遥测语义」）：不携带命令字段；
 * 健康遥测 fault_code == none 且模式无故障；故障遥测
 * device_mode == faulted 且 fault_code 恰为 link_lost 或
 * watchdog_expired 之一。 */
static bool telemetry_is_valid(const mcu_wire_frame_t *frame)
{
    if (!absent_command_fields_are_zero(frame)) {
        return false;
    }
    if (frame->fault_code == MCU_WIRE_FAULT_NONE) {
        return is_non_faulted_mode(frame->device_mode);
    }
    return (frame->fault_code == MCU_WIRE_FAULT_LINK_LOST ||
            frame->fault_code == MCU_WIRE_FAULT_WATCHDOG_EXPIRED) &&
           frame->device_mode == MCU_WIRE_MODE_FAULTED;
}

/* 整帧校验：按帧种类组合上述谓词。普通 command / ack 只接受
 * 0x0000..0x7fff 分区，stop / stop_ack 只接受 0x8000..0xffff 分区
 * （mcu-protocol-v1.md「帧形态」）；未知种类一律无效。 */
static bool frame_is_valid(const mcu_wire_frame_t *frame)
{
    switch (frame->kind) {
    case MCU_WIRE_FRAME_COMMAND:
        return frame->command_id <= MCU_COMMAND_ID_MAX && is_ordinary_opcode(frame->opcode) &&
               absent_response_fields_are_zero(frame);
    case MCU_WIRE_FRAME_ACK:
        return frame->command_id <= MCU_COMMAND_ID_MAX && is_ordinary_opcode(frame->opcode) &&
               frame->sequence_no == 0u && ordinary_ack_is_valid(frame);
    case MCU_WIRE_FRAME_TELEMETRY:
        return telemetry_is_valid(frame);
    case MCU_WIRE_FRAME_STOP:
        return frame->command_id >= MCU_STOP_ID_MIN && frame->opcode == MCU_WIRE_OPCODE_STOP &&
               absent_response_fields_are_zero(frame);
    case MCU_WIRE_FRAME_STOP_ACK:
        return frame->command_id >= MCU_STOP_ID_MIN && frame->opcode == MCU_WIRE_OPCODE_STOP &&
               frame->sequence_no == 0u && stop_ack_is_valid(frame);
    case MCU_WIRE_FRAME_KIND_COUNT:
    default:
        return false;
    }
}

/* ---- 仲裁标识符与帧种类的一一映射 ---- */

/* 帧种类到冻结仲裁 ID（mcu-wire-v1.md「仲裁标识符」）。
 * 未知种类返回 0xffff，必然无法通过 11 位标准 ID 上限校验。 */
static uint16_t frame_kind_to_id(mcu_wire_frame_kind_t kind)
{
    switch (kind) {
    case MCU_WIRE_FRAME_COMMAND:
        return MCU_CAN_ID_COMMAND;
    case MCU_WIRE_FRAME_ACK:
        return MCU_CAN_ID_ACK;
    case MCU_WIRE_FRAME_TELEMETRY:
        return MCU_CAN_ID_TELEMETRY;
    case MCU_WIRE_FRAME_STOP:
        return MCU_CAN_ID_STOP;
    case MCU_WIRE_FRAME_STOP_ACK:
        return MCU_CAN_ID_STOP_ACK;
    case MCU_WIRE_FRAME_KIND_COUNT:
    default:
        return 0xffffu;
    }
}

/* 五个冻结仲裁 ID 到帧种类的反向一一映射；
 * 所有其他标准标识符都被编解码器拒绝。 */
static bool id_to_frame_kind(uint16_t arbitration_id, mcu_wire_frame_kind_t *kind)
{
    switch (arbitration_id) {
    case MCU_CAN_ID_COMMAND:
        *kind = MCU_WIRE_FRAME_COMMAND;
        return true;
    case MCU_CAN_ID_ACK:
        *kind = MCU_WIRE_FRAME_ACK;
        return true;
    case MCU_CAN_ID_TELEMETRY:
        *kind = MCU_WIRE_FRAME_TELEMETRY;
        return true;
    case MCU_CAN_ID_STOP:
        *kind = MCU_WIRE_FRAME_STOP;
        return true;
    case MCU_CAN_ID_STOP_ACK:
        *kind = MCU_WIRE_FRAME_STOP_ACK;
        return true;
    default:
        return false;
    }
}

/* ---- 网络字节序（大端）读写与帧维护 ---- */

/* 16 位大端写出：多字节整数在载荷中高位在前
 * （mcu-wire-v1.md「传输边界」）。 */
static void write_u16_be(uint8_t *destination, uint16_t value)
{
    destination[0] = (uint8_t)(value >> 8);
    destination[1] = (uint8_t)value;
}

/* 32 位大端写出，用于遥测 sequence_no（载荷字节 1..4）。 */
static void write_u32_be(uint8_t *destination, uint32_t value)
{
    destination[0] = (uint8_t)(value >> 24);
    destination[1] = (uint8_t)(value >> 16);
    destination[2] = (uint8_t)(value >> 8);
    destination[3] = (uint8_t)value;
}

/* 16 位大端读出，用于载荷字节 1..2 的 command_id。 */
static uint16_t read_u16_be(const uint8_t *source)
{
    return (uint16_t)(((uint16_t)source[0] << 8) | source[1]);
}

/* 32 位大端读出，用于遥测 sequence_no。 */
static uint32_t read_u32_be(const uint8_t *source)
{
    return ((uint32_t)source[0] << 24) | ((uint32_t)source[1] << 16) |
           ((uint32_t)source[2] << 8) | source[3];
}

/* 把帧置为安全中性初值：kind == COMMAND、opcode == reserved（0）、
 * 其余字段为零/无故障/idle，与全零内存语义一致。 */
static void clear_frame(mcu_wire_frame_t *frame)
{
    frame->kind = MCU_WIRE_FRAME_COMMAND;
    frame->command_id = 0u;
    frame->sequence_no = 0u;
    frame->opcode = MCU_WIRE_OPCODE_RESERVED;
    frame->retry_count = 0u;
    frame->result_code = MCU_WIRE_RESULT_ACCEPTED;
    frame->fault_code = MCU_WIRE_FAULT_NONE;
    frame->device_mode = MCU_WIRE_MODE_IDLE;
}

/* 逐字段复制；解码器只在全部校验通过后才用它发布临时结果。 */
static void copy_frame(mcu_wire_frame_t *destination, const mcu_wire_frame_t *source)
{
    destination->kind = source->kind;
    destination->command_id = source->command_id;
    destination->sequence_no = source->sequence_no;
    destination->opcode = source->opcode;
    destination->retry_count = source->retry_count;
    destination->result_code = source->result_code;
    destination->fault_code = source->fault_code;
    destination->device_mode = source->device_mode;
}

/* ---- 编解码入口 ---- */

/* 编码器在写出任何输出字节之前先校验指针、目标容量与整帧不变量；
 * 失败即拒绝且输出保持不变。成功时恰写出 MCU_WIRE_DLC（8）字节。
 * 载荷布局（mcu-wire-v1.md「载荷布局」）：命令/STOP 为 [0]=version、
 * [1..2]=command_id、[3]=opcode、[4]=retry_count、[5..7]=保留零；
 * ACK/STOP_ACK 的 [5..7] 依次为 result_code、fault_code、device_mode；
 * 遥测的 [1..4]=sequence_no、[5]=fault_code、[6]=device_mode、
 * [7]=保留零。 */
mcu_codec_status_t mcu_frame_encode(const mcu_wire_frame_t *frame,
                                    uint16_t *arbitration_id,
                                    uint8_t *destination,
                                    uint8_t destination_capacity,
                                    uint8_t *encoded_length)
{
    uint8_t encoded[MCU_WIRE_DLC] = {0u};
    uint16_t encoded_id;
    unsigned i;

    if (frame == 0 || arbitration_id == 0 || destination == 0 || encoded_length == 0) {
        return MCU_CODEC_INVALID_ARGUMENT;
    }
    if (destination_capacity < MCU_WIRE_DLC) {
        return MCU_CODEC_BUFFER_TOO_SMALL;
    }
    if (!frame_is_valid(frame)) {
        return MCU_CODEC_INVALID_FIELD;
    }

    encoded_id = frame_kind_to_id(frame->kind);
    encoded[0] = MCU_WIRE_VERSION_V1;
    switch (frame->kind) {
    case MCU_WIRE_FRAME_COMMAND:
    case MCU_WIRE_FRAME_STOP:
        write_u16_be(&encoded[1], frame->command_id);
        encoded[3] = (uint8_t)frame->opcode;
        encoded[4] = frame->retry_count;
        break;
    case MCU_WIRE_FRAME_ACK:
    case MCU_WIRE_FRAME_STOP_ACK:
        write_u16_be(&encoded[1], frame->command_id);
        encoded[3] = (uint8_t)frame->opcode;
        encoded[4] = frame->retry_count;
        encoded[5] = (uint8_t)frame->result_code;
        encoded[6] = (uint8_t)frame->fault_code;
        encoded[7] = (uint8_t)frame->device_mode;
        break;
    case MCU_WIRE_FRAME_TELEMETRY:
        write_u32_be(&encoded[1], frame->sequence_no);
        encoded[5] = (uint8_t)frame->fault_code;
        encoded[6] = (uint8_t)frame->device_mode;
        break;
    case MCU_WIRE_FRAME_KIND_COUNT:
    default:
        return MCU_CODEC_INVALID_FIELD;
    }

    for (i = 0u; i < MCU_WIRE_DLC; i++) {
        destination[i] = encoded[i];
    }
    *arbitration_id = encoded_id;
    *encoded_length = MCU_WIRE_DLC;
    return MCU_CODEC_OK;
}

/* 解码器在读取 source 之前校验 encoded_length 恰为 MCU_WIRE_DLC（8），
 * 依次校验仲裁 ID、版本字节、保留字节与各字段枚举，最后做整帧
 * 跨字段校验；仅当每个字节与不变量全部通过后才发布帧（copy_frame），
 * 否则输出保持不变——失败即拒绝。 */
mcu_codec_status_t mcu_frame_decode(uint16_t arbitration_id,
                                    const uint8_t *source,
                                    uint8_t encoded_length,
                                    mcu_wire_frame_t *frame)
{
    mcu_wire_frame_t decoded;

    if (frame == 0) {
        return MCU_CODEC_INVALID_ARGUMENT;
    }
    if (encoded_length != MCU_WIRE_DLC) {
        return MCU_CODEC_INVALID_LENGTH;
    }
    if (source == 0) {
        return MCU_CODEC_INVALID_ARGUMENT;
    }
    clear_frame(&decoded);
    if (!id_to_frame_kind(arbitration_id, &decoded.kind)) {
        return MCU_CODEC_UNSUPPORTED_ID;
    }
    if (source[0] != MCU_WIRE_VERSION_V1) {
        return MCU_CODEC_INVALID_VERSION;
    }

    switch (decoded.kind) {
    case MCU_WIRE_FRAME_COMMAND:
    case MCU_WIRE_FRAME_STOP:
        if (source[5] != 0u || source[6] != 0u || source[7] != 0u) {
            return MCU_CODEC_NONZERO_RESERVED;
        }
        decoded.command_id = read_u16_be(&source[1]);
        decoded.opcode = (mcu_wire_opcode_t)source[3];
        decoded.retry_count = source[4];
        break;
    case MCU_WIRE_FRAME_ACK:
    case MCU_WIRE_FRAME_STOP_ACK:
        decoded.command_id = read_u16_be(&source[1]);
        decoded.opcode = (mcu_wire_opcode_t)source[3];
        decoded.retry_count = source[4];
        decoded.result_code = (mcu_wire_result_t)source[5];
        decoded.fault_code = (mcu_wire_fault_t)source[6];
        decoded.device_mode = (mcu_wire_device_mode_t)source[7];
        break;
    case MCU_WIRE_FRAME_TELEMETRY:
        if (source[7] != 0u) {
            return MCU_CODEC_NONZERO_RESERVED;
        }
        decoded.sequence_no = read_u32_be(&source[1]);
        decoded.fault_code = (mcu_wire_fault_t)source[5];
        decoded.device_mode = (mcu_wire_device_mode_t)source[6];
        break;
    case MCU_WIRE_FRAME_KIND_COUNT:
    default:
        return MCU_CODEC_UNSUPPORTED_ID;
    }

    if (!frame_is_valid(&decoded)) {
        return MCU_CODEC_INVALID_FIELD;
    }
    copy_frame(frame, &decoded);
    return MCU_CODEC_OK;
}
