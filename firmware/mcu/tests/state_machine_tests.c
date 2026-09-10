/* 状态机测试套件。对应 Issue #53 的 C 安全状态机。
 *
 * 被测契约：docs/architecture/mcu-protocol-v1.md 冻结的 C 安全状态机
 * 语义——事件迁移表、设备模式映射、STOP 优先级与幂等、双闸门可信
 * 复位、故障锁存、失败即拒绝与确定性回放。
 *
 * 本文件与 core/state_machine.c 同源运行于 Host 与 QEMU 两个目标
 * （main_host.c / main_qemu.c 各自调用 mcu_state_machine_run_tests）。
 * 只做黑盒断言，绝不修改 core/ 的实现。
 */
#include "state_machine_tests.h"

#include "state_machine.h"

/* 每个（状态, 事件）组合的期望迁移结果，即权威迁移表的黄金副本。
 * 穷举测试遍历全部 MCU_STATE_COUNT × MCU_EVENT_COUNT 组合逐一比对。 */
typedef struct {
    mcu_state_t state;
    mcu_result_code_t result_code;
} expected_transition_t;

static const expected_transition_t transition_table[MCU_STATE_COUNT][MCU_EVENT_COUNT] = {
    /* IDLE 行：运动/保持/心跳/STOP/故障事件均可接受；COMPLETE 空转。 */
    [MCU_STATE_IDLE] = {
        [MCU_EVENT_BEGIN_MOVE] = {MCU_STATE_EXECUTING, MCU_RESULT_ACCEPTED},
        [MCU_EVENT_BEGIN_HOLD] = {MCU_STATE_EXECUTING, MCU_RESULT_ACCEPTED},
        [MCU_EVENT_COMPLETE] = {MCU_STATE_IDLE, MCU_RESULT_REJECTED},
        [MCU_EVENT_HEARTBEAT] = {MCU_STATE_IDLE, MCU_RESULT_ACCEPTED},
        [MCU_EVENT_STOP] = {MCU_STATE_SAFE_STOP, MCU_RESULT_ACCEPTED},
        [MCU_EVENT_WATCHDOG_EXPIRED] = {MCU_STATE_FAULT, MCU_RESULT_ACCEPTED},
        [MCU_EVENT_RAISE_FAULT] = {MCU_STATE_FAULT, MCU_RESULT_ACCEPTED},
        [MCU_EVENT_TRUSTED_RESET] = {MCU_STATE_IDLE, MCU_RESULT_ACCEPTED},
    },
    /* EXECUTING 行：新运动事件冲突入 FAULT；TRUSTED_RESET 被拒。 */
    [MCU_STATE_EXECUTING] = {
        [MCU_EVENT_BEGIN_MOVE] = {MCU_STATE_FAULT, MCU_RESULT_REJECTED},
        [MCU_EVENT_BEGIN_HOLD] = {MCU_STATE_FAULT, MCU_RESULT_REJECTED},
        [MCU_EVENT_COMPLETE] = {MCU_STATE_IDLE, MCU_RESULT_ACCEPTED},
        [MCU_EVENT_HEARTBEAT] = {MCU_STATE_EXECUTING, MCU_RESULT_ACCEPTED},
        [MCU_EVENT_STOP] = {MCU_STATE_SAFE_STOP, MCU_RESULT_ACCEPTED},
        [MCU_EVENT_WATCHDOG_EXPIRED] = {MCU_STATE_FAULT, MCU_RESULT_ACCEPTED},
        [MCU_EVENT_RAISE_FAULT] = {MCU_STATE_FAULT, MCU_RESULT_ACCEPTED},
        [MCU_EVENT_TRUSTED_RESET] = {MCU_STATE_FAULT, MCU_RESULT_REJECTED},
    },
    /* SAFE_STOP 行：运动事件冲突入 FAULT；STOP 幂等；重置回 IDLE。 */
    [MCU_STATE_SAFE_STOP] = {
        [MCU_EVENT_BEGIN_MOVE] = {MCU_STATE_FAULT, MCU_RESULT_REJECTED},
        [MCU_EVENT_BEGIN_HOLD] = {MCU_STATE_FAULT, MCU_RESULT_REJECTED},
        [MCU_EVENT_COMPLETE] = {MCU_STATE_SAFE_STOP, MCU_RESULT_REJECTED},
        [MCU_EVENT_HEARTBEAT] = {MCU_STATE_SAFE_STOP, MCU_RESULT_ACCEPTED},
        [MCU_EVENT_STOP] = {MCU_STATE_SAFE_STOP, MCU_RESULT_ACCEPTED},
        [MCU_EVENT_WATCHDOG_EXPIRED] = {MCU_STATE_FAULT, MCU_RESULT_ACCEPTED},
        [MCU_EVENT_RAISE_FAULT] = {MCU_STATE_FAULT, MCU_RESULT_ACCEPTED},
        [MCU_EVENT_TRUSTED_RESET] = {MCU_STATE_IDLE, MCU_RESULT_ACCEPTED},
    },
    /* FAULT 行：命令、STOP、心跳全部拒绝并锁存 FAULT；仅可信重置回 IDLE。 */
    [MCU_STATE_FAULT] = {
        [MCU_EVENT_BEGIN_MOVE] = {MCU_STATE_FAULT, MCU_RESULT_REJECTED},
        [MCU_EVENT_BEGIN_HOLD] = {MCU_STATE_FAULT, MCU_RESULT_REJECTED},
        [MCU_EVENT_COMPLETE] = {MCU_STATE_FAULT, MCU_RESULT_REJECTED},
        [MCU_EVENT_HEARTBEAT] = {MCU_STATE_FAULT, MCU_RESULT_REJECTED},
        [MCU_EVENT_STOP] = {MCU_STATE_FAULT, MCU_RESULT_REJECTED},
        [MCU_EVENT_WATCHDOG_EXPIRED] = {MCU_STATE_FAULT, MCU_RESULT_ACCEPTED},
        [MCU_EVENT_RAISE_FAULT] = {MCU_STATE_FAULT, MCU_RESULT_ACCEPTED},
        [MCU_EVENT_TRUSTED_RESET] = {MCU_STATE_IDLE, MCU_RESULT_ACCEPTED},
    },
};

/* 单条断言：递增断言计数；失败时递增失败计数并记录首个失败序号。 */
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

/* 把状态机播种到指定状态，并同步设置一致的状态/模式/故障码字段。 */
static void seed_machine(mcu_state_machine_t *machine, mcu_state_t state)
{
    machine->state = state;
    machine->fault_code = MCU_FAULT_NONE;

    switch (state) {
    case MCU_STATE_IDLE:
        machine->device_mode = MCU_DEVICE_MODE_IDLE;
        break;
    case MCU_STATE_EXECUTING:
        machine->device_mode = MCU_DEVICE_MODE_MOVING;
        break;
    case MCU_STATE_SAFE_STOP:
        machine->device_mode = MCU_DEVICE_MODE_STOPPED;
        break;
    case MCU_STATE_FAULT:
        machine->device_mode = MCU_DEVICE_MODE_FAULTED;
        machine->fault_code = MCU_FAULT_WATCHDOG_EXPIRED;
        break;
    case MCU_STATE_COUNT:
    default:
        machine->device_mode = MCU_DEVICE_MODE_FAULTED;
        machine->fault_code = MCU_FAULT_MALFORMED_FRAME;
        break;
    }
}

/* 构造一个「最强合法」事件：默认授权与原因清除都置真，
 * 使 TRUSTED_RESET 之类需要闸门的事件在默认配置下即可通过。 */
static void init_event(mcu_event_t *event, mcu_event_kind_t kind)
{
    event->kind = kind;
    event->fault_code = MCU_FAULT_LINK_LOST;
    event->reset_authorized = true;
    event->cause_cleared = true;
}

/* 便捷宏：构造一个指定类别的事件并派发，结果写入 result。 */
#define DISPATCH_KIND(machine, kind, result) \
    do { \
        mcu_event_t dispatch_event; \
        init_event(&dispatch_event, (kind)); \
        mcu_sm_dispatch((machine), &dispatch_event, (result)); \
    } while (0)

/* 断言迁移结果与机器状态之间的一致性不变量：
 * 结果镜像机器状态；执行活动仅在 EXECUTING；安全输出强制仅在
 * 非 EXECUTING；两者互斥。 */
static void check_result_invariants(mcu_test_report_t *report,
                                    const mcu_state_machine_t *machine,
                                    const mcu_transition_result_t *result)
{
    check(report, mcu_sm_is_valid(machine));
    check(report, result->state == machine->state);
    check(report, result->device_mode == machine->device_mode);
    check(report, result->fault_code == machine->fault_code);
    check(report, result->execution_active == (machine->state == MCU_STATE_EXECUTING));
    check(report, result->force_safe_outputs == (machine->state != MCU_STATE_EXECUTING));
    check(report, result->execution_active != result->force_safe_outputs);
}

/* 穷举迁移表：对全部（状态 × 事件）组合断言 previous_state、
 * 新状态与 result_code 均与黄金表一致。 */
static void test_exhaustive_transition_table(mcu_test_report_t *report)
{
    mcu_state_t state;
    mcu_event_kind_t kind;

    for (state = MCU_STATE_IDLE; state < MCU_STATE_COUNT; state++) {
        for (kind = MCU_EVENT_BEGIN_MOVE; kind < MCU_EVENT_COUNT; kind++) {
            mcu_state_machine_t machine;
            mcu_event_t event;
            mcu_transition_result_t result;
            const expected_transition_t *expected = &transition_table[state][kind];

            init_event(&event, kind);
            seed_machine(&machine, state);
            mcu_sm_dispatch(&machine, &event, &result);

            check(report, result.previous_state == state);
            check(report, result.state == expected->state);
            check(report, result.result_code == expected->result_code);
            check_result_invariants(report, &machine, &result);
        }
    }
}

/* 协议设备模式映射：move→moving、complete→idle、hold→holding、
 * stop→stopped（mcu-protocol-v1.md 的设备模式注册表）。 */
static void test_protocol_mode_mapping(mcu_test_report_t *report)
{
    mcu_state_machine_t machine;
    mcu_transition_result_t result;

    mcu_sm_init(&machine);
    check(report, machine.state == MCU_STATE_IDLE);
    check(report, machine.device_mode == MCU_DEVICE_MODE_IDLE);
    check(report, machine.fault_code == MCU_FAULT_NONE);

    DISPATCH_KIND(&machine, MCU_EVENT_BEGIN_MOVE, &result);
    check(report, result.device_mode == MCU_DEVICE_MODE_MOVING);
    DISPATCH_KIND(&machine, MCU_EVENT_COMPLETE, &result);
    check(report, result.device_mode == MCU_DEVICE_MODE_IDLE);
    DISPATCH_KIND(&machine, MCU_EVENT_BEGIN_HOLD, &result);
    check(report, result.device_mode == MCU_DEVICE_MODE_HOLDING);
    DISPATCH_KIND(&machine, MCU_EVENT_STOP, &result);
    check(report, result.device_mode == MCU_DEVICE_MODE_STOPPED);
}

/* STOP 幂等且最高优先级：执行中重复 STOP 仍被接受、停留
 * SAFE_STOP、响应无故障且安全输出强制。 */
static void test_stop_is_idempotent_and_highest_priority(mcu_test_report_t *report)
{
    mcu_state_machine_t machine;
    mcu_transition_result_t first;
    mcu_transition_result_t second;

    mcu_sm_init(&machine);
    DISPATCH_KIND(&machine, MCU_EVENT_BEGIN_MOVE, &first);
    DISPATCH_KIND(&machine, MCU_EVENT_STOP, &first);
    DISPATCH_KIND(&machine, MCU_EVENT_STOP, &second);

    check(report, first.result_code == MCU_RESULT_ACCEPTED);
    check(report, first.state == MCU_STATE_SAFE_STOP);
    check(report, second.result_code == MCU_RESULT_ACCEPTED);
    check(report, second.previous_state == MCU_STATE_SAFE_STOP);
    check(report, second.state == MCU_STATE_SAFE_STOP);
    check(report, second.response_fault_code == MCU_FAULT_NONE);
    check(report, second.force_safe_outputs);
}

/* 故障态收到 STOP：拒绝（reason=STOP_REJECTED）、锁存原故障
 * （watchdog_expired）、响应故障码为 stop_rejected。 */
static void test_faulted_stop_preserves_original_cause(mcu_test_report_t *report)
{
    mcu_state_machine_t machine;
    mcu_transition_result_t result;

    seed_machine(&machine, MCU_STATE_FAULT);
    DISPATCH_KIND(&machine, MCU_EVENT_STOP, &result);

    check(report, result.result_code == MCU_RESULT_REJECTED);
    check(report, result.reason == MCU_REASON_STOP_REJECTED);
    check(report, result.state == MCU_STATE_FAULT);
    check(report, result.fault_code == MCU_FAULT_WATCHDOG_EXPIRED);
    check(report, result.response_fault_code == MCU_FAULT_STOP_REJECTED);
    check(report, result.force_safe_outputs);
}

/* 可信复位双闸门：授权与原因清除任一缺失都被拒绝
 * （RESET_NOT_AUTHORIZED / RESET_CAUSE_ACTIVE），两者齐备才进入
 * idle；FAULT 态未授权复位保持锁存。 */
static void test_reset_requires_both_trusted_gates(mcu_test_report_t *report)
{
    mcu_state_machine_t machine;
    mcu_event_t reset;
    mcu_transition_result_t result;

    init_event(&reset, MCU_EVENT_TRUSTED_RESET);
    seed_machine(&machine, MCU_STATE_SAFE_STOP);
    reset.reset_authorized = false;
    mcu_sm_dispatch(&machine, &reset, &result);
    check(report, result.result_code == MCU_RESULT_REJECTED);
    check(report, result.reason == MCU_REASON_RESET_NOT_AUTHORIZED);
    check(report, machine.state == MCU_STATE_SAFE_STOP);

    reset.reset_authorized = true;
    reset.cause_cleared = false;
    mcu_sm_dispatch(&machine, &reset, &result);
    check(report, result.result_code == MCU_RESULT_REJECTED);
    check(report, result.reason == MCU_REASON_RESET_CAUSE_ACTIVE);
    check(report, machine.state == MCU_STATE_SAFE_STOP);

    reset.cause_cleared = true;
    mcu_sm_dispatch(&machine, &reset, &result);
    check(report, result.result_code == MCU_RESULT_ACCEPTED);
    check(report, result.state == MCU_STATE_IDLE);
    check(report, !result.execution_active);

    seed_machine(&machine, MCU_STATE_FAULT);
    reset.reset_authorized = false;
    mcu_sm_dispatch(&machine, &reset, &result);
    check(report, result.state == MCU_STATE_FAULT);
    check(report, result.fault_code == MCU_FAULT_WATCHDOG_EXPIRED);
}

/* 看门狗与故障锁存：WATCHDOG_EXPIRED 进入 FAULT 后，后到的
 * RAISE_FAULT（link_lost）与 BEGIN_MOVE 都不能覆盖首个故障原因，
 * 执行保持禁用、安全输出强制。 */
static void test_watchdog_and_faults_are_latched(mcu_test_report_t *report)
{
    mcu_state_machine_t machine;
    mcu_transition_result_t result;
    mcu_event_t fault;

    init_event(&fault, MCU_EVENT_RAISE_FAULT);
    mcu_sm_init(&machine);
    DISPATCH_KIND(&machine, MCU_EVENT_BEGIN_MOVE, &result);
    DISPATCH_KIND(&machine, MCU_EVENT_WATCHDOG_EXPIRED, &result);
    check(report, result.state == MCU_STATE_FAULT);
    check(report, result.fault_code == MCU_FAULT_WATCHDOG_EXPIRED);
    check(report, !result.execution_active);
    check(report, result.force_safe_outputs);

    fault.fault_code = MCU_FAULT_LINK_LOST;
    mcu_sm_dispatch(&machine, &fault, &result);
    check(report, result.state == MCU_STATE_FAULT);
    check(report, result.fault_code == MCU_FAULT_WATCHDOG_EXPIRED);

    DISPATCH_KIND(&machine, MCU_EVENT_BEGIN_MOVE, &result);
    check(report, result.result_code == MCU_RESULT_REJECTED);
    check(report, result.state == MCU_STATE_FAULT);
    check(report, result.fault_code == MCU_FAULT_WATCHDOG_EXPIRED);
}

/* 无效输入失败即拒绝：非法故障码、越界事件类别、状态/模式不一致、
 * 空指针 machine/event/result 一律拒绝并进入锁存 FAULT
 * （malformed_frame），且机器仍满足 mcu_sm_is_valid。 */
static void test_invalid_inputs_fail_closed(mcu_test_report_t *report)
{
    mcu_state_machine_t machine;
    mcu_event_t event;
    mcu_transition_result_t result;

    init_event(&event, MCU_EVENT_RAISE_FAULT);
    mcu_sm_init(&machine);
    event.fault_code = MCU_FAULT_NONE;
    mcu_sm_dispatch(&machine, &event, &result);
    check(report, result.result_code == MCU_RESULT_REJECTED);
    check(report, result.reason == MCU_REASON_INVALID_ARGUMENT);
    check(report, result.state == MCU_STATE_FAULT);
    check(report, result.fault_code == MCU_FAULT_MALFORMED_FRAME);

    mcu_sm_init(&machine);
    init_event(&event, MCU_EVENT_COUNT);
    mcu_sm_dispatch(&machine, &event, &result);
    check(report, result.result_code == MCU_RESULT_REJECTED);
    check(report, result.reason == MCU_REASON_INVALID_EVENT);
    check(report, result.state == MCU_STATE_FAULT);
    check(report, result.force_safe_outputs);

    mcu_sm_init(&machine);
    machine.device_mode = MCU_DEVICE_MODE_MOVING;
    DISPATCH_KIND(&machine, MCU_EVENT_HEARTBEAT, &result);
    check(report, result.result_code == MCU_RESULT_REJECTED);
    check(report, result.reason == MCU_REASON_INVALID_STATE);
    check(report, result.state == MCU_STATE_FAULT);
    check(report, result.fault_code == MCU_FAULT_MALFORMED_FRAME);

    mcu_sm_init(&machine);
    mcu_sm_dispatch(&machine, 0, &result);
    check(report, result.result_code == MCU_RESULT_REJECTED);
    check(report, result.reason == MCU_REASON_INVALID_ARGUMENT);
    check(report, result.state == MCU_STATE_FAULT);
    check(report, result.fault_code == MCU_FAULT_MALFORMED_FRAME);

    mcu_sm_init(&machine);
    init_event(&event, MCU_EVENT_BEGIN_MOVE);
    mcu_sm_dispatch(&machine, &event, 0);
    check(report, machine.state == MCU_STATE_FAULT);
    check(report, machine.device_mode == MCU_DEVICE_MODE_FAULTED);
    check(report, machine.fault_code == MCU_FAULT_MALFORMED_FRAME);
    check(report, mcu_sm_is_valid(&machine));

    mcu_sm_init(&machine);
    DISPATCH_KIND(&machine, MCU_EVENT_BEGIN_MOVE, &result);
    init_event(&event, MCU_EVENT_STOP);
    mcu_sm_dispatch(&machine, &event, 0);
    check(report, machine.state == MCU_STATE_SAFE_STOP);
    check(report, machine.device_mode == MCU_DEVICE_MODE_STOPPED);
    check(report, machine.fault_code == MCU_FAULT_NONE);
    check(report, mcu_sm_is_valid(&machine));

    DISPATCH_KIND(0, MCU_EVENT_STOP, &result);
    check(report, result.result_code == MCU_RESULT_REJECTED);
    check(report, result.reason == MCU_REASON_INVALID_ARGUMENT);
    check(report, result.state == MCU_STATE_FAULT);
    check(report, result.force_safe_outputs);
}

/* 标称安全向量：move→complete→move→stop→reset→watchdog_expired
 * 的端到端状态迁移（执行 → 空闲 → 执行 → 安全停止 → 空闲 → 故障）。 */
static void test_nominal_safety_vector(mcu_test_report_t *report)
{
    mcu_state_machine_t machine;
    mcu_event_t reset;
    mcu_transition_result_t result;

    init_event(&reset, MCU_EVENT_TRUSTED_RESET);
    mcu_sm_init(&machine);
    DISPATCH_KIND(&machine, MCU_EVENT_BEGIN_MOVE, &result);
    check(report, result.state == MCU_STATE_EXECUTING);
    DISPATCH_KIND(&machine, MCU_EVENT_COMPLETE, &result);
    check(report, result.state == MCU_STATE_IDLE);
    DISPATCH_KIND(&machine, MCU_EVENT_BEGIN_MOVE, &result);
    check(report, result.state == MCU_STATE_EXECUTING);
    DISPATCH_KIND(&machine, MCU_EVENT_STOP, &result);
    check(report, result.state == MCU_STATE_SAFE_STOP);
    mcu_sm_dispatch(&machine, &reset, &result);
    check(report, result.state == MCU_STATE_IDLE);
    DISPATCH_KIND(&machine, MCU_EVENT_WATCHDOG_EXPIRED, &result);
    check(report, result.state == MCU_STATE_FAULT);
}

/* 确定性回放：两台独立机器喂同一事件序列，逐步断言结果字段
 * 完全一致——状态机必须无隐藏状态、无随机性。 */
static void test_deterministic_replay(mcu_test_report_t *report)
{
    static const mcu_event_kind_t sequence[] = {
        MCU_EVENT_BEGIN_MOVE,
        MCU_EVENT_HEARTBEAT,
        MCU_EVENT_STOP,
        MCU_EVENT_STOP,
        MCU_EVENT_TRUSTED_RESET,
        MCU_EVENT_BEGIN_HOLD,
        MCU_EVENT_COMPLETE,
    };
    mcu_state_machine_t first;
    mcu_state_machine_t second;
    unsigned i;

    mcu_sm_init(&first);
    mcu_sm_init(&second);
    for (i = 0; i < sizeof(sequence) / sizeof(sequence[0]); i++) {
        mcu_transition_result_t first_result;
        mcu_transition_result_t second_result;

        DISPATCH_KIND(&first, sequence[i], &first_result);
        DISPATCH_KIND(&second, sequence[i], &second_result);

        check(report, first_result.result_code == second_result.result_code);
        check(report, first_result.reason == second_result.reason);
        check(report, first_result.state == second_result.state);
        check(report, first_result.device_mode == second_result.device_mode);
        check(report, first_result.fault_code == second_result.fault_code);
        check(report, first_result.response_fault_code == second_result.response_fault_code);
    }
}

/* 套件入口：清零报告后按顺序运行全部测试。Host 侧断言总数为
 * 436，QEMU 侧运行同一份源码（共享套件）。 */
void mcu_state_machine_run_tests(mcu_test_report_t *report)
{
    if (report == 0) {
        return;
    }

    report->assertions = 0u;
    report->failures = 0u;
    report->first_failure = 0u;

    test_exhaustive_transition_table(report);
    test_protocol_mode_mapping(report);
    test_stop_is_idempotent_and_highest_priority(report);
    test_faulted_stop_preserves_original_cause(report);
    test_reset_requires_both_trusted_gates(report);
    test_watchdog_and_faults_are_latched(report);
    test_invalid_inputs_fail_closed(report);
    test_nominal_safety_vector(report);
    test_deterministic_replay(report);
}
