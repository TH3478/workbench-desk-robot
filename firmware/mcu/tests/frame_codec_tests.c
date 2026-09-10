/* Wire V1 帧编解码器测试套件。对应 Issue #54 的严格编解码器。
 *
 * 被测契约：docs/architecture/mcu-wire-v1.md 冻结的二进制契约——
 * 五种 CAN ID、DLC 恒为 8、大端载荷布局、ID 分区强制、保留字节
 * 为零、跨字段结果语义，以及失败即拒绝（解码拒绝时不得写入输出）。
 *
 * 覆盖：规范黄金向量（含 interfaces/examples/mcu-frame-stop-ack.json
 * 对应的 10 80 01 05 00 00 00 03 字节向量）、坏信封/坏载荷拒绝、
 * 编码器边界与严格字段校验、以及 4096 + 8192 轮的确定性模糊测试。
 */
#include "frame_codec_tests.h"

#include <stdbool.h>

#include "frame_codec.h"

/* 黄金向量：逻辑帧 ↔ (仲裁 ID, 8 字节载荷) 的规范映射。 */
typedef struct {
    mcu_wire_frame_t frame;
    uint16_t arbitration_id;
    uint8_t data[MCU_WIRE_DLC];
} golden_vector_t;

static const golden_vector_t golden_vectors[] = {
    /* ① command：最大普通 ID 32767 + heartbeat + 最大重试计数 255。 */
    {
        .frame = {
            .kind = MCU_WIRE_FRAME_COMMAND,
            .command_id = 32767u,
            .opcode = MCU_WIRE_OPCODE_HEARTBEAT,
            .retry_count = 255u,
        },
        .arbitration_id = MCU_CAN_ID_COMMAND,
        .data = {0x10u, 0x7fu, 0xffu, 0x06u, 0xffu, 0x00u, 0x00u, 0x00u},
    },
    /* ② ack：hold 的接受确认（结果/故障/模式组合合法）。 */
    {
        .frame = {
            .kind = MCU_WIRE_FRAME_ACK,
            .command_id = 19u,
            .opcode = MCU_WIRE_OPCODE_HOLD,
            .retry_count = 1u,
            .result_code = MCU_WIRE_RESULT_ACCEPTED,
            .fault_code = MCU_WIRE_FAULT_NONE,
            .device_mode = MCU_WIRE_MODE_HOLDING,
        },
        .arbitration_id = MCU_CAN_ID_ACK,
        .data = {0x10u, 0x00u, 0x13u, 0x04u, 0x01u, 0x00u, 0x00u, 0x02u},
    },
    /* ③ ack：grip_close 的失败确认（malformed_frame + faulted）。 */
    {
        .frame = {
            .kind = MCU_WIRE_FRAME_ACK,
            .command_id = 20u,
            .opcode = MCU_WIRE_OPCODE_GRIP_CLOSE,
            .result_code = MCU_WIRE_RESULT_REJECTED,
            .fault_code = MCU_WIRE_FAULT_MALFORMED_FRAME,
            .device_mode = MCU_WIRE_MODE_FAULTED,
        },
        .arbitration_id = MCU_CAN_ID_ACK,
        .data = {0x10u, 0x00u, 0x14u, 0x03u, 0x00u, 0x01u, 0x07u, 0x04u},
    },
    /* ④ telemetry：序号 0xffffffff 的健康遥测（保留字节 7 为零）。 */
    {
        .frame = {
            .kind = MCU_WIRE_FRAME_TELEMETRY,
            .sequence_no = 0xffffffffu,
            .fault_code = MCU_WIRE_FAULT_NONE,
            .device_mode = MCU_WIRE_MODE_IDLE,
        },
        .arbitration_id = MCU_CAN_ID_TELEMETRY,
        .data = {0x10u, 0xffu, 0xffu, 0xffu, 0xffu, 0x00u, 0x00u, 0x00u},
    },
    /* ⑤ telemetry：序号 0 的看门狗到期故障遥测。 */
    {
        .frame = {
            .kind = MCU_WIRE_FRAME_TELEMETRY,
            .sequence_no = 0u,
            .fault_code = MCU_WIRE_FAULT_WATCHDOG_EXPIRED,
            .device_mode = MCU_WIRE_MODE_FAULTED,
        },
        .arbitration_id = MCU_CAN_ID_TELEMETRY,
        .data = {0x10u, 0x00u, 0x00u, 0x00u, 0x00u, 0x06u, 0x04u, 0x00u},
    },
    /* ⑥ stop：STOP 分区下界 32768。 */
    {
        .frame = {
            .kind = MCU_WIRE_FRAME_STOP,
            .command_id = 32768u,
            .opcode = MCU_WIRE_OPCODE_STOP,
        },
        .arbitration_id = MCU_CAN_ID_STOP,
        .data = {0x10u, 0x80u, 0x00u, 0x05u, 0x00u, 0x00u, 0x00u, 0x00u},
    },
    /* ⑦ 规范 STOP ACK：interfaces/examples/mcu-frame-stop-ack.json 的
     * 字节级表示 10 80 01 05 00 00 00 03。 */
    {
        /* interfaces/examples/mcu-frame-stop-ack.json */
        .frame = {
            .kind = MCU_WIRE_FRAME_STOP_ACK,
            .command_id = 32769u,
            .opcode = MCU_WIRE_OPCODE_STOP,
            .result_code = MCU_WIRE_RESULT_ACCEPTED,
            .fault_code = MCU_WIRE_FAULT_NONE,
            .device_mode = MCU_WIRE_MODE_STOPPED,
        },
        .arbitration_id = MCU_CAN_ID_STOP_ACK,
        .data = {0x10u, 0x80u, 0x01u, 0x05u, 0x00u, 0x00u, 0x00u, 0x03u},
    },
    /* ⑧ stop_ack：STOP 分区上界 65535 + 重试 2 的失败停止确认。 */
    {
        .frame = {
            .kind = MCU_WIRE_FRAME_STOP_ACK,
            .command_id = 65535u,
            .opcode = MCU_WIRE_OPCODE_STOP,
            .retry_count = 2u,
            .result_code = MCU_WIRE_RESULT_REJECTED,
            .fault_code = MCU_WIRE_FAULT_STOP_REJECTED,
            .device_mode = MCU_WIRE_MODE_FAULTED,
        },
        .arbitration_id = MCU_CAN_ID_STOP_ACK,
        .data = {0x10u, 0xffu, 0xffu, 0x05u, 0x02u, 0x01u, 0x03u, 0x04u},
    },
};

/* 单条断言：递增计数；失败时记录失败数与首个失败序号。 */
static void check(mcu_test_report_t *report, bool condition)
{
    report->assertions++;
    if (!condition) {
        report->failures++;
        if (report->first_failure == 0u) {
            report->first_failure = report->assertions;
        }
    }
}

/* 逐字节相等比较，避免 memcmp 依赖与长度歧义。 */
static bool bytes_equal(const uint8_t *left, const uint8_t *right, unsigned length)
{
    unsigned i;

    for (i = 0u; i < length; i++) {
        if (left[i] != right[i]) {
            return false;
        }
    }
    return true;
}

/* 逐字节拷贝，供构造畸形载荷时保持其余字节原样。 */
static void copy_bytes(uint8_t *destination, const uint8_t *source, unsigned length)
{
    unsigned i;

    for (i = 0u; i < length; i++) {
        destination[i] = source[i];
    }
}

/* 逻辑帧逐字段相等比较。 */
static bool frames_equal(const mcu_wire_frame_t *left, const mcu_wire_frame_t *right)
{
    return left->kind == right->kind && left->command_id == right->command_id &&
           left->sequence_no == right->sequence_no && left->opcode == right->opcode &&
           left->retry_count == right->retry_count && left->result_code == right->result_code &&
           left->fault_code == right->fault_code && left->device_mode == right->device_mode;
}

/* 逐字段拷贝逻辑帧。 */
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

/* 把逻辑帧重置为合法「空白」COMMAND 帧。 */
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

/* 填充哨兵值（0xa5/0x5a 模式）：解码/编码失败时输出必须
 * 保持哨兵不变，证明「拒绝时不写输出」的失败即拒绝契约。 */
static void set_sentinel_frame(mcu_wire_frame_t *frame)
{
    frame->kind = MCU_WIRE_FRAME_KIND_COUNT;
    frame->command_id = 0xa55au;
    frame->sequence_no = 0x5aa55aa5u;
    frame->opcode = MCU_WIRE_OPCODE_COUNT;
    frame->retry_count = 0xa5u;
    frame->result_code = MCU_WIRE_RESULT_COUNT;
    frame->fault_code = MCU_WIRE_FAULT_COUNT;
    frame->device_mode = MCU_WIRE_MODE_COUNT;
}

/* 解码断言辅助：以哨兵帧为输入，断言返回状态恰为 expected；
 * 若期望失败，再断言输出帧未被写入（仍等于解码前的哨兵）。 */
static void expect_decode_status(mcu_test_report_t *report,
                                 uint16_t arbitration_id,
                                 const uint8_t *data,
                                 uint8_t length,
                                 mcu_codec_status_t expected)
{
    mcu_wire_frame_t decoded;
    mcu_wire_frame_t before;
    mcu_codec_status_t status;

    set_sentinel_frame(&decoded);
    copy_frame(&before, &decoded);
    status = mcu_frame_decode(arbitration_id, data, length, &decoded);

    check(report, status == expected);
    if (expected != MCU_CODEC_OK) {
        check(report, frames_equal(&decoded, &before));
    }
}

/* 黄金向量往返：先断言 CAN ID 优先级与 DLC=8 的不变量，
 * 再对每条向量做 编码 → 逐字节比对 → 解码 → 逻辑帧比对 闭环。 */
static void test_golden_vectors(mcu_test_report_t *report)
{
    unsigned vector_index;

    check(report, MCU_CAN_ID_STOP < MCU_CAN_ID_COMMAND);
    check(report, MCU_CAN_ID_STOP_ACK < MCU_CAN_ID_ACK);
    check(report, MCU_WIRE_DLC == 8u);

    for (vector_index = 0u; vector_index < sizeof(golden_vectors) / sizeof(golden_vectors[0]);
         vector_index++) {
        const golden_vector_t *vector = &golden_vectors[vector_index];
        uint8_t encoded[MCU_WIRE_DLC] = {0u};
        uint8_t encoded_length = 0u;
        uint16_t arbitration_id = 0u;
        mcu_wire_frame_t decoded;

        set_sentinel_frame(&decoded);

        check(report,
              mcu_frame_encode(&vector->frame,
                               &arbitration_id,
                               encoded,
                               sizeof(encoded),
                               &encoded_length) == MCU_CODEC_OK);
        check(report, arbitration_id == vector->arbitration_id);
        check(report, encoded_length == MCU_WIRE_DLC);
        check(report, bytes_equal(encoded, vector->data, MCU_WIRE_DLC));
        check(report,
              mcu_frame_decode(arbitration_id, encoded, encoded_length, &decoded) == MCU_CODEC_OK);
        check(report, frames_equal(&decoded, &vector->frame));
    }
}

/* 坏信封拒绝且不读输出：长度 0..7 与 9 均为 INVALID_LENGTH；
 * 空数据指针/空输出指针为 INVALID_ARGUMENT；未知 CAN ID 为
 * UNSUPPORTED_ID；所有失败都不触碰输出帧。 */
static void test_decode_rejects_bad_envelope_without_reads(mcu_test_report_t *report)
{
    uint8_t one_byte = MCU_WIRE_VERSION_V1;
    mcu_wire_frame_t decoded;
    unsigned length;

    set_sentinel_frame(&decoded);

    for (length = 0u; length < MCU_WIRE_DLC; length++) {
        expect_decode_status(report,
                             MCU_CAN_ID_COMMAND,
                             &one_byte,
                             (uint8_t)length,
                             MCU_CODEC_INVALID_LENGTH);
    }
    expect_decode_status(report, MCU_CAN_ID_COMMAND, &one_byte, 9u, MCU_CODEC_INVALID_LENGTH);
    expect_decode_status(report, MCU_CAN_ID_COMMAND, 0, MCU_WIRE_DLC, MCU_CODEC_INVALID_ARGUMENT);
    expect_decode_status(report,
                         0x7ffu,
                         golden_vectors[0].data,
                         MCU_WIRE_DLC,
                         MCU_CODEC_UNSUPPORTED_ID);
    check(report,
          mcu_frame_decode(MCU_CAN_ID_COMMAND,
                           golden_vectors[0].data,
                           MCU_WIRE_DLC,
                           0) == MCU_CODEC_INVALID_ARGUMENT);
    check(report, decoded.kind == MCU_WIRE_FRAME_KIND_COUNT);
}

/* 畸形载荷拒绝：逐项破坏版本字节、保留字节、ID 分区、
 * opcode 分区与跨字段结果/故障/模式组合，断言精确的状态码
 * （mcu-wire-v1.md 的失败即拒绝与数字注册表）。 */
static void test_decode_rejects_malformed_payloads(mcu_test_report_t *report)
{
    uint8_t data[MCU_WIRE_DLC];

    copy_bytes(data, golden_vectors[0].data, MCU_WIRE_DLC);
    data[0] = 0x11u;
    expect_decode_status(report, MCU_CAN_ID_COMMAND, data, MCU_WIRE_DLC, MCU_CODEC_INVALID_VERSION);

    copy_bytes(data, golden_vectors[0].data, MCU_WIRE_DLC);
    data[5] = 1u;
    expect_decode_status(report, MCU_CAN_ID_COMMAND, data, MCU_WIRE_DLC, MCU_CODEC_NONZERO_RESERVED);

    copy_bytes(data, golden_vectors[3].data, MCU_WIRE_DLC);
    data[7] = 1u;
    expect_decode_status(report, MCU_CAN_ID_TELEMETRY, data, MCU_WIRE_DLC, MCU_CODEC_NONZERO_RESERVED);

    copy_bytes(data, golden_vectors[0].data, MCU_WIRE_DLC);
    data[1] = 0x80u;
    data[2] = 0x00u;
    expect_decode_status(report, MCU_CAN_ID_COMMAND, data, MCU_WIRE_DLC, MCU_CODEC_INVALID_FIELD);

    copy_bytes(data, golden_vectors[5].data, MCU_WIRE_DLC);
    data[1] = 0x7fu;
    data[2] = 0xffu;
    expect_decode_status(report, MCU_CAN_ID_STOP, data, MCU_WIRE_DLC, MCU_CODEC_INVALID_FIELD);

    copy_bytes(data, golden_vectors[0].data, MCU_WIRE_DLC);
    data[3] = MCU_WIRE_OPCODE_RESERVED;
    expect_decode_status(report, MCU_CAN_ID_COMMAND, data, MCU_WIRE_DLC, MCU_CODEC_INVALID_FIELD);
    data[3] = MCU_WIRE_OPCODE_STOP;
    expect_decode_status(report, MCU_CAN_ID_COMMAND, data, MCU_WIRE_DLC, MCU_CODEC_INVALID_FIELD);
    data[3] = 0xffu;
    expect_decode_status(report, MCU_CAN_ID_COMMAND, data, MCU_WIRE_DLC, MCU_CODEC_INVALID_FIELD);

    copy_bytes(data, golden_vectors[5].data, MCU_WIRE_DLC);
    data[3] = MCU_WIRE_OPCODE_HOLD;
    expect_decode_status(report, MCU_CAN_ID_STOP, data, MCU_WIRE_DLC, MCU_CODEC_INVALID_FIELD);

    copy_bytes(data, golden_vectors[1].data, MCU_WIRE_DLC);
    data[5] = 2u;
    expect_decode_status(report, MCU_CAN_ID_ACK, data, MCU_WIRE_DLC, MCU_CODEC_INVALID_FIELD);
    data[5] = MCU_WIRE_RESULT_ACCEPTED;
    data[6] = MCU_WIRE_FAULT_MALFORMED_FRAME;
    expect_decode_status(report, MCU_CAN_ID_ACK, data, MCU_WIRE_DLC, MCU_CODEC_INVALID_FIELD);
    data[6] = MCU_WIRE_FAULT_NONE;
    data[7] = MCU_WIRE_MODE_FAULTED;
    expect_decode_status(report, MCU_CAN_ID_ACK, data, MCU_WIRE_DLC, MCU_CODEC_INVALID_FIELD);

    copy_bytes(data, golden_vectors[2].data, MCU_WIRE_DLC);
    data[6] = MCU_WIRE_FAULT_NONE;
    expect_decode_status(report, MCU_CAN_ID_ACK, data, MCU_WIRE_DLC, MCU_CODEC_INVALID_FIELD);
    data[6] = MCU_WIRE_FAULT_ACK_TIMEOUT;
    expect_decode_status(report, MCU_CAN_ID_ACK, data, MCU_WIRE_DLC, MCU_CODEC_INVALID_FIELD);
    data[6] = MCU_WIRE_FAULT_COUNT;
    expect_decode_status(report, MCU_CAN_ID_ACK, data, MCU_WIRE_DLC, MCU_CODEC_INVALID_FIELD);

    copy_bytes(data, golden_vectors[6].data, MCU_WIRE_DLC);
    data[7] = MCU_WIRE_MODE_IDLE;
    expect_decode_status(report, MCU_CAN_ID_STOP_ACK, data, MCU_WIRE_DLC, MCU_CODEC_INVALID_FIELD);

    copy_bytes(data, golden_vectors[7].data, MCU_WIRE_DLC);
    data[6] = MCU_WIRE_FAULT_STOP_TIMEOUT;
    expect_decode_status(report, MCU_CAN_ID_STOP_ACK, data, MCU_WIRE_DLC, MCU_CODEC_INVALID_FIELD);

    copy_bytes(data, golden_vectors[3].data, MCU_WIRE_DLC);
    data[5] = MCU_WIRE_FAULT_NONE;
    data[6] = MCU_WIRE_MODE_FAULTED;
    expect_decode_status(report, MCU_CAN_ID_TELEMETRY, data, MCU_WIRE_DLC, MCU_CODEC_INVALID_FIELD);
    data[5] = MCU_WIRE_FAULT_LINK_LOST;
    data[6] = MCU_WIRE_MODE_MOVING;
    expect_decode_status(report, MCU_CAN_ID_TELEMETRY, data, MCU_WIRE_DLC, MCU_CODEC_INVALID_FIELD);
    data[5] = MCU_WIRE_FAULT_DUPLICATE_FRAME;
    data[6] = MCU_WIRE_MODE_FAULTED;
    expect_decode_status(report, MCU_CAN_ID_TELEMETRY, data, MCU_WIRE_DLC, MCU_CODEC_INVALID_FIELD);
    data[5] = 0xffu;
    expect_decode_status(report, MCU_CAN_ID_TELEMETRY, data, MCU_WIRE_DLC, MCU_CODEC_INVALID_FIELD);
}

/* 编码失败断言辅助：目标缓冲与长度用哨兵（0xa0+i / 0xa5）填充，
 * 断言编码返回 expected 且所有输出（ID、长度、8 字节）原封未动。 */
static void check_encode_failure(mcu_test_report_t *report,
                                 const mcu_wire_frame_t *frame,
                                 mcu_codec_status_t expected)
{
    uint8_t destination[MCU_WIRE_DLC];
    uint8_t before[MCU_WIRE_DLC];
    uint8_t encoded_length = 0xa5u;
    uint16_t arbitration_id = 0xa55au;
    unsigned i;

    for (i = 0u; i < MCU_WIRE_DLC; i++) {
        destination[i] = (uint8_t)(0xa0u + i);
        before[i] = destination[i];
    }
    check(report,
          mcu_frame_encode(frame,
                           &arbitration_id,
                           destination,
                           sizeof(destination),
                           &encoded_length) == expected);
    check(report, arbitration_id == 0xa55au);
    check(report, encoded_length == 0xa5u);
    check(report, bytes_equal(destination, before, MCU_WIRE_DLC));
}

/* 编码器边界与严格字段：缓冲不足、空指针参数均失败且不写输出；
 * 越界的 command_id / 序列号 / 各枚举组合均被拒绝。 */
static void test_encoder_bounds_and_strict_fields(mcu_test_report_t *report)
{
    mcu_wire_frame_t invalid;
    uint8_t guarded[MCU_WIRE_DLC] = {0xa5u, 0xa5u, 0xa5u, 0xa5u, 0xa5u, 0xa5u, 0xa5u, 0xa5u};
    uint8_t encoded_length = 0x5au;
    uint16_t arbitration_id = 0x5aa5u;

    check(report,
          mcu_frame_encode(&golden_vectors[0].frame,
                           &arbitration_id,
                           guarded,
                           MCU_WIRE_DLC - 1u,
                           &encoded_length) == MCU_CODEC_BUFFER_TOO_SMALL);
    check(report, arbitration_id == 0x5aa5u);
    check(report, encoded_length == 0x5au);
    check(report, guarded[0] == 0xa5u && guarded[MCU_WIRE_DLC - 1u] == 0xa5u);

    check(report,
          mcu_frame_encode(0,
                           &arbitration_id,
                           guarded,
                           sizeof(guarded),
                           &encoded_length) == MCU_CODEC_INVALID_ARGUMENT);
    check(report,
          mcu_frame_encode(&golden_vectors[0].frame,
                           0,
                           guarded,
                           sizeof(guarded),
                           &encoded_length) == MCU_CODEC_INVALID_ARGUMENT);
    check(report,
          mcu_frame_encode(&golden_vectors[0].frame,
                           &arbitration_id,
                           0,
                           sizeof(guarded),
                           &encoded_length) == MCU_CODEC_INVALID_ARGUMENT);
    check(report,
          mcu_frame_encode(&golden_vectors[0].frame,
                           &arbitration_id,
                           guarded,
                           sizeof(guarded),
                           0) == MCU_CODEC_INVALID_ARGUMENT);

    copy_frame(&invalid, &golden_vectors[0].frame);
    invalid.command_id = MCU_STOP_ID_MIN;
    check_encode_failure(report, &invalid, MCU_CODEC_INVALID_FIELD);
    copy_frame(&invalid, &golden_vectors[0].frame);
    invalid.sequence_no = 1u;
    check_encode_failure(report, &invalid, MCU_CODEC_INVALID_FIELD);
    copy_frame(&invalid, &golden_vectors[0].frame);
    invalid.fault_code = MCU_WIRE_FAULT_MALFORMED_FRAME;
    check_encode_failure(report, &invalid, MCU_CODEC_INVALID_FIELD);
    copy_frame(&invalid, &golden_vectors[3].frame);
    invalid.command_id = 1u;
    check_encode_failure(report, &invalid, MCU_CODEC_INVALID_FIELD);
    copy_frame(&invalid, &golden_vectors[5].frame);
    invalid.opcode = MCU_WIRE_OPCODE_MOVE;
    check_encode_failure(report, &invalid, MCU_CODEC_INVALID_FIELD);
    copy_frame(&invalid, &golden_vectors[1].frame);
    invalid.result_code = MCU_WIRE_RESULT_REJECTED;
    check_encode_failure(report, &invalid, MCU_CODEC_INVALID_FIELD);
    copy_frame(&invalid, &golden_vectors[6].frame);
    invalid.fault_code = MCU_WIRE_FAULT_STOP_REJECTED;
    check_encode_failure(report, &invalid, MCU_CODEC_INVALID_FIELD);
    copy_frame(&invalid, &golden_vectors[0].frame);
    invalid.kind = MCU_WIRE_FRAME_KIND_COUNT;
    check_encode_failure(report, &invalid, MCU_CODEC_INVALID_FIELD);
}

/* 确定性 xorshift32 伪随机数：模糊测试的种子固定为 0x54c0dec1，
 * 保证每次运行产生同一语料、同一断言数。 */
static uint32_t next_random(uint32_t *state)
{
    uint32_t value = *state;

    value ^= value << 13;
    value ^= value >> 17;
    value ^= value << 5;
    *state = value;
    return value;
}

/* 从五个普通 opcode 中确定性选取一个。 */
static mcu_wire_opcode_t random_ordinary_opcode(uint32_t value)
{
    static const mcu_wire_opcode_t opcodes[] = {
        MCU_WIRE_OPCODE_MOVE,
        MCU_WIRE_OPCODE_GRIP_OPEN,
        MCU_WIRE_OPCODE_GRIP_CLOSE,
        MCU_WIRE_OPCODE_HOLD,
        MCU_WIRE_OPCODE_HEARTBEAT,
    };

    return opcodes[value % (sizeof(opcodes) / sizeof(opcodes[0]))];
}

/* 生成一个确定性「合法」随机帧：按帧类别填入合法分区与
 * 合法枚举组合（成功/失败分支各占一半）。 */
static void random_valid_frame(uint32_t *state, mcu_wire_frame_t *frame)
{
    uint32_t value = next_random(state);

    clear_frame(frame);
    frame->kind = (mcu_wire_frame_kind_t)(value % MCU_WIRE_FRAME_KIND_COUNT);
    switch (frame->kind) {
    case MCU_WIRE_FRAME_COMMAND:
        frame->command_id = (uint16_t)(next_random(state) & MCU_COMMAND_ID_MAX);
        frame->opcode = random_ordinary_opcode(next_random(state));
        frame->retry_count = (uint8_t)next_random(state);
        break;
    case MCU_WIRE_FRAME_ACK:
        frame->command_id = (uint16_t)(next_random(state) & MCU_COMMAND_ID_MAX);
        frame->opcode = random_ordinary_opcode(next_random(state));
        frame->retry_count = (uint8_t)next_random(state);
        if ((next_random(state) & 1u) == 0u) {
            frame->device_mode = (mcu_wire_device_mode_t)(next_random(state) % MCU_WIRE_MODE_FAULTED);
        } else {
            frame->result_code = MCU_WIRE_RESULT_REJECTED;
            frame->fault_code = (next_random(state) & 1u) == 0u
                                    ? MCU_WIRE_FAULT_DUPLICATE_FRAME
                                    : MCU_WIRE_FAULT_MALFORMED_FRAME;
            frame->device_mode = MCU_WIRE_MODE_FAULTED;
        }
        break;
    case MCU_WIRE_FRAME_TELEMETRY:
        frame->sequence_no = next_random(state);
        if ((next_random(state) & 1u) == 0u) {
            frame->device_mode = (mcu_wire_device_mode_t)(next_random(state) % MCU_WIRE_MODE_FAULTED);
        } else {
            frame->fault_code = (next_random(state) & 1u) == 0u
                                    ? MCU_WIRE_FAULT_LINK_LOST
                                    : MCU_WIRE_FAULT_WATCHDOG_EXPIRED;
            frame->device_mode = MCU_WIRE_MODE_FAULTED;
        }
        break;
    case MCU_WIRE_FRAME_STOP:
        frame->command_id = (uint16_t)(next_random(state) | MCU_STOP_ID_MIN);
        frame->opcode = MCU_WIRE_OPCODE_STOP;
        frame->retry_count = (uint8_t)next_random(state);
        break;
    case MCU_WIRE_FRAME_STOP_ACK:
        frame->command_id = (uint16_t)(next_random(state) | MCU_STOP_ID_MIN);
        frame->opcode = MCU_WIRE_OPCODE_STOP;
        frame->retry_count = (uint8_t)next_random(state);
        if ((next_random(state) & 1u) == 0u) {
            frame->device_mode = MCU_WIRE_MODE_STOPPED;
        } else {
            frame->result_code = MCU_WIRE_RESULT_REJECTED;
            frame->fault_code = MCU_WIRE_FAULT_STOP_REJECTED;
            frame->device_mode = MCU_WIRE_MODE_FAULTED;
        }
        break;
    case MCU_WIRE_FRAME_KIND_COUNT:
    default:
        break;
    }
}

/* 从五种合法 CAN ID 加 0x000/0x7ff 两个越界 ID 中确定性选取。 */
static uint16_t fuzz_arbitration_id(uint32_t value)
{
    static const uint16_t ids[] = {
        MCU_CAN_ID_COMMAND,
        MCU_CAN_ID_ACK,
        MCU_CAN_ID_TELEMETRY,
        MCU_CAN_ID_STOP,
        MCU_CAN_ID_STOP_ACK,
        0x000u,
        0x7ffu,
    };

    return ids[value % (sizeof(ids) / sizeof(ids[0]))];
}

/* 有界性质与畸形模糊：4096 轮合法帧编码→解码往返必须无损；
 * 8192 轮随机字节/随机 ID/随机长度——若解码成功则要求再编码
 * 逐字节还原（规范性幂等），失败则要求输出帧未被写入。 */
static void test_bounded_properties_and_malformed_fuzz(mcu_test_report_t *report)
{
    uint32_t random_state = 0x54c0dec1u;
    unsigned iteration;

    for (iteration = 0u; iteration < 4096u; iteration++) {
        mcu_wire_frame_t original;
        mcu_wire_frame_t decoded;
        uint8_t encoded[MCU_WIRE_DLC];
        uint8_t encoded_length = 0u;
        uint16_t arbitration_id = 0u;

        random_valid_frame(&random_state, &original);
        set_sentinel_frame(&decoded);

        check(report,
              mcu_frame_encode(&original,
                               &arbitration_id,
                               encoded,
                               sizeof(encoded),
                               &encoded_length) == MCU_CODEC_OK);
        check(report,
              mcu_frame_decode(arbitration_id, encoded, encoded_length, &decoded) == MCU_CODEC_OK);
        check(report, frames_equal(&original, &decoded));
    }

    for (iteration = 0u; iteration < 8192u; iteration++) {
        mcu_wire_frame_t decoded;
        mcu_wire_frame_t before;
        uint8_t data[MCU_WIRE_DLC];
        uint8_t length;
        uint16_t arbitration_id;
        mcu_codec_status_t status;
        unsigned byte_index;

        set_sentinel_frame(&decoded);
        copy_frame(&before, &decoded);
        for (byte_index = 0u; byte_index < MCU_WIRE_DLC; byte_index++) {
            data[byte_index] = (uint8_t)next_random(&random_state);
        }
        arbitration_id = fuzz_arbitration_id(next_random(&random_state));
        length = (next_random(&random_state) & 3u) == 0u
                     ? MCU_WIRE_DLC
                     : (uint8_t)(next_random(&random_state) % 11u);
        status = mcu_frame_decode(arbitration_id, data, length, &decoded);

        if (status == MCU_CODEC_OK) {
            uint8_t reencoded[MCU_WIRE_DLC];
            uint8_t reencoded_length = 0u;
            uint16_t reencoded_id = 0u;

            check(report,
                  mcu_frame_encode(&decoded,
                                   &reencoded_id,
                                   reencoded,
                                   sizeof(reencoded),
                                   &reencoded_length) == MCU_CODEC_OK);
            check(report, reencoded_id == arbitration_id);
            check(report, reencoded_length == MCU_WIRE_DLC);
            check(report, bytes_equal(reencoded, data, MCU_WIRE_DLC));
        } else {
            check(report, frames_equal(&decoded, &before));
        }
    }
}

/* 套件入口：清零报告后依次运行五个测试组。Host 侧断言总数为
 * 20637，QEMU 侧运行同一份源码（共享套件）。 */
void mcu_frame_codec_run_tests(mcu_test_report_t *report)
{
    if (report == 0) {
        return;
    }

    report->assertions = 0u;
    report->failures = 0u;
    report->first_failure = 0u;

    test_golden_vectors(report);
    test_decode_rejects_bad_envelope_without_reads(report);
    test_decode_rejects_malformed_payloads(report);
    test_encoder_bounds_and_strict_fields(report);
    test_bounded_properties_and_malformed_fuzz(report);
}
