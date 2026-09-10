#ifndef MCU_STATE_MACHINE_H
#define MCU_STATE_MACHINE_H

#include <stdbool.h>
#include <stdint.h>

/* 平台无关的安全状态。协议设备模式单独暴露，
 * 因为 EXECUTING 既可以表示运动中也可以表示保持中。 */
typedef enum {
    MCU_STATE_IDLE = 0,
    MCU_STATE_EXECUTING,
    MCU_STATE_SAFE_STOP,
    MCU_STATE_FAULT,
    MCU_STATE_COUNT
} mcu_state_t;

typedef enum {
    MCU_DEVICE_MODE_IDLE = 0,
    MCU_DEVICE_MODE_MOVING,
    MCU_DEVICE_MODE_HOLDING,
    MCU_DEVICE_MODE_STOPPED,
    MCU_DEVICE_MODE_FAULTED,
    MCU_DEVICE_MODE_COUNT
} mcu_device_mode_t;

/* 来自协议 v1.0 的 MCU 侧故障含义。ACK_TIMEOUT 与 STOP_TIMEOUT 属于主机侧
 * 诊断，因此不应出现在本 core 中。 */
typedef enum {
    MCU_FAULT_NONE = 0,
    MCU_FAULT_STOP_REJECTED,
    MCU_FAULT_LINK_LOST,
    MCU_FAULT_DUPLICATE_FRAME,
    MCU_FAULT_WATCHDOG_EXPIRED,
    MCU_FAULT_MALFORMED_FRAME,
    MCU_FAULT_COUNT
} mcu_fault_code_t;

typedef enum {
    MCU_EVENT_BEGIN_MOVE = 0,
    MCU_EVENT_BEGIN_HOLD,
    MCU_EVENT_COMPLETE,
    MCU_EVENT_HEARTBEAT,
    MCU_EVENT_STOP,
    MCU_EVENT_WATCHDOG_EXPIRED,
    MCU_EVENT_RAISE_FAULT,
    MCU_EVENT_TRUSTED_RESET,
    MCU_EVENT_COUNT
} mcu_event_kind_t;

typedef struct {
    mcu_event_kind_t kind;

    /* 仅由 MCU_EVENT_RAISE_FAULT 使用。 */
    mcu_fault_code_t fault_code;

    /* 仅由 MCU_EVENT_TRUSTED_RESET 使用。这些闸门来自可信控制路径，
     * 绝不来自协议 v1.0 帧。 */
    bool reset_authorized;
    bool cause_cleared;
} mcu_event_t;

/* 取值有意与协议 v1.0 的 result_code 保持一致。 */
typedef enum {
    MCU_RESULT_ACCEPTED = 0,
    MCU_RESULT_REJECTED = 1
} mcu_result_code_t;

typedef enum {
    MCU_REASON_NONE = 0,
    MCU_REASON_INVALID_EVENT,
    MCU_REASON_INVALID_ARGUMENT,
    MCU_REASON_INVALID_STATE,
    MCU_REASON_INVALID_TRANSITION,
    MCU_REASON_RESET_NOT_AUTHORIZED,
    MCU_REASON_RESET_CAUSE_ACTIVE,
    MCU_REASON_STOP_REJECTED
} mcu_result_reason_t;

typedef struct {
    mcu_state_t state;
    mcu_device_mode_t device_mode;
    mcu_fault_code_t fault_code;
} mcu_state_machine_t;

typedef struct {
    mcu_result_code_t result_code;
    mcu_result_reason_t reason;
    mcu_state_t previous_state;
    mcu_state_t state;
    mcu_device_mode_t device_mode;

    /* 当前锁存的活动原因（若有）。 */
    mcu_fault_code_t fault_code;

    /* 本事件响应中要放置的故障。当 STOP 在更早的故障仍被锁存时被处理，
     * 该值与活动原因不同。 */
    mcu_fault_code_t response_fault_code;

    bool execution_active;
    bool force_safe_outputs;
} mcu_transition_result_t;

void mcu_sm_init(mcu_state_machine_t *machine);
bool mcu_sm_is_valid(const mcu_state_machine_t *machine);
/* 对普通事件而言，缺失结果缓冲区属于无效的调用方输入，按失败即拒绝处理。
 * STOP 仍会被派发，因此诊断缓冲区缺失永远不会压制安全动作。 */
void mcu_sm_dispatch(mcu_state_machine_t *machine,
                     const mcu_event_t *event,
                     mcu_transition_result_t *result);

#endif /* MCU_STATE_MACHINE_H */
