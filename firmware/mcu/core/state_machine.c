/* state_machine.c —— 平台无关的 C 安全状态机实现。
 *
 * 职责：维护 IDLE / EXECUTING / SAFE_STOP / FAULT 四个安全状态及其协议
 * 设备模式，分发 mcu_event_t 事件并产出 mcu_transition_result_t；任何
 * 非法输入、非法状态或非法转移都失败即拒绝并锁存 malformed_frame。
 *
 * 契约文档：docs/architecture/mcu-protocol-v1.md（「结果语义」「遥测语义」
 *   「重置授权」「失败即拒绝处理」）；看门狗期限经由
 *   MCU_EVENT_WATCHDOG_EXPIRED 注入（docs/architecture/mcu-watchdog-v1.md
 *   「软件链路看门狗」）。
 *
 * 编译目标：host、qemu、ch32v307 三目标共源；core/ 不含厂商或平台头文件
 *   （firmware/mcu/README.md「唯一规则」）。
 */

#include "state_machine.h"

/* 故障码枚举范围校验（NONE..COUNT 前）。 */
static bool fault_code_is_valid(mcu_fault_code_t fault_code)
{
    return fault_code >= MCU_FAULT_NONE && fault_code < MCU_FAULT_COUNT;
}

/* 活动（锁存的）故障码必须是非 NONE 的合法值。 */
static bool active_fault_is_valid(mcu_fault_code_t fault_code)
{
    return fault_code > MCU_FAULT_NONE && fault_code < MCU_FAULT_COUNT;
}

/* 状态不变量：每个状态的设备模式与故障码组合固定——
 * IDLE/idle/none；EXECUTING/moving 或 holding/none；
 * SAFE_STOP/stopped/none；FAULT/faulted/活动故障码。
 * 任何其他组合都视为损坏状态，失败即拒绝。 */
bool mcu_sm_is_valid(const mcu_state_machine_t *machine)
{
    if (machine == 0 || !fault_code_is_valid(machine->fault_code)) {
        return false;
    }

    switch (machine->state) {
    case MCU_STATE_IDLE:
        return machine->device_mode == MCU_DEVICE_MODE_IDLE && machine->fault_code == MCU_FAULT_NONE;
    case MCU_STATE_EXECUTING:
        return (machine->device_mode == MCU_DEVICE_MODE_MOVING ||
                machine->device_mode == MCU_DEVICE_MODE_HOLDING) &&
               machine->fault_code == MCU_FAULT_NONE;
    case MCU_STATE_SAFE_STOP:
        return machine->device_mode == MCU_DEVICE_MODE_STOPPED && machine->fault_code == MCU_FAULT_NONE;
    case MCU_STATE_FAULT:
        return machine->device_mode == MCU_DEVICE_MODE_FAULTED && active_fault_is_valid(machine->fault_code);
    case MCU_STATE_COUNT:
    default:
        return false;
    }
}

/* 复位为初始安全状态 IDLE/idle/none；空指针为无效调用方输入，直接返回。 */
void mcu_sm_init(mcu_state_machine_t *machine)
{
    if (machine == 0) {
        return;
    }

    machine->state = MCU_STATE_IDLE;
    machine->device_mode = MCU_DEVICE_MODE_IDLE;
    machine->fault_code = MCU_FAULT_NONE;
}

/* ---- 状态进入辅助函数 ---- */

/* 进入 IDLE。三个字段（state、device_mode、fault_code）总是一起写，
 * 保证任何观察点都不会看到撕裂的中间组合。 */
static void enter_idle(mcu_state_machine_t *machine)
{
    machine->state = MCU_STATE_IDLE;
    machine->device_mode = MCU_DEVICE_MODE_IDLE;
    machine->fault_code = MCU_FAULT_NONE;
}

/* 进入 EXECUTING；mode 由调用方给定（moving 或 holding）。 */
static void enter_executing(mcu_state_machine_t *machine, mcu_device_mode_t mode)
{
    machine->state = MCU_STATE_EXECUTING;
    machine->device_mode = mode;
    machine->fault_code = MCU_FAULT_NONE;
}

/* 进入 SAFE_STOP/stopped/none。 */
static void enter_safe_stop(mcu_state_machine_t *machine)
{
    machine->state = MCU_STATE_SAFE_STOP;
    machine->device_mode = MCU_DEVICE_MODE_STOPPED;
    machine->fault_code = MCU_FAULT_NONE;
}

/* 进入 FAULT：state 与 device_mode 无条件置位，fault_code 保留
 * 第一个活动原因（详见函数体注释）。 */
static void enter_fault(mcu_state_machine_t *machine, mcu_fault_code_t fault_code)
{
    /* 保留第一个活动原因。后续流量不得覆盖
     * 最初锁存故障的证据。 */
    if (machine->state != MCU_STATE_FAULT || !active_fault_is_valid(machine->fault_code)) {
        machine->fault_code = fault_code;
    }
    machine->state = MCU_STATE_FAULT;
    machine->device_mode = MCU_DEVICE_MODE_FAULTED;
}

/* ---- 迁移结果构造与失败即拒绝路径 ---- */

/* 从当前机器状态填充迁移结果。execution_active 仅当状态为
 * EXECUTING；force_safe_outputs 为「非 EXECUTING 即强制安全输出」。 */
static void make_result(mcu_transition_result_t *result,
                        const mcu_state_machine_t *machine,
                        mcu_state_t previous_state,
                        mcu_result_code_t result_code,
                        mcu_result_reason_t reason,
                        mcu_fault_code_t response_fault_code)
{
    result->result_code = result_code;
    result->reason = reason;
    result->previous_state = previous_state;
    result->state = machine->state;
    result->device_mode = machine->device_mode;
    result->fault_code = machine->fault_code;
    result->response_fault_code = response_fault_code;
    result->execution_active = machine->state == MCU_STATE_EXECUTING;
    result->force_safe_outputs = machine->state != MCU_STATE_EXECUTING;
}

/* machine == 0 时构造合成的 FAULT 结果：拒绝 + invalid_argument +
 * malformed_frame，绝不让空机器对象伪装成安全状态。 */
static void null_machine_result(mcu_transition_result_t *result)
{
    mcu_state_machine_t safe;

    safe.state = MCU_STATE_FAULT;
    safe.device_mode = MCU_DEVICE_MODE_FAULTED;
    safe.fault_code = MCU_FAULT_MALFORMED_FRAME;
    make_result(result,
                &safe,
                MCU_STATE_FAULT,
                MCU_RESULT_REJECTED,
                MCU_REASON_INVALID_ARGUMENT,
                MCU_FAULT_MALFORMED_FRAME);
}

/* 非 IDLE 状态收到 BEGIN_MOVE / BEGIN_HOLD 时的失败即拒绝路径：
 * 进入 FAULT 并锁存 malformed_frame（INVALID_TRANSITION）。 */
static void reject_start(mcu_state_machine_t *machine,
                          mcu_state_t previous_state,
                          mcu_transition_result_t *result)
{
    enter_fault(machine, MCU_FAULT_MALFORMED_FRAME);
    make_result(result,
                machine,
                previous_state,
                MCU_RESULT_REJECTED,
                MCU_REASON_INVALID_TRANSITION,
                MCU_FAULT_MALFORMED_FRAME);
}

/* 可信重置的三重闸门（mcu-protocol-v1.md「重置授权」）：
 * 必须 reset_authorized、cause_cleared 且不处于 EXECUTING；
 * 接受的重置只进入 IDLE，绝不进入执行模式。协议 v1.0 帧
 * 永不携带这两个闸门位——它们来自可信控制路径。 */
static void handle_reset(mcu_state_machine_t *machine,
                         mcu_state_t previous_state,
                         const mcu_event_t *event,
                         mcu_transition_result_t *result)
{
    if (!event->reset_authorized) {
        make_result(result,
                    machine,
                    previous_state,
                    MCU_RESULT_REJECTED,
                    MCU_REASON_RESET_NOT_AUTHORIZED,
                    MCU_FAULT_NONE);
        return;
    }
    if (!event->cause_cleared) {
        make_result(result,
                    machine,
                    previous_state,
                    MCU_RESULT_REJECTED,
                    MCU_REASON_RESET_CAUSE_ACTIVE,
                    MCU_FAULT_NONE);
        return;
    }
    if (machine->state == MCU_STATE_EXECUTING) {
        enter_fault(machine, MCU_FAULT_MALFORMED_FRAME);
        make_result(result,
                    machine,
                    previous_state,
                    MCU_RESULT_REJECTED,
                    MCU_REASON_INVALID_TRANSITION,
                    MCU_FAULT_MALFORMED_FRAME);
        return;
    }

    enter_idle(machine);
    make_result(result, machine, previous_state, MCU_RESULT_ACCEPTED, MCU_REASON_NONE, MCU_FAULT_NONE);
}

/* ---- 事件分发入口 ---- */

/* 分发校验顺序：结果缓冲区缺失（STOP 仍走本地缓冲区派发，其余事件
 * 按无效输入拒绝）→ machine / event 空指针 → 状态不变量。
 * 转移规则：BEGIN_MOVE / BEGIN_HOLD 仅从 IDLE 接受；COMPLETE 仅从
 * EXECUTING 接受；HEARTBEAT 除 FAULT 外接受且不改变模式；STOP 在
 * FAULT 中拒绝（stop_rejected）否则进入 SAFE_STOP；WATCHDOG_EXPIRED
 * 进入锁存 FAULT；RAISE_FAULT 不接受 STOP_REJECTED（该故障只由
 * STOP 路径产生）；未知事件种类进入 FAULT 并锁存 malformed_frame。 */
void mcu_sm_dispatch(mcu_state_machine_t *machine,
                     const mcu_event_t *event,
                     mcu_transition_result_t *result)
{
    mcu_transition_result_t ignored_result;
    bool result_missing;
    mcu_state_t previous_state;

    result_missing = result == 0;
    if (result_missing) {
        /* 调用方不能通过省略输出缓冲区来压制安全处理。
         * 本地缓冲区为 STOP 保留正常派发路径，
         * 而非 STOP 事件将在下方作为无效输入被拒绝。 */
        result = &ignored_result;
    }
    if (machine == 0) {
        null_machine_result(result);
        return;
    }

    previous_state = machine->state;
    if (!mcu_sm_is_valid(machine)) {
        machine->state = MCU_STATE_FAULT;
        machine->device_mode = MCU_DEVICE_MODE_FAULTED;
        machine->fault_code = MCU_FAULT_MALFORMED_FRAME;
        make_result(result,
                    machine,
                    previous_state,
                    MCU_RESULT_REJECTED,
                    MCU_REASON_INVALID_STATE,
                    MCU_FAULT_MALFORMED_FRAME);
        return;
    }
    if (event == 0) {
        enter_fault(machine, MCU_FAULT_MALFORMED_FRAME);
        make_result(result,
                    machine,
                    previous_state,
                    MCU_RESULT_REJECTED,
                    MCU_REASON_INVALID_ARGUMENT,
                    MCU_FAULT_MALFORMED_FRAME);
        return;
    }
    if (result_missing && event->kind != MCU_EVENT_STOP) {
        enter_fault(machine, MCU_FAULT_MALFORMED_FRAME);
        make_result(result,
                    machine,
                    previous_state,
                    MCU_RESULT_REJECTED,
                    MCU_REASON_INVALID_ARGUMENT,
                    MCU_FAULT_MALFORMED_FRAME);
        return;
    }

    switch (event->kind) {
    case MCU_EVENT_BEGIN_MOVE:
        if (machine->state != MCU_STATE_IDLE) {
            reject_start(machine, previous_state, result);
            return;
        }
        enter_executing(machine, MCU_DEVICE_MODE_MOVING);
        make_result(result, machine, previous_state, MCU_RESULT_ACCEPTED, MCU_REASON_NONE, MCU_FAULT_NONE);
        return;

    case MCU_EVENT_BEGIN_HOLD:
        if (machine->state != MCU_STATE_IDLE) {
            reject_start(machine, previous_state, result);
            return;
        }
        enter_executing(machine, MCU_DEVICE_MODE_HOLDING);
        make_result(result, machine, previous_state, MCU_RESULT_ACCEPTED, MCU_REASON_NONE, MCU_FAULT_NONE);
        return;

    case MCU_EVENT_COMPLETE:
        if (machine->state != MCU_STATE_EXECUTING) {
            make_result(result,
                        machine,
                        previous_state,
                        MCU_RESULT_REJECTED,
                        MCU_REASON_INVALID_TRANSITION,
                        MCU_FAULT_NONE);
            return;
        }
        enter_idle(machine);
        make_result(result, machine, previous_state, MCU_RESULT_ACCEPTED, MCU_REASON_NONE, MCU_FAULT_NONE);
        return;

    case MCU_EVENT_HEARTBEAT:
        if (machine->state == MCU_STATE_FAULT) {
            make_result(result,
                        machine,
                        previous_state,
                        MCU_RESULT_REJECTED,
                        MCU_REASON_INVALID_TRANSITION,
                        machine->fault_code);
            return;
        }
        make_result(result, machine, previous_state, MCU_RESULT_ACCEPTED, MCU_REASON_NONE, MCU_FAULT_NONE);
        return;

    case MCU_EVENT_STOP:
        if (machine->state == MCU_STATE_FAULT) {
            make_result(result,
                        machine,
                        previous_state,
                        MCU_RESULT_REJECTED,
                        MCU_REASON_STOP_REJECTED,
                        MCU_FAULT_STOP_REJECTED);
            return;
        }
        enter_safe_stop(machine);
        make_result(result, machine, previous_state, MCU_RESULT_ACCEPTED, MCU_REASON_NONE, MCU_FAULT_NONE);
        return;

    case MCU_EVENT_WATCHDOG_EXPIRED:
        enter_fault(machine, MCU_FAULT_WATCHDOG_EXPIRED);
        make_result(result,
                    machine,
                    previous_state,
                    MCU_RESULT_ACCEPTED,
                    MCU_REASON_NONE,
                    MCU_FAULT_WATCHDOG_EXPIRED);
        return;

    case MCU_EVENT_RAISE_FAULT:
        if (!active_fault_is_valid(event->fault_code) || event->fault_code == MCU_FAULT_STOP_REJECTED) {
            enter_fault(machine, MCU_FAULT_MALFORMED_FRAME);
            make_result(result,
                        machine,
                        previous_state,
                        MCU_RESULT_REJECTED,
                        MCU_REASON_INVALID_ARGUMENT,
                        MCU_FAULT_MALFORMED_FRAME);
            return;
        }
        enter_fault(machine, event->fault_code);
        make_result(result, machine, previous_state, MCU_RESULT_ACCEPTED, MCU_REASON_NONE, event->fault_code);
        return;

    case MCU_EVENT_TRUSTED_RESET:
        handle_reset(machine, previous_state, event, result);
        return;

    case MCU_EVENT_COUNT:
    default:
        enter_fault(machine, MCU_FAULT_MALFORMED_FRAME);
        make_result(result,
                    machine,
                    previous_state,
                    MCU_RESULT_REJECTED,
                    MCU_REASON_INVALID_EVENT,
                    MCU_FAULT_MALFORMED_FRAME);
        return;
    }
}
