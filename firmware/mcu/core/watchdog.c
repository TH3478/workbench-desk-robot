/* watchdog.c —— 平台无关时序安全路径实现（软件链路看门狗与 STOP ACK 期限）。
 *
 * 职责：只在完整有效且序号全新的普通活动上装载/刷新绝对链路期限；
 * 期限到期派发 MCU_EVENT_WATCHDOG_EXPIRED 并恰发布一条故障遥测。
 * 管理 STOP_ACK 的 10 ms 交接期限：合法 STOP 立即派发，挂起槽位支持
 * 精确链路重放与协议重试回放，交接迟到时发布恰一条本地 STOP_TIMEOUT。
 *
 * 契约文档：docs/architecture/mcu-watchdog-v1.md（「受控常量」「软件链路
 *   看门狗」「STOP 确认时序」）；无符号半区间比较与故障码语义来自
 *   docs/architecture/mcu-protocol-v1.md（「时间与期限语义」「故障码
 *   注册表」）。
 *
 * 编译目标：host、qemu、ch32v307 三目标共源；core/ 不含厂商或平台头文件
 *   （firmware/mcu/README.md「唯一规则」）。
 */

#include "watchdog.h"

/* 初始化 cookie（ASCII "WDT1"）。 */
#define MCU_WATCHDOG_COOKIE 0x57445431u
/* 无符号时间比较的半区间 2^63：按契约假设任一计时窗口都短于
 * 计数器区间的一半，期限跨越 UINT64_MAX 仍保持确定性
 * （mcu-watchdog-v1.md「软件链路看门狗」）。 */
#define MCU_TIME_HALF_RANGE (UINT64_C(1) << 63)

/* 无符号半区间「已到期限」：delta = now - deadline < 2^63，
 * 包含恰好相等（闭区间端点）。 */
static bool deadline_reached(uint64_t now_us, uint64_t deadline_us)
{
    return (uint64_t)(now_us - deadline_us) < MCU_TIME_HALF_RANGE;
}

/* 无符号半区间「严格超过期限」：delta != 0 且 < 2^63；
 * 恰好相等不算迟到。 */
static bool deadline_after(uint64_t now_us, uint64_t deadline_us)
{
    uint64_t delta = now_us - deadline_us;

    return delta != 0u && delta < MCU_TIME_HALF_RANGE;
}

/* 把帧置为安全中性初值（kind == COMMAND、opcode == reserved、全零）。 */
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

/* 逐字段复制帧。 */
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

/* 清空缓存的 STOP_ACK 槽位（观测时间与帧一并归零）。 */
static void clear_stop_ack_cache(mcu_watchdog_t *watchdog)
{
    watchdog->stop_ack_observed_at_us = 0u;
    clear_frame(&watchdog->stop_ack_frame);
}

/* 缓存最近一次发出的 STOP_ACK 观测，供精确链路重放使用。 */
static void cache_stop_ack(mcu_watchdog_t *watchdog,
                           const mcu_watchdog_record_t *record)
{
    watchdog->stop_ack_observed_at_us = record->observed_at_us;
    copy_frame(&watchdog->stop_ack_frame, &record->frame);
}

/* 看门狗记录清零：kind == NONE 表示无输出。 */
static void clear_record(mcu_watchdog_record_t *record)
{
    record->kind = MCU_WATCHDOG_RECORD_NONE;
    record->fault = MCU_WATCHDOG_FAULT_NONE;
    record->observed_at_us = 0u;
    record->deadline_us = 0u;
    record->command_id = 0u;
    record->retry_count = 0u;
    clear_frame(&record->frame);
}

/* 活动分类枚举范围校验。 */
static bool activity_is_valid(mcu_watchdog_activity_t activity)
{
    return activity >= MCU_WATCHDOG_ACTIVITY_VALID_NEW && activity < MCU_WATCHDOG_ACTIVITY_COUNT;
}

/* 挂起 STOP 槽位的关联匹配：只有 command_id 相同且重试计数不低于
 * 最近发出尝试的 STOP 才命中挂起槽位（详见函数体注释）。 */
static bool stop_matches_pending(const mcu_watchdog_t *watchdog,
                                 const mcu_wire_frame_t *stop)
{
    /* retry_count 是尝试元数据，不属于 STOP 命令语义。
     * 计数相等是链路级重放，严格更大是协议重试。
     * 待处理的 uint8_t 重试序号不会回绕：
     * 更小的计数是过期流量，不得回滚关联槽位。 */
    return watchdog->stop_ack_pending && watchdog->stop_command_id == stop->command_id &&
           stop->retry_count >= watchdog->stop_retry_count;
}

/* 状态机 MCU 侧故障码到 Wire V1 故障码；NONE 与未知值映射为 NONE
 * （mcu-wire-v1.md「数字注册表」）。 */
static mcu_wire_fault_t map_fault(mcu_fault_code_t fault_code)
{
    switch (fault_code) {
    case MCU_FAULT_STOP_REJECTED:
        return MCU_WIRE_FAULT_STOP_REJECTED;
    case MCU_FAULT_LINK_LOST:
        return MCU_WIRE_FAULT_LINK_LOST;
    case MCU_FAULT_DUPLICATE_FRAME:
        return MCU_WIRE_FAULT_DUPLICATE_FRAME;
    case MCU_FAULT_WATCHDOG_EXPIRED:
        return MCU_WIRE_FAULT_WATCHDOG_EXPIRED;
    case MCU_FAULT_MALFORMED_FRAME:
        return MCU_WIRE_FAULT_MALFORMED_FRAME;
    case MCU_FAULT_NONE:
    case MCU_FAULT_COUNT:
    default:
        return MCU_WIRE_FAULT_NONE;
    }
}

/* 构造故障遥测：sequence_no 单调递增（uint32 允许回绕），
 * fault_code == watchdog_expired、device_mode == faulted。 */
static void make_watchdog_telemetry(mcu_watchdog_t *watchdog,
                                    uint64_t now_us,
                                    uint64_t deadline_us,
                                    mcu_watchdog_record_t *record)
{
    clear_record(record);
    record->kind = MCU_WATCHDOG_RECORD_FAULT_TELEMETRY;
    record->fault = MCU_WATCHDOG_FAULT_WATCHDOG_EXPIRED;
    record->observed_at_us = now_us;
    record->deadline_us = deadline_us;
    record->frame.kind = MCU_WIRE_FRAME_TELEMETRY;
    record->frame.sequence_no = watchdog->next_telemetry_sequence++;
    record->frame.fault_code = MCU_WIRE_FAULT_WATCHDOG_EXPIRED;
    record->frame.device_mode = MCU_WIRE_MODE_FAULTED;
}

/* 从 STOP 迁移结果构造 STOP_ACK：失败要求 fault_code == stop_rejected
 * 且 device_mode == faulted，成功要求 stopped
 * （mcu-protocol-v1.md「结果语义」）。 */
static void make_stop_ack(const mcu_wire_frame_t *stop,
                          const mcu_transition_result_t *transition,
                          uint64_t now_us,
                          uint64_t deadline_us,
                          mcu_watchdog_record_t *record)
{
    clear_record(record);
    record->kind = MCU_WATCHDOG_RECORD_STOP_ACK;
    record->fault = MCU_WATCHDOG_FAULT_NONE;
    record->observed_at_us = now_us;
    record->deadline_us = deadline_us;
    record->command_id = stop->command_id;
    record->retry_count = stop->retry_count;
    record->frame.kind = MCU_WIRE_FRAME_STOP_ACK;
    record->frame.command_id = stop->command_id;
    record->frame.opcode = MCU_WIRE_OPCODE_STOP;
    record->frame.retry_count = stop->retry_count;
    record->frame.result_code = transition->result_code == MCU_RESULT_ACCEPTED
                                    ? MCU_WIRE_RESULT_ACCEPTED
                                    : MCU_WIRE_RESULT_REJECTED;
    record->frame.fault_code = map_fault(transition->response_fault_code);
    record->frame.device_mode = transition->device_mode == MCU_DEVICE_MODE_FAULTED
                                    ? MCU_WIRE_MODE_FAULTED
                                    : MCU_WIRE_MODE_STOPPED;
}

/* 挂起槽位存活期内的 STOP 重试回放（mcu-watchdog-v1.md「STOP 确认
 * 时序」）：精确重试回放原始观测时间与缓存帧；协议重试使用新观测
 * 时间并回显收到的 retry_count，保留原始 result / fault / device
 * mode；两类重放都不重复 STOP 副作用、不延长期限。 */
static void replay_pending_stop_ack(mcu_watchdog_t *watchdog,
                                    const mcu_wire_frame_t *stop,
                                    uint64_t now_us,
                                    mcu_watchdog_record_t *record)
{
    bool exact_retry = watchdog->stop_retry_count == stop->retry_count;

    clear_record(record);
    record->kind = MCU_WATCHDOG_RECORD_STOP_ACK;
    record->fault = MCU_WATCHDOG_FAULT_NONE;
    record->observed_at_us = exact_retry ? watchdog->stop_ack_observed_at_us : now_us;
    record->deadline_us = watchdog->stop_deadline_us;
    record->command_id = watchdog->stop_command_id;
    record->retry_count = stop->retry_count;
    copy_frame(&record->frame, &watchdog->stop_ack_frame);
    /* 保留原始 result/fault/device mode，同时回显
     * 协议级重试携带的尝试元数据。 */
    record->frame.retry_count = stop->retry_count;
    if (!exact_retry) {
        /* 下一次完全相同的链路重试必须重放这次最近发出的尝试，
         * 同时语义响应字段保持不可变。 */
        watchdog->stop_ack_observed_at_us = now_us;
        watchdog->stop_ack_frame.retry_count = stop->retry_count;
    }
    watchdog->stop_retry_count = stop->retry_count;
}

/* ---- 初始化与合法性 ---- */

/* 初始化时序状态：链路期限与 STOP 槽位全部解除，遥测序号从
 * first_telemetry_sequence 起算。 */
void mcu_watchdog_init(mcu_watchdog_t *watchdog, uint32_t first_telemetry_sequence)
{
    if (watchdog == 0) {
        return;
    }

    watchdog->initialized = MCU_WATCHDOG_COOKIE;
    watchdog->next_telemetry_sequence = first_telemetry_sequence;
    watchdog->link_watchdog_armed = false;
    watchdog->watchdog_cause_active = false;
    watchdog->watchdog_record_emitted = false;
    watchdog->link_deadline_us = 0u;
    watchdog->stop_ack_pending = false;
    watchdog->stop_timeout_cause_active = false;
    watchdog->stop_timeout_record_emitted = false;
    watchdog->stop_deadline_us = 0u;
    watchdog->stop_command_id = 0u;
    watchdog->stop_retry_count = 0u;
    clear_stop_ack_cache(watchdog);
}

/* 合法性：cookie 匹配，挂起的 STOP command_id 必须处于
 * 0x8000..0xffff 分区。 */
bool mcu_watchdog_is_valid(const mcu_watchdog_t *watchdog)
{
    if (watchdog == 0 || watchdog->initialized != MCU_WATCHDOG_COOKIE) {
        return false;
    }
    if (watchdog->stop_ack_pending && watchdog->stop_command_id < MCU_STOP_ID_MIN) {
        return false;
    }
    return true;
}

/* ---- 软件链路看门狗 ---- */

/* 只有 EXECUTING 状态下、VALID_NEW 活动且无活动看门狗原因时才装载
 * 或刷新绝对期限 now + MCU_SOFTWARE_WATCHDOG_TIMEOUT_US（150 ms）。
 * 重试、重复、过期、畸形与 STOP 活动永不刷新执行期限。 */
bool mcu_watchdog_note_activity(mcu_watchdog_t *watchdog,
                                const mcu_state_machine_t *machine,
                                mcu_watchdog_activity_t activity,
                                uint64_t now_us)
{
    if (!mcu_watchdog_is_valid(watchdog) || machine == 0 || !mcu_sm_is_valid(machine) ||
        !activity_is_valid(activity)) {
        return false;
    }
    if (machine->state != MCU_STATE_EXECUTING) {
        watchdog->link_watchdog_armed = false;
        return false;
    }
    if (watchdog->watchdog_cause_active || activity != MCU_WATCHDOG_ACTIVITY_VALID_NEW) {
        return false;
    }

    watchdog->link_watchdog_armed = true;
    watchdog->link_deadline_us = now_us + MCU_SOFTWARE_WATCHDOG_TIMEOUT_US;
    return true;
}

/* ---- 轮询 ---- */

/* 每次调用至多发布一条记录。先检查 STOP 交接期限（迟到即关闭挂起
 * 槽位并发布恰一条 STOP_TIMEOUT，这是本地诊断、不是线上故障帧）；
 * 再检查软件链路期限：到期派发 MCU_EVENT_WATCHDOG_EXPIRED、进入锁存
 * FAULT 并恰发布一条遥测，后续轮询不再发布记录。 */
bool mcu_watchdog_poll(mcu_watchdog_t *watchdog,
                       mcu_state_machine_t *machine,
                       uint64_t now_us,
                       mcu_watchdog_record_t *record)
{
    mcu_event_t event;
    mcu_transition_result_t transition;

    if (watchdog == 0 || machine == 0 || record == 0 || !mcu_watchdog_is_valid(watchdog) ||
        !mcu_sm_is_valid(machine)) {
        return false;
    }

    if (watchdog->stop_ack_pending && deadline_after(now_us, watchdog->stop_deadline_us)) {
        clear_record(record);
        record->kind = MCU_WATCHDOG_RECORD_STOP_TIMEOUT;
        record->fault = MCU_WATCHDOG_FAULT_STOP_TIMEOUT;
        record->observed_at_us = now_us;
        record->deadline_us = watchdog->stop_deadline_us;
        record->command_id = watchdog->stop_command_id;
        record->retry_count = watchdog->stop_retry_count;
        watchdog->stop_ack_pending = false;
        clear_stop_ack_cache(watchdog);
        watchdog->stop_timeout_cause_active = true;
        watchdog->stop_timeout_record_emitted = true;
        return true;
    }

    if (machine->state != MCU_STATE_EXECUTING) {
        watchdog->link_watchdog_armed = false;
        return false;
    }
    if (!watchdog->link_watchdog_armed || !deadline_reached(now_us, watchdog->link_deadline_us) ||
        watchdog->watchdog_cause_active) {
        return false;
    }

    event.kind = MCU_EVENT_WATCHDOG_EXPIRED;
    event.fault_code = MCU_FAULT_NONE;
    event.reset_authorized = false;
    event.cause_cleared = false;
    mcu_sm_dispatch(machine, &event, &transition);
    watchdog->link_watchdog_armed = false;
    watchdog->watchdog_cause_active = true;
    if (watchdog->watchdog_record_emitted) {
        return false;
    }
    watchdog->watchdog_record_emitted = true;
    make_watchdog_telemetry(watchdog, now_us, watchdog->link_deadline_us, record);
    return true;
}

/* ---- STOP 路径与交接确认 ---- */

/* 前置条件：stop 是完整有效、编码后仲裁 ID 为 0x080 的 STOP 帧。
 * 挂起槽位存活期内且未过期时，匹配重试走回放；否则派发 STOP 事件、
 * 禁用链路期限、建立 now + MCU_STOP_ACK_DEADLINE_US（10 ms）的交接
 * 期限并缓存 STOP_ACK；状态机拒绝（FAULT）时返回 stop_rejected 的
 * STOP_ACK 且不建立挂起交接。调用方必须通过
 * mcu_watchdog_confirm_stop_ack() 确认传输交接。 */
bool mcu_watchdog_receive_stop(mcu_watchdog_t *watchdog,
                               mcu_state_machine_t *machine,
                               const mcu_wire_frame_t *stop,
                               uint64_t now_us,
                               mcu_watchdog_record_t *record)
{
    uint16_t arbitration_id;
    uint8_t encoded[MCU_WIRE_DLC];
    uint8_t encoded_length;
    mcu_transition_result_t transition;
    mcu_event_t event;

    if (watchdog == 0 || machine == 0 || stop == 0 || record == 0 ||
        !mcu_watchdog_is_valid(watchdog) || !mcu_sm_is_valid(machine)) {
        return false;
    }
    if (mcu_frame_encode(stop, &arbitration_id, encoded, sizeof(encoded), &encoded_length) != MCU_CODEC_OK ||
        arbitration_id != MCU_CAN_ID_STOP || encoded_length != MCU_WIRE_DLC) {
        return false;
    }
    if (watchdog->stop_timeout_cause_active) {
        return false;
    }
    if (watchdog->stop_ack_pending) {
        if (!stop_matches_pending(watchdog, stop) ||
            deadline_after(now_us, watchdog->stop_deadline_us)) {
            return false;
        }
        replay_pending_stop_ack(watchdog, stop, now_us, record);
        return true;
    }

    event.kind = MCU_EVENT_STOP;
    event.fault_code = MCU_FAULT_NONE;
    event.reset_authorized = false;
    event.cause_cleared = false;
    mcu_sm_dispatch(machine, &event, &transition);
    watchdog->link_watchdog_armed = false;
    watchdog->stop_ack_pending = transition.result_code == MCU_RESULT_ACCEPTED;
    watchdog->stop_deadline_us = now_us + MCU_STOP_ACK_DEADLINE_US;
    watchdog->stop_command_id = stop->command_id;
    watchdog->stop_retry_count = stop->retry_count;
    make_stop_ack(stop, &transition, now_us, watchdog->stop_deadline_us, record);
    if (watchdog->stop_ack_pending) {
        cache_stop_ack(watchdog, record);
    } else {
        clear_stop_ack_cache(watchdog);
    }
    return true;
}

/* 传输交接确认：command_id 与 retry_count 必须匹配最近发出的
 * 尝试，且恰在期限上的确认被接受（闭区间端点）、迟到被拒绝。 */
bool mcu_watchdog_confirm_stop_ack(mcu_watchdog_t *watchdog,
                                   uint16_t command_id,
                                   uint8_t retry_count,
                                   uint64_t now_us)
{
    if (!mcu_watchdog_is_valid(watchdog) || !watchdog->stop_ack_pending ||
        watchdog->stop_command_id != command_id || watchdog->stop_retry_count != retry_count ||
        deadline_after(now_us, watchdog->stop_deadline_us)) {
        return false;
    }

    watchdog->stop_ack_pending = false;
    clear_stop_ack_cache(watchdog);
    return true;
}

/* 可信安全控制操作：记录外部 Owner 已清除存续的时序原因；
 * 它本身不授权复位。 */
void mcu_watchdog_mark_causes_cleared(mcu_watchdog_t *watchdog)
{
    if (watchdog == 0 || !mcu_watchdog_is_valid(watchdog)) {
        return;
    }

    watchdog->watchdog_cause_active = false;
    watchdog->stop_timeout_cause_active = false;
}

/* 复位请求：存续的时序原因（活动原因或挂起 STOP 槽位）强制
 * cause_cleared == false，无论调用方传入什么；复位成功（进入 IDLE）
 * 后清除全部时序状态与发射标志。协议帧不能直接调用本函数——
 * 授权位来自可信控制路径。 */
bool mcu_watchdog_request_reset(mcu_watchdog_t *watchdog,
                                mcu_state_machine_t *machine,
                                bool reset_authorized,
                                bool cause_cleared,
                                mcu_transition_result_t *result)
{
    mcu_event_t event;
    bool live_cause;

    if (watchdog == 0 || machine == 0 || result == 0 || !mcu_watchdog_is_valid(watchdog)) {
        return false;
    }

    live_cause = watchdog->watchdog_cause_active || watchdog->stop_timeout_cause_active ||
                 watchdog->stop_ack_pending;
    event.kind = MCU_EVENT_TRUSTED_RESET;
    event.fault_code = MCU_FAULT_NONE;
    event.reset_authorized = reset_authorized;
    event.cause_cleared = cause_cleared && !live_cause;
    mcu_sm_dispatch(machine, &event, result);
    if (result->result_code != MCU_RESULT_ACCEPTED) {
        return false;
    }

    watchdog->link_watchdog_armed = false;
    watchdog->watchdog_cause_active = false;
    watchdog->watchdog_record_emitted = false;
    watchdog->stop_ack_pending = false;
    clear_stop_ack_cache(watchdog);
    watchdog->stop_timeout_cause_active = false;
    watchdog->stop_timeout_record_emitted = false;
    return true;
}

/* 硬件看门狗喂狗资格：状态合法、非 FAULT 且无活动时序原因；
 * 锁存 FAULT 即使原因已清除也不恢复喂狗资格。 */
bool mcu_watchdog_should_feed_hardware(const mcu_watchdog_t *watchdog,
                                       const mcu_state_machine_t *machine)
{
    return mcu_watchdog_is_valid(watchdog) && machine != 0 && mcu_sm_is_valid(machine) &&
           machine->state != MCU_STATE_FAULT && !watchdog->watchdog_cause_active &&
           !watchdog->stop_timeout_cause_active;
}
