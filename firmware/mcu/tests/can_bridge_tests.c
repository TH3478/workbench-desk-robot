/* CAN 桥（原始 HAL/Wire V1 包络映射）测试套件。对应 Issue #180
 * 的原始 HAL/Wire V1 桥（FW10 传输）。
 *
 * 被测契约：docs/architecture/mcu-can-hal-boundary-v1.md 冻结的
 * 原始包络规则——HAL 暴露 11 位 arbitration_id、DLC、显式
 * extended/RTR/error/FD 标志与八个数据字节；只有无标志、DLC-8
 * 且带已知 Wire V1 ID 的标准帧才能解码；格式错误或方向错误的
 * 流量不产生状态机事件；有效 STOP 绕过普通启动会话闸门与去重
 * 窗口（firmware/mcu/README.md 的「CAN HAL/Wire 边界」）。
 *
 * 本套件是平台无关的部分，Host 与 QEMU 共享；经由假 HAL 传输
 * 的宿主专属部分在 can_bridge_host_tests.c。
 */
#include "can_bridge_tests.h"

#include <stdbool.h>

#include "can_bridge.h"

/* 桥接向量：五种逻辑帧 ↔ 对应 CAN ID 的规范映射。 */
typedef struct {
    mcu_wire_frame_t frame;
    uint16_t arbitration_id;
} bridge_vector_t;

static const bridge_vector_t bridge_vectors[] = {
    /* ① command（move，重试 2）→ 0x100。 */
    {
        .frame = {
            .kind = MCU_WIRE_FRAME_COMMAND,
            .command_id = 42u,
            .opcode = MCU_WIRE_OPCODE_MOVE,
            .retry_count = 2u,
        },
        .arbitration_id = MCU_CAN_ID_COMMAND,
    },
    /* ② ack（接受的 move 确认）→ 0x101。 */
    {
        .frame = {
            .kind = MCU_WIRE_FRAME_ACK,
            .command_id = 42u,
            .opcode = MCU_WIRE_OPCODE_MOVE,
            .retry_count = 2u,
            .result_code = MCU_WIRE_RESULT_ACCEPTED,
            .fault_code = MCU_WIRE_FAULT_NONE,
            .device_mode = MCU_WIRE_MODE_MOVING,
        },
        .arbitration_id = MCU_CAN_ID_ACK,
    },
    /* ③ telemetry（序号 9，健康）→ 0x180。 */
    {
        .frame = {
            .kind = MCU_WIRE_FRAME_TELEMETRY,
            .sequence_no = 9u,
            .fault_code = MCU_WIRE_FAULT_NONE,
            .device_mode = MCU_WIRE_MODE_IDLE,
        },
        .arbitration_id = MCU_CAN_ID_TELEMETRY,
    },
    /* ④ stop（STOP 分区下界）→ 0x080。 */
    {
        .frame = {
            .kind = MCU_WIRE_FRAME_STOP,
            .command_id = MCU_STOP_ID_MIN,
            .opcode = MCU_WIRE_OPCODE_STOP,
            .retry_count = 1u,
        },
        .arbitration_id = MCU_CAN_ID_STOP,
    },
    /* ⑤ stop_ack（接受的停止确认）→ 0x081。 */
    {
        .frame = {
            .kind = MCU_WIRE_FRAME_STOP_ACK,
            .command_id = MCU_STOP_ID_MIN,
            .opcode = MCU_WIRE_OPCODE_STOP,
            .retry_count = 1u,
            .result_code = MCU_WIRE_RESULT_ACCEPTED,
            .fault_code = MCU_WIRE_FAULT_NONE,
            .device_mode = MCU_WIRE_MODE_STOPPED,
        },
        .arbitration_id = MCU_CAN_ID_STOP_ACK,
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

/* 逐字段拷贝逻辑帧。 */
static void copy_wire_frame(mcu_wire_frame_t *destination,
                            const mcu_wire_frame_t *source)
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

/* 逻辑帧逐字段相等比较。 */
static bool wire_frames_equal(const mcu_wire_frame_t *left,
                              const mcu_wire_frame_t *right)
{
    return left->kind == right->kind && left->command_id == right->command_id &&
           left->sequence_no == right->sequence_no && left->opcode == right->opcode &&
           left->retry_count == right->retry_count && left->result_code == right->result_code &&
           left->fault_code == right->fault_code && left->device_mode == right->device_mode;
}

/* 逐字段拷贝 HAL 帧（含 8 字节数据）。 */
static void copy_hal_frame(hal_can_frame *destination,
                           const hal_can_frame *source)
{
    unsigned i;

    destination->arbitration_id = source->arbitration_id;
    destination->dlc = source->dlc;
    destination->flags = source->flags;
    for (i = 0u; i < HAL_CAN_CLASSIC_DLC_MAX; i++) {
        destination->data[i] = source->data[i];
    }
}

/* HAL 帧逐字段相等比较。 */
static bool hal_frames_equal(const hal_can_frame *left,
                             const hal_can_frame *right)
{
    unsigned i;

    if (left->arbitration_id != right->arbitration_id || left->dlc != right->dlc ||
        left->flags != right->flags) {
        return false;
    }
    for (i = 0u; i < HAL_CAN_CLASSIC_DLC_MAX; i++) {
        if (left->data[i] != right->data[i]) {
            return false;
        }
    }
    return true;
}

/* 逻辑帧哨兵（0xa5/0x5a 模式）：解码失败时输出必须保持哨兵
 * 不变，证明拒绝时不写输出。 */
static void set_wire_sentinel(mcu_wire_frame_t *frame)
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

/* HAL 帧哨兵：编码失败时输出必须保持哨兵不变。 */
static void set_hal_sentinel(hal_can_frame *frame)
{
    unsigned i;

    frame->arbitration_id = 0x0555u;
    frame->dlc = 0xa5u;
    frame->flags = 0x5au;
    for (i = 0u; i < HAL_CAN_CLASSIC_DLC_MAX; i++) {
        frame->data[i] = (uint8_t)(0xa0u + i);
    }
}

/* 初始化去重/状态机/看门狗，可选打开可信会话闸门。 */
static void initialize_core(mcu_command_dedup_t *dedup,
                            mcu_state_machine_t *machine,
                            mcu_watchdog_t *watchdog,
                            bool open_session,
                            mcu_test_report_t *report)
{
    mcu_command_dedup_init(dedup);
    mcu_sm_init(machine);
    mcu_watchdog_init(watchdog, 1u);
    if (open_session) {
        check(report, mcu_command_dedup_open_session(dedup, true));
    }
}

/* 解码失败断言辅助：以哨兵帧为输入，断言返回 expected 且
 * 输出帧未被写入。 */
static void expect_decode_failure(mcu_test_report_t *report,
                                  const hal_can_frame *encoded,
                                  mcu_can_bridge_status_t expected)
{
    mcu_wire_frame_t decoded;
    mcu_wire_frame_t before;

    set_wire_sentinel(&decoded);
    copy_wire_frame(&before, &decoded);
    check(report, mcu_can_bridge_decode(encoded, &decoded) == expected);
    check(report, wire_frames_equal(&decoded, &before));
}

/* 五种 Wire 帧往返：断言 CAN ID 优先级链、DLC/标志/版本字节，
 * 再对每条向量做 编码 → 解码 → 逻辑帧比对；编码器对非法
 * command_id 失败且不写 HAL 输出。 */
static void test_all_wire_kinds_round_trip(mcu_test_report_t *report)
{
    unsigned index;

    check(report, HAL_CAN_STANDARD_ID_MAX == 0x07ffu);
    check(report, HAL_CAN_CLASSIC_DLC_MAX == MCU_WIRE_DLC);
    check(report, MCU_CAN_ID_STOP < MCU_CAN_ID_STOP_ACK);
    check(report, MCU_CAN_ID_STOP_ACK < MCU_CAN_ID_COMMAND);
    check(report, MCU_CAN_ID_COMMAND < MCU_CAN_ID_ACK);
    check(report, MCU_CAN_ID_ACK < MCU_CAN_ID_TELEMETRY);

    for (index = 0u; index < sizeof(bridge_vectors) / sizeof(bridge_vectors[0]); index++) {
        hal_can_frame encoded;
        mcu_wire_frame_t decoded;

        set_hal_sentinel(&encoded);
        set_wire_sentinel(&decoded);
        check(report, mcu_can_bridge_encode(&bridge_vectors[index].frame, &encoded) ==
                        MCU_CAN_BRIDGE_OK);
        check(report, encoded.arbitration_id == bridge_vectors[index].arbitration_id);
        check(report, encoded.dlc == MCU_WIRE_DLC);
        check(report, encoded.flags == (uint8_t)HAL_CAN_FRAME_FLAG_NONE);
        check(report, encoded.data[0] == MCU_WIRE_VERSION_V1);
        check(report, mcu_can_bridge_decode(&encoded, &decoded) == MCU_CAN_BRIDGE_OK);
        check(report, wire_frames_equal(&decoded, &bridge_vectors[index].frame));
    }

    {
        hal_can_frame encoded;
        hal_can_frame before;
        mcu_wire_frame_t invalid;

        set_hal_sentinel(&encoded);
        copy_hal_frame(&before, &encoded);
        copy_wire_frame(&invalid, &bridge_vectors[0].frame);
        invalid.command_id = MCU_STOP_ID_MIN;
        check(report, mcu_can_bridge_encode(&invalid, &encoded) ==
                        MCU_CAN_BRIDGE_INVALID_WIRE_FIELD);
        check(report, hal_frames_equal(&encoded, &before));
    }

    check(report, mcu_can_bridge_encode(0, 0) == MCU_CAN_BRIDGE_INVALID_ARGUMENT);
    check(report, mcu_can_bridge_decode(0, 0) == MCU_CAN_BRIDGE_INVALID_ARGUMENT);
}

/* 原始信封与 Wire 拒绝：flags 非零（穷举 1..255）、ID 越界、
 * DLC ≠ 8、未知 CAN ID、版本字节、保留字节、ID 分区与 opcode
 * 分区错误，各自映射到精确状态码。 */
static void test_raw_envelope_and_wire_rejections(mcu_test_report_t *report)
{
    hal_can_frame valid;
    hal_can_frame invalid;
    unsigned dlc;
    unsigned flags;

    check(report, mcu_can_bridge_encode(&bridge_vectors[0].frame, &valid) == MCU_CAN_BRIDGE_OK);

    for (flags = 1u; flags <= UINT8_MAX; flags++) {
        copy_hal_frame(&invalid, &valid);
        invalid.flags = (uint8_t)flags;
        expect_decode_failure(report, &invalid, MCU_CAN_BRIDGE_INVALID_FLAGS);
    }

    copy_hal_frame(&invalid, &valid);
    invalid.arbitration_id = HAL_CAN_STANDARD_ID_MAX + 1u;
    expect_decode_failure(report, &invalid, MCU_CAN_BRIDGE_INVALID_ARBITRATION_ID);

    for (dlc = 0u; dlc <= UINT8_MAX; dlc++) {
        if (dlc == MCU_WIRE_DLC) {
            continue;
        }
        copy_hal_frame(&invalid, &valid);
        invalid.dlc = (uint8_t)dlc;
        expect_decode_failure(report, &invalid, MCU_CAN_BRIDGE_INVALID_DLC);
    }

    copy_hal_frame(&invalid, &valid);
    invalid.arbitration_id = HAL_CAN_STANDARD_ID_MAX;
    expect_decode_failure(report, &invalid, MCU_CAN_BRIDGE_UNSUPPORTED_ID);

    copy_hal_frame(&invalid, &valid);
    invalid.data[0] = 0x11u;
    expect_decode_failure(report, &invalid, MCU_CAN_BRIDGE_INVALID_VERSION);

    copy_hal_frame(&invalid, &valid);
    invalid.data[5] = 1u;
    expect_decode_failure(report, &invalid, MCU_CAN_BRIDGE_NONZERO_RESERVED);

    copy_hal_frame(&invalid, &valid);
    invalid.data[1] = 0x80u;
    invalid.data[2] = 0x00u;
    expect_decode_failure(report, &invalid, MCU_CAN_BRIDGE_INVALID_WIRE_FIELD);

    copy_hal_frame(&invalid, &valid);
    invalid.data[3] = (uint8_t)MCU_WIRE_OPCODE_STOP;
    expect_decode_failure(report, &invalid, MCU_CAN_BRIDGE_INVALID_WIRE_FIELD);
}

/* 断言 core 三对象未被触碰：去重仍有效且无 last_accepted、
 * 状态机 idle 无故障、看门狗未武装且无挂起 STOP ACK。 */
static void check_core_unchanged(mcu_test_report_t *report,
                                 const mcu_command_dedup_t *dedup,
                                 const mcu_state_machine_t *machine,
                                 const mcu_watchdog_t *watchdog)
{
    check(report, mcu_command_dedup_is_valid(dedup));
    check(report, !dedup->has_last_accepted);
    check(report, machine->state == MCU_STATE_IDLE);
    check(report, machine->device_mode == MCU_DEVICE_MODE_IDLE);
    check(report, machine->fault_code == MCU_FAULT_NONE);
    check(report, !watchdog->link_watchdog_armed);
    check(report, !watchdog->stop_ack_pending);
}

/* 被拒入口无安全副作用：带标志位/坏版本/错误方向的流量被拒后，
 * 去重、状态机与看门狗全部保持原状；损坏的去重对象整体拒绝。 */
static void test_rejected_ingress_has_no_safety_side_effect(mcu_test_report_t *report)
{
    static const unsigned wrong_direction_vectors[] = {1u, 2u, 4u};
    mcu_command_dedup_t dedup;
    mcu_state_machine_t machine;
    mcu_watchdog_t watchdog;
    mcu_can_bridge_record_t record;
    hal_can_frame encoded;
    unsigned index;

    initialize_core(&dedup, &machine, &watchdog, true, report);
    check(report, mcu_can_bridge_encode(&bridge_vectors[0].frame, &encoded) == MCU_CAN_BRIDGE_OK);
    encoded.flags = (uint8_t)HAL_CAN_FRAME_FLAG_EXTENDED_ID;
    check(report, mcu_can_bridge_process_frame(&dedup,
                                               &machine,
                                               &watchdog,
                                               &encoded,
                                               100u,
                                               &record));
    check(report, record.outcome == MCU_CAN_BRIDGE_OUTCOME_REJECTED);
    check(report, record.status == MCU_CAN_BRIDGE_INVALID_FLAGS);
    check(report, !record.request_decoded);
    check(report, !record.response_available);
    check_core_unchanged(report, &dedup, &machine, &watchdog);

    initialize_core(&dedup, &machine, &watchdog, true, report);
    check(report, mcu_can_bridge_encode(&bridge_vectors[0].frame, &encoded) == MCU_CAN_BRIDGE_OK);
    encoded.data[0] = 0x11u;
    check(report, mcu_can_bridge_process_frame(&dedup,
                                               &machine,
                                               &watchdog,
                                               &encoded,
                                               101u,
                                               &record));
    check(report, record.status == MCU_CAN_BRIDGE_INVALID_VERSION);
    check_core_unchanged(report, &dedup, &machine, &watchdog);

    for (index = 0u;
         index < sizeof(wrong_direction_vectors) / sizeof(wrong_direction_vectors[0]);
         index++) {
        initialize_core(&dedup, &machine, &watchdog, true, report);
        check(report,
              mcu_can_bridge_encode(&bridge_vectors[wrong_direction_vectors[index]].frame,
                                    &encoded) == MCU_CAN_BRIDGE_OK);
        check(report, mcu_can_bridge_process_frame(&dedup,
                                                   &machine,
                                                   &watchdog,
                                                   &encoded,
                                                   102u + index,
                                                   &record));
        check(report, record.request_decoded);
        check(report, record.status == MCU_CAN_BRIDGE_UNEXPECTED_DIRECTION);
        check(report, !record.response_available);
        check_core_unchanged(report, &dedup, &machine, &watchdog);
    }

    initialize_core(&dedup, &machine, &watchdog, false, report);
    dedup.initialized = 0u;
    check(report, mcu_can_bridge_encode(&bridge_vectors[0].frame, &encoded) == MCU_CAN_BRIDGE_OK);
    check(report, mcu_can_bridge_process_frame(&dedup,
                                               &machine,
                                               &watchdog,
                                               &encoded,
                                               106u,
                                               &record));
    check(report, record.outcome == MCU_CAN_BRIDGE_OUTCOME_REJECTED);
    check(report, record.status == MCU_CAN_BRIDGE_CORE_REJECTED);
    check(report, !record.ordinary_event_dispatched);
    check(report, !record.response_available);
    check(report, machine.state == MCU_STATE_IDLE);
}

/* 会话闸门与合法命令：闸门关闭 → SESSION_CLOSED 且无响应；
 * 打开后合法命令派发事件、产生 ACK 响应（未交接）；重发
 * 回放相同结果且不刷新链路期限。 */
static void test_session_gate_and_valid_command(mcu_test_report_t *report)
{
    mcu_command_dedup_t dedup;
    mcu_state_machine_t machine;
    mcu_watchdog_t watchdog;
    mcu_can_bridge_record_t record;
    hal_can_frame encoded;

    initialize_core(&dedup, &machine, &watchdog, false, report);
    check(report, mcu_can_bridge_encode(&bridge_vectors[0].frame, &encoded) == MCU_CAN_BRIDGE_OK);
    check(report, mcu_can_bridge_process_frame(&dedup,
                                               &machine,
                                               &watchdog,
                                               &encoded,
                                               200u,
                                               &record));
    check(report, record.outcome == MCU_CAN_BRIDGE_OUTCOME_SESSION_CLOSED);
    check(report, record.request_decoded);
    check(report, !record.ordinary_event_dispatched);
    check(report, !record.response_available);
    check(report, machine.state == MCU_STATE_IDLE);

    initialize_core(&dedup, &machine, &watchdog, true, report);
    check(report, mcu_can_bridge_process_frame(&dedup,
                                               &machine,
                                               &watchdog,
                                               &encoded,
                                               201u,
                                               &record));
    check(report, record.outcome == MCU_CAN_BRIDGE_OUTCOME_COMMAND_HANDLED);
    check(report, record.status == MCU_CAN_BRIDGE_OK);
    check(report, record.ordinary_event_dispatched);
    check(report, record.response_available);
    check(report, !record.response_handed_off);
    check(report, record.response.kind == MCU_WIRE_FRAME_ACK);
    check(report, record.response.command_id == bridge_vectors[0].frame.command_id);
    check(report, record.response.result_code == MCU_WIRE_RESULT_ACCEPTED);
    check(report, machine.state == MCU_STATE_EXECUTING);
    check(report, watchdog.link_watchdog_armed);

    check(report, mcu_can_bridge_process_frame(&dedup,
                                               &machine,
                                               &watchdog,
                                               &encoded,
                                               202u,
                                               &record));
    check(report, record.outcome == MCU_CAN_BRIDGE_OUTCOME_COMMAND_HANDLED);
    check(report, !record.ordinary_event_dispatched);
    check(report, record.response.result_code == MCU_WIRE_RESULT_ACCEPTED);
    check(report, machine.state == MCU_STATE_EXECUTING);
    check(report, watchdog.link_deadline_us == 201u + MCU_SOFTWARE_WATCHDOG_TIMEOUT_US);
}

/* STOP 绕过普通会话：会话关闭时 STOP 仍被处理（STOP_HANDLED、
 * 进入 stop 路径、产生可编码的 STOP_ACK、状态机 SAFE_STOP、
 * 挂起交接）；去重对象缺失也不能压制 STOP 路径。 */
static void test_stop_bypasses_ordinary_session(mcu_test_report_t *report)
{
    mcu_command_dedup_t dedup;
    mcu_state_machine_t machine;
    mcu_watchdog_t watchdog;
    mcu_can_bridge_record_t record;
    hal_can_frame encoded;
    hal_can_frame response;

    initialize_core(&dedup, &machine, &watchdog, false, report);
    check(report, mcu_can_bridge_encode(&bridge_vectors[3].frame, &encoded) == MCU_CAN_BRIDGE_OK);
    check(report, encoded.arbitration_id == MCU_CAN_ID_STOP);
    check(report, encoded.data[1] == 0x80u && encoded.data[2] == 0x00u);
    check(report, mcu_can_bridge_process_frame(&dedup,
                                               &machine,
                                               &watchdog,
                                               &encoded,
                                               300u,
                                               &record));
    check(report, record.outcome == MCU_CAN_BRIDGE_OUTCOME_STOP_HANDLED);
    check(report, record.stop_path_entered);
    check(report, !record.ordinary_event_dispatched);
    check(report, record.response_available);
    check(report, record.response.kind == MCU_WIRE_FRAME_STOP_ACK);
    check(report, record.response.command_id >= MCU_STOP_ID_MIN);
    check(report, machine.state == MCU_STATE_SAFE_STOP);
    check(report, watchdog.stop_ack_pending);
    check(report, mcu_can_bridge_encode(&record.response, &response) == MCU_CAN_BRIDGE_OK);
    check(report, response.arbitration_id == MCU_CAN_ID_STOP_ACK);

    /* Ordinary replay state is not a STOP dependency. A missing dedup object
     * cannot suppress a newly initialized watchdog/state-machine STOP path. */
    mcu_sm_init(&machine);
    mcu_watchdog_init(&watchdog, 2u);
    check(report, mcu_can_bridge_process_frame(0,
                                               &machine,
                                               &watchdog,
                                               &encoded,
                                               301u,
                                               &record));
    check(report, record.outcome == MCU_CAN_BRIDGE_OUTCOME_STOP_HANDLED);
    check(report, record.stop_path_entered);
    check(report, machine.state == MCU_STATE_SAFE_STOP);
}

/* 套件入口：清零报告后依次运行五个测试组。Host 侧断言总数
 * 为 1192，QEMU 侧运行同一份源码（共享套件）。 */
void mcu_can_bridge_run_tests(mcu_test_report_t *report)
{
    if (report == 0) {
        return;
    }

    report->assertions = 0u;
    report->failures = 0u;
    report->first_failure = 0u;

    test_all_wire_kinds_round_trip(report);
    test_raw_envelope_and_wire_rejections(report);
    test_rejected_ingress_has_no_safety_side_effect(report);
    test_session_gate_and_valid_command(report);
    test_stop_bypasses_ordinary_session(report);
}
