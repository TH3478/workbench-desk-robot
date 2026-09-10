/* command_dedup.c —— 普通命令回放保护与关联分发的平台无关入口。
 *
 * 职责：用八条定长记录缓存普通 ACK，按冻结的 15 位半区间 delta 分类
 * 序号；序号全新的命令向安全状态机恰好分发一个事件，精确重复与递增
 * 的协议重试回放缓存结果；冲突、递减、过期与被驱逐的尝试以
 * duplicate_frame 失败即拒绝。整个实现无分配、无平台 API。
 *
 * 契约文档：docs/architecture/mcu-command-dedup-v1.md（「序号分类」「新命令
 *   与回放行为」「会话与重置边界」「STOP 独立性」）；delta 分类与故障
 *   语义来自 docs/architecture/mcu-protocol-v1.md（「关联、重试与回绕
 *   语义」「故障码注册表」）。
 *
 * 编译目标：host、qemu、ch32v307 三目标共源；core/ 不含厂商或平台头文件
 *   （firmware/mcu/README.md「唯一规则」）。
 */

#include "command_dedup.h"

/* 初始化 cookie（ASCII "DED1"）：与编译期存储预算断言一起保证
 * 状态只能来自 mcu_command_dedup_init()，回放窗口绝不触发堆分配。 */
#define MCU_COMMAND_DEDUP_COOKIE 0x44454431u

_Static_assert(sizeof(mcu_command_dedup_t) <= MCU_COMMAND_DEDUP_STORAGE_BUDGET_BYTES,
               "command dedup state exceeds its fixed storage budget");

/* 普通 opcode 集合（对应 mcu-wire-v1.md「数字注册表」）。 */
static bool ordinary_opcode_is_valid(mcu_wire_opcode_t opcode)
{
    return opcode == MCU_WIRE_OPCODE_MOVE || opcode == MCU_WIRE_OPCODE_GRIP_OPEN ||
           opcode == MCU_WIRE_OPCODE_GRIP_CLOSE || opcode == MCU_WIRE_OPCODE_HOLD ||
           opcode == MCU_WIRE_OPCODE_HEARTBEAT;
}

/* 缓存 ACK 的结果语义必须与帧编解码器相同：accepted 要求无故障且
 * 模式无故障；rejected 要求 faulted 且 fault_code 恰为
 * duplicate_frame 或 malformed_frame 之一。 */
static bool cached_response_is_valid(const mcu_command_replay_entry_t *entry)
{
    if (!entry->valid || entry->command_id > MCU_COMMAND_ID_MAX ||
        !ordinary_opcode_is_valid(entry->opcode)) {
        return false;
    }
    if (entry->result_code == MCU_WIRE_RESULT_ACCEPTED) {
        return entry->fault_code == MCU_WIRE_FAULT_NONE &&
               entry->device_mode >= MCU_WIRE_MODE_IDLE &&
               entry->device_mode <= MCU_WIRE_MODE_STOPPED;
    }
    return entry->result_code == MCU_WIRE_RESULT_REJECTED &&
           (entry->fault_code == MCU_WIRE_FAULT_DUPLICATE_FRAME ||
            entry->fault_code == MCU_WIRE_FAULT_MALFORMED_FRAME) &&
           entry->device_mode == MCU_WIRE_MODE_FAULTED;
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

/* 记录清零：无结果、无 ACK、未派发事件、未刷新看门狗。 */
static void clear_record(mcu_command_record_t *record)
{
    record->outcome = MCU_COMMAND_OUTCOME_NONE;
    record->ack_available = false;
    record->ordinary_event_dispatched = false;
    record->watchdog_refreshed = false;
    clear_frame(&record->ack);
}

/* 回放条目清零：valid == false 表示槽位空闲。 */
static void clear_entry(mcu_command_replay_entry_t *entry)
{
    entry->valid = false;
    entry->command_id = 0u;
    entry->opcode = MCU_WIRE_OPCODE_RESERVED;
    entry->last_retry_count = 0u;
    entry->result_code = MCU_WIRE_RESULT_ACCEPTED;
    entry->fault_code = MCU_WIRE_FAULT_NONE;
    entry->device_mode = MCU_WIRE_MODE_IDLE;
}

/* 清空整个回放窗口：丢弃 last_accepted 与全部条目，next_slot 归零。
 * 注意：这不等于会话建立，新序号纪元仍受会话闸门约束。 */
static void clear_history(mcu_command_dedup_t *dedup)
{
    unsigned i;

    dedup->has_last_accepted = false;
    dedup->last_accepted_id = 0u;
    dedup->next_slot = 0u;
    for (i = 0u; i < MCU_COMMAND_REPLAY_WINDOW_SIZE; i++) {
        clear_entry(&dedup->entries[i]);
    }
}

/* ---- 生命周期与会话闸门 ---- */

/* 启动初始化：普通命令派发关闭、历史为空
 * （mcu-command-dedup-v1.md「会话与重置边界」）。
 * STOP 路径归看门狗所有，不受此闸门限制。 */
void mcu_command_dedup_init(mcu_command_dedup_t *dedup)
{
    if (dedup == 0) {
        return;
    }

    dedup->initialized = MCU_COMMAND_DEDUP_COOKIE;
    dedup->session_open = false;
    clear_history(dedup);
}

/* 结构不变量校验：cookie、next_slot 范围、last_accepted 处于普通分区
 * （0x0000..0x7fff）、有效条目两两不同且 last_accepted 恰被一条有效
 * 条目命中；会话关闭时历史必须为空。 */
bool mcu_command_dedup_is_valid(const mcu_command_dedup_t *dedup)
{
    unsigned i;
    unsigned j;
    unsigned last_matches = 0u;
    unsigned valid_entries = 0u;

    if (dedup == 0 || dedup->initialized != MCU_COMMAND_DEDUP_COOKIE ||
        dedup->next_slot >= MCU_COMMAND_REPLAY_WINDOW_SIZE) {
        return false;
    }
    if (dedup->has_last_accepted && dedup->last_accepted_id > MCU_COMMAND_ID_MAX) {
        return false;
    }

    for (i = 0u; i < MCU_COMMAND_REPLAY_WINDOW_SIZE; i++) {
        const mcu_command_replay_entry_t *entry = &dedup->entries[i];

        if (!entry->valid) {
            continue;
        }
        valid_entries++;
        if (!cached_response_is_valid(entry)) {
            return false;
        }
        if (dedup->has_last_accepted && entry->command_id == dedup->last_accepted_id) {
            last_matches++;
        }
        for (j = i + 1u; j < MCU_COMMAND_REPLAY_WINDOW_SIZE; j++) {
            if (dedup->entries[j].valid && dedup->entries[j].command_id == entry->command_id) {
                return false;
            }
        }
    }

    if (!dedup->session_open) {
        return !dedup->has_last_accepted && valid_entries == 0u && dedup->next_slot == 0u;
    }
    if (!dedup->has_last_accepted) {
        return valid_entries == 0u && dedup->next_slot == 0u;
    }
    return valid_entries > 0u && last_matches == 1u;
}

/* 可信传输闸门：仅在排队的会话前流量已丢弃且当前会话关闭时打开；
 * 打开即清空历史。重复打开被拒绝，避免调用方静默抹除回放历史。 */
bool mcu_command_dedup_open_session(mcu_command_dedup_t *dedup,
                                    bool queued_traffic_discarded)
{
    if (!mcu_command_dedup_is_valid(dedup) || dedup->session_open ||
        !queued_traffic_discarded) {
        return false;
    }

    clear_history(dedup);
    dedup->session_open = true;
    return true;
}

/* 关闭会话并清空回放历史。 */
bool mcu_command_dedup_close_session(mcu_command_dedup_t *dedup)
{
    if (!mcu_command_dedup_is_valid(dedup)) {
        return false;
    }

    clear_history(dedup);
    dedup->session_open = false;
    return true;
}

/* ---- 序号分类与回放 ---- */

/* 线性查找保留窗口内 command_id 相同的条目；未命中返回 0。 */
static mcu_command_replay_entry_t *find_entry(mcu_command_dedup_t *dedup,
                                               uint16_t command_id)
{
    unsigned i;

    for (i = 0u; i < MCU_COMMAND_REPLAY_WINDOW_SIZE; i++) {
        if (dedup->entries[i].valid && dedup->entries[i].command_id == command_id) {
            return &dedup->entries[i];
        }
    }
    return 0;
}

/* 状态机设备模式到 Wire V1 设备模式的映射；
 * faulted 与未知值映射为 faulted（失败即拒绝）。 */
static mcu_wire_device_mode_t map_mode(mcu_device_mode_t mode)
{
    switch (mode) {
    case MCU_DEVICE_MODE_IDLE:
        return MCU_WIRE_MODE_IDLE;
    case MCU_DEVICE_MODE_MOVING:
        return MCU_WIRE_MODE_MOVING;
    case MCU_DEVICE_MODE_HOLDING:
        return MCU_WIRE_MODE_HOLDING;
    case MCU_DEVICE_MODE_STOPPED:
        return MCU_WIRE_MODE_STOPPED;
    case MCU_DEVICE_MODE_FAULTED:
    case MCU_DEVICE_MODE_COUNT:
    default:
        return MCU_WIRE_MODE_FAULTED;
    }
}

/* 构造普通 ACK：回显 command_id、opcode 与 retry_count
 * （mcu-wire-v1.md「载荷布局」）。 */
static void make_ack(const mcu_wire_frame_t *command,
                     mcu_wire_result_t result_code,
                     mcu_wire_fault_t fault_code,
                     mcu_wire_device_mode_t device_mode,
                     mcu_wire_frame_t *ack)
{
    clear_frame(ack);
    ack->kind = MCU_WIRE_FRAME_ACK;
    ack->command_id = command->command_id;
    ack->opcode = command->opcode;
    ack->retry_count = command->retry_count;
    ack->result_code = result_code;
    ack->fault_code = fault_code;
    ack->device_mode = device_mode;
}

/* 用缓存条目重放 ACK：结果、故障与设备模式来自缓存，
 * 尝试元数据（retry_count）回显本次请求。 */
static void make_cached_ack(const mcu_wire_frame_t *command,
                            const mcu_command_replay_entry_t *entry,
                            mcu_wire_frame_t *ack)
{
    make_ack(command,
             entry->result_code,
             entry->fault_code,
             entry->device_mode,
             ack);
}

/* 向状态机派发 RAISE_FAULT / duplicate_frame，使冲突、递减与过期
 * 尝试进入或保持锁存的 FAULT 状态。 */
static void raise_duplicate_fault(mcu_state_machine_t *machine)
{
    mcu_event_t event;
    mcu_transition_result_t transition;

    event.kind = MCU_EVENT_RAISE_FAULT;
    event.fault_code = MCU_FAULT_DUPLICATE_FRAME;
    event.reset_authorized = false;
    event.cause_cleared = false;
    mcu_sm_dispatch(machine, &event, &transition);
}

/* 关联拒绝的统一失败路径：不分发请求事件、进入/保持 FAULT、
 * 返回带 duplicate_frame 的拒绝 ACK、且绝不刷新软件看门狗
 * （mcu-command-dedup-v1.md「新命令与回放行为」）。 */
static void reject_correlation(mcu_state_machine_t *machine,
                               mcu_watchdog_t *watchdog,
                               const mcu_wire_frame_t *command,
                               uint64_t now_us,
                               mcu_command_outcome_t outcome,
                               mcu_watchdog_activity_t activity,
                               mcu_command_record_t *record)
{
    raise_duplicate_fault(machine);
    clear_record(record);
    record->outcome = outcome;
    record->ack_available = true;
    make_ack(command,
             MCU_WIRE_RESULT_REJECTED,
             MCU_WIRE_FAULT_DUPLICATE_FRAME,
             MCU_WIRE_MODE_FAULTED,
             &record->ack);
    record->watchdog_refreshed =
      mcu_watchdog_note_activity(watchdog, machine, activity, now_us);
}

/* opcode 到安全事件的映射（mcu-command-dedup-v1.md「新命令与回放
 * 行为」表格）：hold → BEGIN_HOLD；heartbeat → HEARTBEAT；
 * move 与夹爪命令 → BEGIN_MOVE。 */
static mcu_event_kind_t opcode_to_event(mcu_wire_opcode_t opcode)
{
    switch (opcode) {
    case MCU_WIRE_OPCODE_HOLD:
        return MCU_EVENT_BEGIN_HOLD;
    case MCU_WIRE_OPCODE_HEARTBEAT:
        return MCU_EVENT_HEARTBEAT;
    case MCU_WIRE_OPCODE_MOVE:
    case MCU_WIRE_OPCODE_GRIP_OPEN:
    case MCU_WIRE_OPCODE_GRIP_CLOSE:
    case MCU_WIRE_OPCODE_RESERVED:
    case MCU_WIRE_OPCODE_STOP:
    case MCU_WIRE_OPCODE_COUNT:
    default:
        return MCU_EVENT_BEGIN_MOVE;
    }
}

/* 构造并派发新命令的安全事件；事件不携带协议外的授权位。 */
static void dispatch_new_command(mcu_state_machine_t *machine,
                                 const mcu_wire_frame_t *command,
                                 mcu_transition_result_t *transition)
{
    mcu_event_t event;

    event.kind = opcode_to_event(command->opcode);
    event.fault_code = MCU_FAULT_NONE;
    event.reset_authorized = false;
    event.cause_cleared = false;
    mcu_sm_dispatch(machine, &event, transition);
}

/* 把产生的 ACK 缓存进 next_slot 槽位并环形前进（模 8）；第九条不同
 * 的已接受命令覆盖最旧槽位，被驱逐 ID 的后续流量以
 * duplicate_frame 失败（mcu-command-dedup-v1.md「序号分类」）。 */
static void cache_new_result(mcu_command_dedup_t *dedup,
                             const mcu_wire_frame_t *command,
                             const mcu_wire_frame_t *ack)
{
    mcu_command_replay_entry_t *entry = &dedup->entries[dedup->next_slot];

    entry->valid = true;
    entry->command_id = command->command_id;
    entry->opcode = command->opcode;
    entry->last_retry_count = command->retry_count;
    entry->result_code = ack->result_code;
    entry->fault_code = ack->fault_code;
    entry->device_mode = ack->device_mode;
    dedup->next_slot = (uint8_t)((dedup->next_slot + 1u) % MCU_COMMAND_REPLAY_WINDOW_SIZE);
    dedup->has_last_accepted = true;
    dedup->last_accepted_id = command->command_id;
}

/* 15 位序号回绕分类：delta = (candidate - last_accepted) 模 32768
 * （mcu-protocol-v1.md「关联、重试与回绕语义」）。
 * delta == 0 是重复；1..16383 是更新；16384..32767 是过期或歧义。 */
static uint16_t serial_delta(uint16_t candidate, uint16_t last_accepted)
{
    return (uint16_t)((candidate - last_accepted) & MCU_COMMAND_SERIAL_MASK);
}

/* ---- 普通命令入口 ---- */

/* 前置条件：dedup / machine / watchdog 结构合法，command 是完整解码
 * 且通过重编码校验的普通 COMMAND（STOP 帧编码后仲裁 ID 为 0x080，
 * 在此被拒绝，永不进入回放窗口）。
 * 处理顺序：会话闸门 → delta 分类 → 回放/冲突/过期 → 新命令派发。
 * 半区间内的新候选胜过数值相同的旧保留条目，用于区分真正的序号
 * 回绕与延迟的旧纪元回放；前向跨回绕时先清除回绕前记录。只有
 * VALID_NEW 活动才刷新软件看门狗（mcu-watchdog-v1.md「软件链路
 * 看门狗」）。 */
bool mcu_command_dedup_receive(mcu_command_dedup_t *dedup,
                               mcu_state_machine_t *machine,
                               mcu_watchdog_t *watchdog,
                               const mcu_wire_frame_t *command,
                               uint64_t now_us,
                               mcu_command_record_t *record)
{
    uint16_t arbitration_id;
    uint8_t encoded[MCU_WIRE_DLC];
    uint8_t encoded_length;
    mcu_command_replay_entry_t *entry;
    mcu_transition_result_t transition;
    uint16_t delta;
    bool wrapped;

    if (dedup == 0 || machine == 0 || watchdog == 0 || command == 0 || record == 0 ||
        !mcu_command_dedup_is_valid(dedup) || !mcu_sm_is_valid(machine) ||
        !mcu_watchdog_is_valid(watchdog)) {
        return false;
    }
    if (mcu_frame_encode(command,
                         &arbitration_id,
                         encoded,
                         sizeof(encoded),
                         &encoded_length) != MCU_CODEC_OK ||
        arbitration_id != MCU_CAN_ID_COMMAND || encoded_length != MCU_WIRE_DLC) {
        return false;
    }

    clear_record(record);
    if (!dedup->session_open) {
        record->outcome = MCU_COMMAND_OUTCOME_SESSION_CLOSED;
        return true;
    }

    delta = dedup->has_last_accepted
              ? serial_delta(command->command_id, dedup->last_accepted_id)
              : 1u;
    entry = find_entry(dedup, command->command_id);
    /* 半程范围内的新候选会胜过数值相同的旧保留 ID。在 Wire V1 没有纪元
     * 字段的情况下，这正用于区分真正的序号回绕与延迟的旧纪元回放。 */
    if (dedup->has_last_accepted && delta != 0u &&
        delta < MCU_COMMAND_SERIAL_HALF_RANGE) {
        entry = 0;
    }
    if (entry != 0) {
        if (entry->opcode != command->opcode) {
            reject_correlation(machine,
                               watchdog,
                               command,
                               now_us,
                               MCU_COMMAND_OUTCOME_REJECTED_CONFLICT,
                               MCU_WATCHDOG_ACTIVITY_DUPLICATE,
                               record);
            return true;
        }
        if (command->retry_count < entry->last_retry_count) {
            reject_correlation(machine,
                               watchdog,
                               command,
                               now_us,
                               MCU_COMMAND_OUTCOME_REJECTED_RETRY,
                               MCU_WATCHDOG_ACTIVITY_STALE,
                               record);
            return true;
        }

        record->outcome = MCU_COMMAND_OUTCOME_REPLAYED;
        record->ack_available = true;
        make_cached_ack(command, entry, &record->ack);
        record->watchdog_refreshed = mcu_watchdog_note_activity(
          watchdog,
          machine,
          command->retry_count == entry->last_retry_count
            ? MCU_WATCHDOG_ACTIVITY_DUPLICATE
            : MCU_WATCHDOG_ACTIVITY_RETRY,
          now_us);
        entry->last_retry_count = command->retry_count;
        return true;
    }

    if (dedup->has_last_accepted &&
        (delta == 0u || delta >= MCU_COMMAND_SERIAL_HALF_RANGE)) {
        reject_correlation(machine,
                           watchdog,
                           command,
                           now_us,
                           MCU_COMMAND_OUTCOME_REJECTED_STALE,
                           MCU_WATCHDOG_ACTIVITY_STALE,
                           record);
        return true;
    }

    wrapped = dedup->has_last_accepted && command->command_id < dedup->last_accepted_id;
    if (wrapped) {
        /* 从最大值到零的正向跳变开启新的序号纪元。Wire V1 不携带纪元位，
         * 因此任何回绕前的缓存条目都不得存留。 */
        clear_history(dedup);
    }

    dispatch_new_command(machine, command, &transition);
    record->outcome = transition.result_code == MCU_RESULT_ACCEPTED
                        ? MCU_COMMAND_OUTCOME_ACCEPTED_NEW
                        : MCU_COMMAND_OUTCOME_REJECTED_NEW;
    record->ack_available = true;
    record->ordinary_event_dispatched = true;
    if (transition.result_code == MCU_RESULT_ACCEPTED) {
        make_ack(command,
                 MCU_WIRE_RESULT_ACCEPTED,
                 MCU_WIRE_FAULT_NONE,
                 map_mode(transition.device_mode),
                 &record->ack);
    } else {
        make_ack(command,
                 MCU_WIRE_RESULT_REJECTED,
                 MCU_WIRE_FAULT_MALFORMED_FRAME,
                 MCU_WIRE_MODE_FAULTED,
                 &record->ack);
    }
    cache_new_result(dedup, command, &record->ack);
    record->watchdog_refreshed = mcu_watchdog_note_activity(
      watchdog, machine, MCU_WATCHDOG_ACTIVITY_VALID_NEW, now_us);
    return true;
}
