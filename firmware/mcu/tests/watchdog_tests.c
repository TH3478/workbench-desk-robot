/* 看门狗与 STOP 时序测试套件。对应 Issue #60（FW5）的时序安全路径。
 *
 * 被测契约：docs/architecture/mcu-watchdog-v1.md 冻结的时序安全
 * 路径——受控常量关系、只有「有效且序号为新的普通帧」才刷新
 * 链路期限、期限到达单条故障遥测、uint64 回绕、STOP ACK 交接
 * 期限（恰在期限算通过、迟到拒绝）、精确/协议重试回放、递减
 * 计数过期、STOP 超时锁存与可信复位双闸门。
 *
 * 时间由本文件自己的 fake_clock_t 假时钟驱动，与真实定时器无关；
 * 真实中断下的对应证据在 hal/qemu/main_qemu.c 的
 * run_qemu_timing_evidence 中给出。
 */
#include "watchdog_tests.h"

#include <stdbool.h>
#include <stdint.h>


#include "watchdog.h"

/* 假时钟：只携带一个微秒时间戳，由各测试显式推进，保证确定性。 */
typedef struct {
    uint64_t now_us;
} fake_clock_t;

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

/* 构造一个指定类别的事件（无故障、未授权、原因未清除）并派发。 */
static void dispatch(mcu_state_machine_t *machine,
                     mcu_event_kind_t kind,
                     mcu_transition_result_t *result)
{
    mcu_event_t event;

    event.kind = kind;
    event.fault_code = MCU_FAULT_NONE;
    event.reset_authorized = false;
    event.cause_cleared = false;
    mcu_sm_dispatch(machine, &event, result);
}

/* 初始化状态机并派发 BEGIN_MOVE，送入 EXECUTING 状态。 */
static void start_move(mcu_state_machine_t *machine)
{
    mcu_transition_result_t result;

    mcu_sm_init(machine);
    dispatch(machine, MCU_EVENT_BEGIN_MOVE, &result);
}

/* 构造一个结构上合法的 Wire V1 STOP 帧。 */
static void make_stop(mcu_wire_frame_t *stop, uint16_t command_id, uint8_t retry_count)
{
    stop->kind = MCU_WIRE_FRAME_STOP;
    stop->command_id = command_id;
    stop->sequence_no = 0u;
    stop->opcode = MCU_WIRE_OPCODE_STOP;
    stop->retry_count = retry_count;
    stop->result_code = MCU_WIRE_RESULT_ACCEPTED;
    stop->fault_code = MCU_WIRE_FAULT_NONE;
    stop->device_mode = MCU_WIRE_MODE_IDLE;
}

/* 记录携带的 Wire V1 帧必须能被编解码器编码为 DLC-8 帧
 * （发布记录前先验证线上表示合法）。 */
static bool record_frame_encodes(const mcu_watchdog_record_t *record)
{
    uint16_t arbitration_id;
    uint8_t data[MCU_WIRE_DLC];
    uint8_t length;

    return mcu_frame_encode(&record->frame, &arbitration_id, data, sizeof(data), &length) ==
             MCU_CODEC_OK &&
           length == MCU_WIRE_DLC;
}

/* 逻辑帧逐字段相等比较。 */
static bool frames_equal(const mcu_wire_frame_t *left, const mcu_wire_frame_t *right)
{
    return left->kind == right->kind && left->command_id == right->command_id &&
           left->sequence_no == right->sequence_no && left->opcode == right->opcode &&
           left->retry_count == right->retry_count && left->result_code == right->result_code &&
           left->fault_code == right->fault_code && left->device_mode == right->device_mode;
}

/* 看门狗记录逐字段相等比较（含观测/期限时间戳与线上帧）。 */
static bool records_equal(const mcu_watchdog_record_t *left,
                          const mcu_watchdog_record_t *right)
{
    return left->kind == right->kind && left->fault == right->fault &&
           left->observed_at_us == right->observed_at_us &&
           left->deadline_us == right->deadline_us && left->command_id == right->command_id &&
           left->retry_count == right->retry_count && frames_equal(&left->frame, &right->frame);
}

/* 受控常量关系（mcu-watchdog-v1.md 的常量表）：
 * 心跳周期为正；软件期限 ≥ 2 倍心跳；STOP ACK 期限为正且严格
 * 短于软件期限；硬件看门狗周期为正。 */
static void test_controlled_constants(mcu_test_report_t *report)
{
    check(report, MCU_HEARTBEAT_PERIOD_US > 0u);
    check(report, MCU_SOFTWARE_WATCHDOG_TIMEOUT_US >= 2u * MCU_HEARTBEAT_PERIOD_US);
    check(report, MCU_STOP_ACK_DEADLINE_US > 0u);
    check(report, MCU_STOP_ACK_DEADLINE_US < MCU_SOFTWARE_WATCHDOG_TIMEOUT_US);
    check(report, MCU_HARDWARE_WATCHDOG_PERIOD_MS > 0u);
}

/* 只有有效新活动才喂链路看门狗：IDLE 时 VALID_NEW 不武装
 * （无执行期间）；EXECUTING 时武装后，重试/重复/过期/畸形/STOP
 * 与越界类别都不能刷新期限。 */
static void test_only_valid_new_activity_feeds(mcu_test_report_t *report)
{
    /* 不得刷新链路期限的活动类别：重试、重复、过期、畸形与 STOP
     * （mcu-watchdog-v1.md「软件链路看门狗」）。 */
    static const mcu_watchdog_activity_t rejected_activity[] = {
        MCU_WATCHDOG_ACTIVITY_RETRY,
        MCU_WATCHDOG_ACTIVITY_DUPLICATE,
        MCU_WATCHDOG_ACTIVITY_STALE,
        MCU_WATCHDOG_ACTIVITY_MALFORMED,
        MCU_WATCHDOG_ACTIVITY_STOP,
    };
    mcu_watchdog_t watchdog;
    mcu_state_machine_t machine;
    mcu_transition_result_t transition;
    fake_clock_t clock = {.now_us = 1000u};
    uint64_t deadline;
    unsigned i;

    mcu_watchdog_init(&watchdog, 7u);
    mcu_sm_init(&machine);
    check(report,
          !mcu_watchdog_note_activity(
            &watchdog, &machine, MCU_WATCHDOG_ACTIVITY_VALID_NEW, clock.now_us));
    check(report, !watchdog.link_watchdog_armed);

    dispatch(&machine, MCU_EVENT_BEGIN_MOVE, &transition);
    check(report,
          mcu_watchdog_note_activity(
            &watchdog, &machine, MCU_WATCHDOG_ACTIVITY_VALID_NEW, clock.now_us));
    deadline = clock.now_us + MCU_SOFTWARE_WATCHDOG_TIMEOUT_US;
    check(report, watchdog.link_watchdog_armed);
    check(report, watchdog.link_deadline_us == deadline);

    for (i = 0u; i < sizeof(rejected_activity) / sizeof(rejected_activity[0]); i++) {
        clock.now_us += MCU_HEARTBEAT_PERIOD_US;
        check(report,
              !mcu_watchdog_note_activity(
                &watchdog, &machine, rejected_activity[i], clock.now_us));
        check(report, watchdog.link_deadline_us == deadline);
    }

    check(report,
          !mcu_watchdog_note_activity(
            &watchdog, &machine, MCU_WATCHDOG_ACTIVITY_COUNT, clock.now_us));
    check(report, watchdog.link_deadline_us == deadline);
}

/* 期限与单条记录：deadline-1 未到期；恰在 deadline 到期并恰好
 * 发布一条 watchdog_expired 故障遥测（序号 = 初始化参数）；
 * 之后轮询不再产生记录（锁存）。 */
static void test_watchdog_deadline_and_single_record(mcu_test_report_t *report)
{
    mcu_watchdog_t watchdog;
    mcu_state_machine_t machine;
    mcu_watchdog_record_t record;
    fake_clock_t clock = {.now_us = 9000u};
    uint64_t deadline;

    mcu_watchdog_init(&watchdog, 41u);
    start_move(&machine);
    check(report,
          mcu_watchdog_note_activity(
            &watchdog, &machine, MCU_WATCHDOG_ACTIVITY_VALID_NEW, clock.now_us));
    deadline = watchdog.link_deadline_us;

    record.kind = MCU_WATCHDOG_RECORD_STOP_ACK;
    check(report, !mcu_watchdog_poll(&watchdog, &machine, deadline - 1u, &record));
    check(report, record.kind == MCU_WATCHDOG_RECORD_STOP_ACK);
    check(report, machine.state == MCU_STATE_EXECUTING);

    check(report, mcu_watchdog_poll(&watchdog, &machine, deadline, &record));
    check(report, record.kind == MCU_WATCHDOG_RECORD_FAULT_TELEMETRY);
    check(report, record.fault == MCU_WATCHDOG_FAULT_WATCHDOG_EXPIRED);
    check(report, record.observed_at_us == deadline);
    check(report, record.deadline_us == deadline);
    check(report, record.frame.kind == MCU_WIRE_FRAME_TELEMETRY);
    check(report, record.frame.sequence_no == 41u);
    check(report, record.frame.fault_code == MCU_WIRE_FAULT_WATCHDOG_EXPIRED);
    check(report, record.frame.device_mode == MCU_WIRE_MODE_FAULTED);
    check(report, record_frame_encodes(&record));
    check(report, machine.state == MCU_STATE_FAULT);
    check(report, machine.device_mode == MCU_DEVICE_MODE_FAULTED);
    check(report, machine.fault_code == MCU_FAULT_WATCHDOG_EXPIRED);
    check(report, !watchdog.link_watchdog_armed);

    record.kind = MCU_WATCHDOG_RECORD_STOP_ACK;
    check(report, !mcu_watchdog_poll(&watchdog, &machine, deadline + 1u, &record));
    check(report, record.kind == MCU_WATCHDOG_RECORD_STOP_ACK);
}

/* uint64 时钟回绕：假时钟放在 UINT64_MAX 附近，期限越过
 * UINT64_MAX 仍按无符号半区间规则正确到期，遥测序号也回绕到 0。 */
static void test_uint64_wraparound(mcu_test_report_t *report)
{
    mcu_watchdog_t watchdog;
    mcu_state_machine_t machine;
    mcu_watchdog_record_t record;
    fake_clock_t clock = {.now_us = UINT64_MAX - 100000u};
    uint64_t deadline = clock.now_us + MCU_SOFTWARE_WATCHDOG_TIMEOUT_US;

    mcu_watchdog_init(&watchdog, UINT32_MAX);
    start_move(&machine);
    check(report,
          mcu_watchdog_note_activity(
            &watchdog, &machine, MCU_WATCHDOG_ACTIVITY_VALID_NEW, clock.now_us));
    check(report, watchdog.link_deadline_us == deadline);
    check(report, !mcu_watchdog_poll(&watchdog, &machine, deadline - 1u, &record));
    check(report, machine.state == MCU_STATE_EXECUTING);
    check(report, mcu_watchdog_poll(&watchdog, &machine, deadline, &record));
    check(report, record.frame.sequence_no == UINT32_MAX);
    check(report, watchdog.next_telemetry_sequence == 0u);
    check(report, machine.state == MCU_STATE_FAULT);
}

/* STOP ACK 交接在期限内：有效 STOP 进入 SAFE_STOP、禁用链路
 * 期限并挂起 ACK；ID 不匹配的确认被拒；恰在期限的匹配确认
 * 被接受并关闭挂起槽位。 */
static void test_stop_ack_within_bound(mcu_test_report_t *report)
{
    mcu_watchdog_t watchdog;
    mcu_state_machine_t machine;
    mcu_wire_frame_t stop;
    mcu_watchdog_record_t record;
    fake_clock_t clock = {.now_us = 50000u};
    uint64_t deadline;

    mcu_watchdog_init(&watchdog, 0u);
    start_move(&machine);
    check(report,
          mcu_watchdog_note_activity(
            &watchdog, &machine, MCU_WATCHDOG_ACTIVITY_VALID_NEW, clock.now_us));
    make_stop(&stop, MCU_STOP_ID_MIN + 9u, 2u);
    check(report, mcu_watchdog_receive_stop(&watchdog, &machine, &stop, clock.now_us, &record));
    deadline = clock.now_us + MCU_STOP_ACK_DEADLINE_US;

    check(report, machine.state == MCU_STATE_SAFE_STOP);
    check(report, machine.device_mode == MCU_DEVICE_MODE_STOPPED);
    check(report, !watchdog.link_watchdog_armed);
    check(report, watchdog.stop_ack_pending);
    check(report, watchdog.stop_deadline_us == deadline);
    check(report, record.kind == MCU_WATCHDOG_RECORD_STOP_ACK);
    check(report, record.command_id == stop.command_id);
    check(report, record.retry_count == stop.retry_count);
    check(report, record.deadline_us == deadline);
    check(report, record.observed_at_us == clock.now_us);
    check(report, record.frame.kind == MCU_WIRE_FRAME_STOP_ACK);
    check(report, record.frame.command_id == stop.command_id);
    check(report, record.frame.retry_count == stop.retry_count);
    check(report, record.frame.result_code == MCU_WIRE_RESULT_ACCEPTED);
    check(report, record.frame.fault_code == MCU_WIRE_FAULT_NONE);
    check(report, record.frame.device_mode == MCU_WIRE_MODE_STOPPED);
    check(report, record_frame_encodes(&record));

    check(report,
          !mcu_watchdog_confirm_stop_ack(
            &watchdog, (uint16_t)(stop.command_id + 1u), stop.retry_count, deadline));
    check(report, watchdog.stop_ack_pending);
    check(report,
          mcu_watchdog_confirm_stop_ack(
            &watchdog, stop.command_id, stop.retry_count, deadline));
    check(report, !watchdog.stop_ack_pending);
    check(report, !mcu_watchdog_poll(&watchdog, &machine, deadline + 1u, &record));
    check(report, machine.state == MCU_STATE_SAFE_STOP);
}

/* 故障后精确链路重试回放原始 STOP ACK：状态机先入 FAULT，同 ID
 * 同 retry_count 的 STOP 仍返回与首条逐字段相等的记录，且不
 * 延长期限、不重复副作用。 */
static void test_stop_ack_retry_replays_original_after_fault(mcu_test_report_t *report)
{
    mcu_watchdog_t watchdog;
    mcu_state_machine_t machine;
    mcu_wire_frame_t stop;
    mcu_watchdog_record_t first_record;
    mcu_watchdog_record_t retry_record;
    mcu_transition_result_t transition;
    mcu_event_t event;
    uint64_t now_us = 70000u;
    uint64_t deadline;

    mcu_watchdog_init(&watchdog, 0u);
    start_move(&machine);
    make_stop(&stop, MCU_STOP_ID_MIN + 10u, 3u);
    check(report, mcu_watchdog_receive_stop(&watchdog, &machine, &stop, now_us, &first_record));
    deadline = watchdog.stop_deadline_us;
    check(report, first_record.frame.result_code == MCU_WIRE_RESULT_ACCEPTED);
    check(report, first_record.frame.fault_code == MCU_WIRE_FAULT_NONE);
    check(report, first_record.frame.device_mode == MCU_WIRE_MODE_STOPPED);

    event.kind = MCU_EVENT_RAISE_FAULT;
    event.fault_code = MCU_FAULT_MALFORMED_FRAME;
    event.reset_authorized = false;
    event.cause_cleared = false;
    mcu_sm_dispatch(&machine, &event, &transition);
    check(report, machine.state == MCU_STATE_FAULT);
    check(report, !watchdog.watchdog_cause_active);
    check(report, !watchdog.stop_timeout_cause_active);

    check(report,
          mcu_watchdog_receive_stop(
            &watchdog, &machine, &stop, now_us + 1u, &retry_record));
    check(report, records_equal(&retry_record, &first_record));
    check(report, watchdog.stop_deadline_us == deadline);
    check(report, watchdog.stop_ack_pending);
}

/* 协议重试回显计数：retry_count + 1 的 STOP 回放原始线上结果
 * 与设备模式、回显新计数、记录新观测时间但不延长期限；再次
 * 相同计数为精确链路回放；随后确认即关闭挂起槽位。 */
static void test_stop_ack_protocol_retry_echoes_count(mcu_test_report_t *report)
{
    mcu_watchdog_t watchdog;
    mcu_state_machine_t machine;
    mcu_wire_frame_t stop;
    mcu_wire_frame_t retry;
    mcu_watchdog_record_t first_record;
    mcu_watchdog_record_t retry_record;
    mcu_watchdog_record_t duplicate_record;
    mcu_transition_result_t transition;
    mcu_event_t event;
    uint64_t now_us = 71000u;
    uint64_t retry_now_us = now_us + 2u;
    uint64_t deadline;

    mcu_watchdog_init(&watchdog, 0u);
    start_move(&machine);
    make_stop(&stop, MCU_STOP_ID_MIN + 11u, 7u);
    check(report, mcu_watchdog_receive_stop(&watchdog, &machine, &stop, now_us, &first_record));
    deadline = watchdog.stop_deadline_us;

    event.kind = MCU_EVENT_RAISE_FAULT;
    event.fault_code = MCU_FAULT_MALFORMED_FRAME;
    event.reset_authorized = false;
    event.cause_cleared = false;
    mcu_sm_dispatch(&machine, &event, &transition);
    check(report, machine.state == MCU_STATE_FAULT);

    make_stop(&retry, stop.command_id, (uint8_t)(stop.retry_count + 1u));
    check(report,
          mcu_watchdog_receive_stop(&watchdog, &machine, &retry, retry_now_us, &retry_record));
    check(report, retry_record.kind == MCU_WATCHDOG_RECORD_STOP_ACK);
    check(report, retry_record.command_id == stop.command_id);
    check(report, retry_record.retry_count == retry.retry_count);
    check(report, retry_record.frame.retry_count == retry.retry_count);
    check(report, retry_record.frame.result_code == first_record.frame.result_code);
    check(report, retry_record.frame.fault_code == first_record.frame.fault_code);
    check(report, retry_record.frame.device_mode == first_record.frame.device_mode);
    check(report, retry_record.observed_at_us == retry_now_us);
    check(report, retry_record.deadline_us == deadline);
    check(report, watchdog.stop_deadline_us == deadline);
    check(report, watchdog.stop_retry_count == retry.retry_count);
    check(report, record_frame_encodes(&retry_record));

    check(report,
          mcu_watchdog_receive_stop(
            &watchdog, &machine, &retry, retry_now_us + 1u, &duplicate_record));
    check(report, records_equal(&duplicate_record, &retry_record));
    check(report,
          mcu_watchdog_confirm_stop_ack(
            &watchdog, retry.command_id, retry.retry_count, deadline));
    check(report, !watchdog.stop_ack_pending);
}

/* 递减/过期重试被拒：接受 0→1→2 后，回退的 1 被拒绝且不改变
 * 挂起槽位；255→0 的回绕同理。两种过期计数都不能完成确认。 */
static void test_stop_ack_rejects_stale_protocol_retry(mcu_test_report_t *report)
{
    mcu_watchdog_t watchdog;
    mcu_state_machine_t machine;
    mcu_wire_frame_t stop;
    mcu_wire_frame_t retry;
    mcu_wire_frame_t stale;
    mcu_watchdog_record_t record;
    mcu_watchdog_record_t latest_record;
    mcu_watchdog_record_t stale_record;
    uint64_t now_us = 72000u;
    uint64_t deadline;

    mcu_watchdog_init(&watchdog, 0u);
    start_move(&machine);
    make_stop(&stop, MCU_STOP_ID_MIN + 12u, 0u);
    check(report, mcu_watchdog_receive_stop(&watchdog, &machine, &stop, now_us, &record));
    deadline = watchdog.stop_deadline_us;

    make_stop(&retry, stop.command_id, 1u);
    check(report,
          mcu_watchdog_receive_stop(&watchdog, &machine, &retry, now_us + 1u, &record));
    make_stop(&retry, stop.command_id, 2u);
    check(report,
          mcu_watchdog_receive_stop(&watchdog, &machine, &retry, now_us + 2u, &latest_record));
    check(report, watchdog.stop_retry_count == retry.retry_count);
    check(report, watchdog.stop_ack_pending);

    make_stop(&stale, stop.command_id, 1u);
    stale_record.kind = MCU_WATCHDOG_RECORD_NONE;
    check(report,
          !mcu_watchdog_receive_stop(&watchdog, &machine, &stale, now_us + 3u, &stale_record));
    check(report, stale_record.kind == MCU_WATCHDOG_RECORD_NONE);
    check(report, watchdog.stop_retry_count == retry.retry_count);
    check(report, watchdog.stop_deadline_us == deadline);
    check(report, watchdog.stop_ack_pending);
    check(report,
          !mcu_watchdog_confirm_stop_ack(
            &watchdog, stale.command_id, stale.retry_count, deadline));
    check(report,
          mcu_watchdog_confirm_stop_ack(
            &watchdog, retry.command_id, retry.retry_count, deadline));
    check(report, !watchdog.stop_ack_pending);

    mcu_watchdog_init(&watchdog, 0u);
    start_move(&machine);
    make_stop(&stop, MCU_STOP_ID_MIN + 13u, 255u);
    check(report, mcu_watchdog_receive_stop(&watchdog, &machine, &stop, now_us, &record));
    deadline = watchdog.stop_deadline_us;
    make_stop(&stale, stop.command_id, 0u);
    stale_record.kind = MCU_WATCHDOG_RECORD_NONE;
    check(report,
          !mcu_watchdog_receive_stop(&watchdog, &machine, &stale, now_us + 1u, &stale_record));
    check(report, stale_record.kind == MCU_WATCHDOG_RECORD_NONE);
    check(report, watchdog.stop_retry_count == 255u);
    check(report, watchdog.stop_deadline_us == deadline);
    check(report, watchdog.stop_ack_pending);
    check(report,
          !mcu_watchdog_confirm_stop_ack(
            &watchdog, stale.command_id, stale.retry_count, deadline));
    check(report,
          mcu_watchdog_confirm_stop_ack(
            &watchdog, stop.command_id, stop.retry_count, deadline));
    check(report, !watchdog.stop_ack_pending);
}

/* STOP 超时独立且锁存：期限后未确认关闭挂起槽位并恰好发布一条
 * 本地 STOP_TIMEOUT 记录（不是有效 Wire V1 帧）；安全状态保持；
 * 迟到确认被拒；stop_timeout_cause_active 必须在原因清除后才能
 * 通过双闸门可信复位。 */
static void test_stop_timeout_is_distinct_and_latched(mcu_test_report_t *report)
{
    mcu_watchdog_t watchdog;
    mcu_state_machine_t machine;
    mcu_wire_frame_t stop;
    mcu_watchdog_record_t record;
    mcu_transition_result_t reset;
    fake_clock_t clock = {.now_us = UINT64_MAX - 4000u};
    uint64_t deadline = clock.now_us + MCU_STOP_ACK_DEADLINE_US;

    mcu_watchdog_init(&watchdog, 0u);
    start_move(&machine);
    make_stop(&stop, MCU_STOP_ID_MIN, 255u);
    check(report, mcu_watchdog_receive_stop(&watchdog, &machine, &stop, clock.now_us, &record));
    check(report, !mcu_watchdog_poll(&watchdog, &machine, deadline, &record));
    check(report, watchdog.stop_ack_pending);
    check(report, mcu_watchdog_poll(&watchdog, &machine, deadline + 1u, &record));
    check(report, record.kind == MCU_WATCHDOG_RECORD_STOP_TIMEOUT);
    check(report, record.fault == MCU_WATCHDOG_FAULT_STOP_TIMEOUT);
    check(report, record.command_id == stop.command_id);
    check(report, record.retry_count == stop.retry_count);
    check(report, record.deadline_us == deadline);
    check(report, machine.state == MCU_STATE_SAFE_STOP);
    check(report, machine.device_mode == MCU_DEVICE_MODE_STOPPED);
    check(report, !watchdog.stop_ack_pending);
    check(report, watchdog.stop_timeout_cause_active);
    check(report,
          !mcu_watchdog_receive_stop(&watchdog, &machine, &stop, deadline + 2u, &record));
    check(report,
          !mcu_watchdog_confirm_stop_ack(
            &watchdog, stop.command_id, stop.retry_count, deadline + 1u));

    record.kind = MCU_WATCHDOG_RECORD_STOP_ACK;
    check(report, !mcu_watchdog_poll(&watchdog, &machine, deadline + 2u, &record));
    check(report, record.kind == MCU_WATCHDOG_RECORD_STOP_ACK);
    check(report,
          !mcu_watchdog_request_reset(&watchdog, &machine, true, true, &reset));
    check(report, reset.reason == MCU_REASON_RESET_CAUSE_ACTIVE);
    check(report, machine.state == MCU_STATE_SAFE_STOP);
    mcu_watchdog_mark_causes_cleared(&watchdog);
    check(report,
          !mcu_watchdog_request_reset(&watchdog, &machine, false, true, &reset));
    check(report, reset.reason == MCU_REASON_RESET_NOT_AUTHORIZED);
    check(report, mcu_watchdog_request_reset(&watchdog, &machine, true, true, &reset));
    check(report, machine.state == MCU_STATE_IDLE);
}

/* 可信复位要求原因清除「现场」：看门狗到期后未清除原因的重置
 * 被拒（RESET_CAUSE_ACTIVE）；mark_causes_cleared 之后才被接受
 * 进入 idle，且未发出任何看门狗记录。 */
static void test_watchdog_reset_requires_live_cause_clear(mcu_test_report_t *report)
{
    mcu_watchdog_t watchdog;
    mcu_state_machine_t machine;
    mcu_watchdog_record_t record;
    mcu_transition_result_t reset;
    fake_clock_t clock = {.now_us = 8u};

    mcu_watchdog_init(&watchdog, 0u);
    start_move(&machine);
    check(report,
          mcu_watchdog_note_activity(
            &watchdog, &machine, MCU_WATCHDOG_ACTIVITY_VALID_NEW, clock.now_us));
    check(report,
          mcu_watchdog_poll(
            &watchdog, &machine, clock.now_us + MCU_SOFTWARE_WATCHDOG_TIMEOUT_US, &record));
    check(report, watchdog.watchdog_cause_active);
    check(report,
          !mcu_watchdog_request_reset(&watchdog, &machine, true, true, &reset));
    check(report, reset.reason == MCU_REASON_RESET_CAUSE_ACTIVE);
    check(report, machine.state == MCU_STATE_FAULT);
    mcu_watchdog_mark_causes_cleared(&watchdog);
    check(report, mcu_watchdog_request_reset(&watchdog, &machine, true, true, &reset));
    check(report, machine.state == MCU_STATE_IDLE);
    check(report, !watchdog.watchdog_record_emitted);
}

/* 锁存故障下的硬件喂狗策略：到期进入 FAULT 后 core 停止喂
 * 硬件看门狗；清除时序原因不复活锁存的 FAULT 状态，仍不喂；
 * 由畸形帧进入的 FAULT（无时序原因）同样不喂。 */
static void test_hardware_feed_policy_in_latched_fault(mcu_test_report_t *report)
{
    mcu_watchdog_t watchdog;
    mcu_state_machine_t machine;
    mcu_watchdog_record_t record;
    mcu_transition_result_t transition;
    mcu_event_t event;
    fake_clock_t clock = {.now_us = 21u};

    mcu_watchdog_init(&watchdog, 0u);
    start_move(&machine);
    check(report,
          mcu_watchdog_note_activity(
            &watchdog, &machine, MCU_WATCHDOG_ACTIVITY_VALID_NEW, clock.now_us));
    check(report,
          mcu_watchdog_poll(
            &watchdog, &machine, clock.now_us + MCU_SOFTWARE_WATCHDOG_TIMEOUT_US, &record));
    check(report, machine.state == MCU_STATE_FAULT);
    check(report, watchdog.watchdog_cause_active);
    check(report, !mcu_watchdog_should_feed_hardware(&watchdog, &machine));

    mcu_watchdog_mark_causes_cleared(&watchdog);
    check(report, machine.state == MCU_STATE_FAULT);
    check(report, !watchdog.watchdog_cause_active);
    check(report, !mcu_watchdog_should_feed_hardware(&watchdog, &machine));

    mcu_sm_init(&machine);
    event.kind = MCU_EVENT_RAISE_FAULT;
    event.fault_code = MCU_FAULT_MALFORMED_FRAME;
    event.reset_authorized = false;
    event.cause_cleared = false;
    mcu_sm_dispatch(&machine, &event, &transition);
    check(report, machine.state == MCU_STATE_FAULT);
    check(report, !watchdog.watchdog_cause_active);
    check(report, !watchdog.stop_timeout_cause_active);
    check(report, !mcu_watchdog_should_feed_hardware(&watchdog, &machine));
}

/* 无效输入与喂狗闸门：空指针参数失败即拒绝；模式与状态不一致
 * 时不喂狗；STOP 帧 opcode 非 stop 被拒且无副作用；未初始化的
 * 看门狗对象不可用。 */
static void test_invalid_inputs_and_hardware_feed_gate(mcu_test_report_t *report)
{
    mcu_watchdog_t watchdog;
    mcu_state_machine_t machine;
    mcu_watchdog_record_t record;
    mcu_wire_frame_t stop;

    mcu_watchdog_init(&watchdog, 0u);
    mcu_sm_init(&machine);
    check(report, mcu_watchdog_is_valid(&watchdog));
    check(report, mcu_watchdog_should_feed_hardware(&watchdog, &machine));
    check(report, !mcu_watchdog_should_feed_hardware(0, &machine));
    check(report, !mcu_watchdog_should_feed_hardware(&watchdog, 0));

    machine.device_mode = MCU_DEVICE_MODE_MOVING;
    check(report, !mcu_watchdog_should_feed_hardware(&watchdog, &machine));
    check(report,
          !mcu_watchdog_note_activity(
            &watchdog, &machine, MCU_WATCHDOG_ACTIVITY_VALID_NEW, 0u));
    check(report, !mcu_watchdog_poll(&watchdog, &machine, 0u, &record));

    mcu_sm_init(&machine);
    make_stop(&stop, MCU_STOP_ID_MIN, 0u);
    stop.opcode = MCU_WIRE_OPCODE_HEARTBEAT;
    check(report, !mcu_watchdog_receive_stop(&watchdog, &machine, &stop, 0u, &record));
    check(report, machine.state == MCU_STATE_IDLE);
    check(report, !watchdog.stop_ack_pending);

    watchdog.initialized = 0u;
    check(report, !mcu_watchdog_is_valid(&watchdog));
    check(report, !mcu_watchdog_poll(&watchdog, &machine, 0u, &record));
}

/* 套件入口：清零报告后依次运行十二个测试组。Host 侧断言总数
 * 为 184，QEMU 侧运行同一份源码（共享套件）。 */
void mcu_watchdog_run_tests(mcu_test_report_t *report)
{
    if (report == 0) {
        return;
    }

    report->assertions = 0u;
    report->failures = 0u;
    report->first_failure = 0u;

    test_controlled_constants(report);
    test_only_valid_new_activity_feeds(report);
    test_watchdog_deadline_and_single_record(report);
    test_uint64_wraparound(report);
    test_stop_ack_within_bound(report);
    test_stop_ack_retry_replays_original_after_fault(report);
    test_stop_ack_protocol_retry_echoes_count(report);
    test_stop_ack_rejects_stale_protocol_retry(report);
    test_stop_timeout_is_distinct_and_latched(report);
    test_watchdog_reset_requires_live_cause_clear(report);
    test_hardware_feed_policy_in_latched_fault(report);
    test_invalid_inputs_and_hardware_feed_gate(report);
}
