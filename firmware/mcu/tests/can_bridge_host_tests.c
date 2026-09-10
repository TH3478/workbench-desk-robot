/* CAN 桥宿主测试套件（假 HAL 传输路径）。对应 Issue #180 的
 * 原始 HAL/Wire V1 桥的宿主传输交接（FW10）。
 *
 * 被测契约：docs/architecture/mcu-can-hal-boundary-v1.md 的传输
 * 交接部分，经由 hal/host/hal_host.c 的固定测试队列验证——五种
 * Wire 帧的发送交接、轮询无帧与畸形输入、假仲裁让 STOP 优先、
 * 损坏去重对象不压制 STOP、以及发送队列已满时 STOP 交接失败
 * 仍保持挂起（ACK 仅在 hal_can_send 接受交接后才确认）。
 *
 * 本套件只在 Host 运行（can_bridge_tests.c 的平台无关部分在
 * QEMU 同样运行）；假仲裁是确定性逻辑证据，不是物理总线时序。
 */
#include "can_bridge_host_tests.h"

#include <stdbool.h>

#include "can_bridge.h"
#include "hal_host_test.h"

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

/* 把逻辑帧重置为合法「空白」COMMAND 帧。 */
static void clear_wire_frame(mcu_wire_frame_t *frame)
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

/* 按种类构造五种合法逻辑帧（各字段取固定值）。 */
static void make_frame(mcu_wire_frame_kind_t kind, mcu_wire_frame_t *frame)
{
    clear_wire_frame(frame);
    frame->kind = kind;
    switch (kind) {
    case MCU_WIRE_FRAME_COMMAND:
        frame->command_id = 7u;
        frame->opcode = MCU_WIRE_OPCODE_MOVE;
        break;
    case MCU_WIRE_FRAME_ACK:
        frame->command_id = 7u;
        frame->opcode = MCU_WIRE_OPCODE_MOVE;
        frame->device_mode = MCU_WIRE_MODE_MOVING;
        break;
    case MCU_WIRE_FRAME_TELEMETRY:
        frame->sequence_no = 11u;
        break;
    case MCU_WIRE_FRAME_STOP:
        frame->command_id = MCU_STOP_ID_MIN;
        frame->opcode = MCU_WIRE_OPCODE_STOP;
        break;
    case MCU_WIRE_FRAME_STOP_ACK:
        frame->command_id = MCU_STOP_ID_MIN;
        frame->opcode = MCU_WIRE_OPCODE_STOP;
        frame->device_mode = MCU_WIRE_MODE_STOPPED;
        break;
    case MCU_WIRE_FRAME_KIND_COUNT:
    default:
        break;
    }
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

/* 初始化去重/状态机/看门狗（会话保持关闭，由各测试自行打开）。 */
static void initialize_core(mcu_command_dedup_t *dedup,
                            mcu_state_machine_t *machine,
                            mcu_watchdog_t *watchdog)
{
    mcu_command_dedup_init(dedup);
    mcu_sm_init(machine);
    mcu_watchdog_init(watchdog, 1u);
}

/* 假 HAL 发送全部五种 Wire 帧：send 交接入队 → 取出 → 解码
 * 逐字段还原（发送交接的确定性闭环）。 */
static void test_fake_hal_sends_all_wire_kinds(mcu_test_report_t *report)
{
    unsigned kind;

    hal_host_can_reset();
    check(report, hal_can_init());
    for (kind = 0u; kind < MCU_WIRE_FRAME_KIND_COUNT; kind++) {
        mcu_wire_frame_t original;
        mcu_wire_frame_t decoded;
        hal_can_frame encoded;

        make_frame((mcu_wire_frame_kind_t)kind, &original);
        check(report, mcu_can_bridge_send(&original) == MCU_CAN_BRIDGE_OK);
        check(report, hal_host_can_tx_count() == 1u);
        check(report, hal_host_can_take_tx(&encoded));
        check(report, mcu_can_bridge_decode(&encoded, &decoded) == MCU_CAN_BRIDGE_OK);
        check(report, wire_frames_equal(&original, &decoded));
        check(report, hal_host_can_tx_count() == 0u);
    }
}

/* 轮询无帧与畸形输入：队列空 → NO_FRAME；带 RTR 标志的帧
 * 被拒且无响应、无状态机副作用、core 保持未触碰。 */
static void test_poll_no_frame_and_malformed_input(mcu_test_report_t *report)
{
    mcu_command_dedup_t dedup;
    mcu_state_machine_t machine;
    mcu_watchdog_t watchdog;
    mcu_can_bridge_record_t record;
    mcu_wire_frame_t command;
    hal_can_frame encoded;

    hal_host_can_reset();
    check(report, hal_can_init());
    initialize_core(&dedup, &machine, &watchdog);
    check(report, mcu_command_dedup_open_session(&dedup, true));
    check(report, mcu_can_bridge_poll(&dedup, &machine, &watchdog, 10u, &record));
    check(report, record.outcome == MCU_CAN_BRIDGE_OUTCOME_NO_FRAME);
    check(report, record.status == MCU_CAN_BRIDGE_NO_FRAME);

    make_frame(MCU_WIRE_FRAME_COMMAND, &command);
    check(report, mcu_can_bridge_encode(&command, &encoded) == MCU_CAN_BRIDGE_OK);
    encoded.flags = (uint8_t)HAL_CAN_FRAME_FLAG_REMOTE;
    check(report, hal_host_can_inject_rx(&encoded));
    check(report, mcu_can_bridge_poll(&dedup, &machine, &watchdog, 11u, &record));
    check(report, record.outcome == MCU_CAN_BRIDGE_OUTCOME_REJECTED);
    check(report, record.status == MCU_CAN_BRIDGE_INVALID_FLAGS);
    check(report, !record.request_decoded);
    check(report, !record.response_handed_off);
    check(report, hal_host_can_tx_count() == 0u);
    check(report, machine.state == MCU_STATE_IDLE);
    check(report, !dedup.has_last_accepted);
}

/* 假仲裁先路由 STOP：普通帧先注入、STOP 后注入，轮询仍先
 * 取仲裁 ID 较小的 STOP；STOP_ACK 完成交接；随后队列里的
 * 普通命令因会话关闭被拒且不能撤销 STOP。 */
static void test_fake_arbitration_routes_stop_first(mcu_test_report_t *report)
{
    mcu_command_dedup_t dedup;
    mcu_state_machine_t machine;
    mcu_watchdog_t watchdog;
    mcu_can_bridge_record_t record;
    mcu_wire_frame_t command;
    mcu_wire_frame_t stop;
    mcu_wire_frame_t decoded_response;
    hal_can_frame command_encoded;
    hal_can_frame stop_encoded;
    hal_can_frame response_encoded;

    hal_host_can_reset();
    check(report, hal_can_init());
    initialize_core(&dedup, &machine, &watchdog);
    make_frame(MCU_WIRE_FRAME_COMMAND, &command);
    make_frame(MCU_WIRE_FRAME_STOP, &stop);
    check(report, mcu_can_bridge_encode(&command, &command_encoded) == MCU_CAN_BRIDGE_OK);
    check(report, mcu_can_bridge_encode(&stop, &stop_encoded) == MCU_CAN_BRIDGE_OK);
    check(report, command_encoded.arbitration_id > stop_encoded.arbitration_id);

    /* Inject ordinary traffic first. Both are pending before poll, so the
     * fake's deterministic arbitration must still expose STOP first. */
    check(report, hal_host_can_inject_rx(&command_encoded));
    check(report, hal_host_can_inject_rx(&stop_encoded));
    check(report, hal_host_can_rx_count() == 2u);
    check(report, mcu_can_bridge_poll(&dedup, &machine, &watchdog, 20u, &record));
    check(report, record.outcome == MCU_CAN_BRIDGE_OUTCOME_STOP_HANDLED);
    check(report, record.stop_path_entered);
    check(report, record.response_handed_off);
    check(report, machine.state == MCU_STATE_SAFE_STOP);
    check(report, !watchdog.stop_ack_pending);
    check(report, hal_host_can_rx_count() == 1u);
    check(report, hal_host_can_take_tx(&response_encoded));
    check(report, response_encoded.arbitration_id == MCU_CAN_ID_STOP_ACK);
    check(report, mcu_can_bridge_decode(&response_encoded, &decoded_response) == MCU_CAN_BRIDGE_OK);
    check(report, decoded_response.kind == MCU_WIRE_FRAME_STOP_ACK);
    check(report, decoded_response.command_id == stop.command_id);

    /* Ordinary dispatch is still closed, so the queued command cannot undo
     * the STOP or create a response after it is polled. */
    check(report, mcu_can_bridge_poll(&dedup, &machine, &watchdog, 21u, &record));
    check(report, record.outcome == MCU_CAN_BRIDGE_OUTCOME_SESSION_CLOSED);
    check(report, !record.ordinary_event_dispatched);
    check(report, !record.response_available);
    check(report, machine.state == MCU_STATE_SAFE_STOP);
    check(report, hal_host_can_tx_count() == 0u);
}

/* 轮询 STOP 忽略损坏的去重对象：initialized 清零不影响
 * STOP 路径——仍进入 SAFE_STOP 并完成响应交接。 */
static void test_poll_stop_ignores_corrupt_dedup(mcu_test_report_t *report)
{
    mcu_command_dedup_t dedup;
    mcu_state_machine_t machine;
    mcu_watchdog_t watchdog;
    mcu_can_bridge_record_t record;
    mcu_wire_frame_t stop;
    hal_can_frame stop_encoded;

    hal_host_can_reset();
    check(report, hal_can_init());
    initialize_core(&dedup, &machine, &watchdog);
    dedup.initialized = 0u;
    make_frame(MCU_WIRE_FRAME_STOP, &stop);
    check(report, mcu_can_bridge_encode(&stop, &stop_encoded) == MCU_CAN_BRIDGE_OK);
    check(report, hal_host_can_inject_rx(&stop_encoded));
    check(report, mcu_can_bridge_poll(&dedup, &machine, &watchdog, 25u, &record));
    check(report, record.outcome == MCU_CAN_BRIDGE_OUTCOME_STOP_HANDLED);
    check(report, record.response_handed_off);
    check(report, machine.state == MCU_STATE_SAFE_STOP);
}

/* STOP 交接失败保持挂起：先塞满假发送队列，STOP_ACK 交接被
 * hal_can_send 拒绝（HAL_SEND_FAILED），槽位保持挂起、
 * stop_command_id 不变——ACK 只在接受交接后才确认。 */
static void test_failed_stop_handoff_remains_pending(mcu_test_report_t *report)
{
    mcu_command_dedup_t dedup;
    mcu_state_machine_t machine;
    mcu_watchdog_t watchdog;
    mcu_can_bridge_record_t record;
    mcu_wire_frame_t stop;
    mcu_wire_frame_t telemetry;
    hal_can_frame stop_encoded;
    hal_can_frame filler;
    unsigned index;

    hal_host_can_reset();
    check(report, hal_can_init());
    initialize_core(&dedup, &machine, &watchdog);
    make_frame(MCU_WIRE_FRAME_STOP, &stop);
    make_frame(MCU_WIRE_FRAME_TELEMETRY, &telemetry);
    check(report, mcu_can_bridge_encode(&stop, &stop_encoded) == MCU_CAN_BRIDGE_OK);
    check(report, mcu_can_bridge_encode(&telemetry, &filler) == MCU_CAN_BRIDGE_OK);
    for (index = 0u; index < HAL_HOST_CAN_QUEUE_CAPACITY; index++) {
        check(report, hal_can_send(&filler));
    }
    check(report, hal_host_can_inject_rx(&stop_encoded));
    check(report, mcu_can_bridge_poll(&dedup, &machine, &watchdog, 30u, &record));
    check(report, record.outcome == MCU_CAN_BRIDGE_OUTCOME_STOP_HANDLED);
    check(report, record.status == MCU_CAN_BRIDGE_HAL_SEND_FAILED);
    check(report, record.response_available);
    check(report, !record.response_handed_off);
    check(report, machine.state == MCU_STATE_SAFE_STOP);
    check(report, watchdog.stop_ack_pending);
    check(report, watchdog.stop_command_id == stop.command_id);
}

/* 套件入口：清零报告后依次运行五个测试组。仅 Host 构建运行，
 * 断言总数为 106。 */
void mcu_can_bridge_host_run_tests(mcu_test_report_t *report)
{
    if (report == 0) {
        return;
    }

    report->assertions = 0u;
    report->failures = 0u;
    report->first_failure = 0u;

    test_fake_hal_sends_all_wire_kinds(report);
    test_poll_no_frame_and_malformed_input(report);
    test_fake_arbitration_routes_stop_first(report);
    test_poll_stop_ignores_corrupt_dedup(report);
    test_failed_stop_handoff_remains_pending(report);
}
