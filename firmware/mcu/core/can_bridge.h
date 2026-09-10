#ifndef MCU_CAN_BRIDGE_H
#define MCU_CAN_BRIDGE_H

#include <stdbool.h>
#include <stdint.h>

#include "command_dedup.h"
#include "frame_codec.h"
#include "hal.h"
#include "state_machine.h"
#include "watchdog.h"

/* 针对输入被拒绝的具体边界的精确诊断。
 * 被拒绝的原始帧或 Wire V1 帧绝不会到达安全状态机。 */
typedef enum {
    MCU_CAN_BRIDGE_OK = 0,
    MCU_CAN_BRIDGE_NO_FRAME,
    MCU_CAN_BRIDGE_INVALID_ARGUMENT,
    MCU_CAN_BRIDGE_INVALID_FLAGS,
    MCU_CAN_BRIDGE_INVALID_ARBITRATION_ID,
    MCU_CAN_BRIDGE_INVALID_DLC,
    MCU_CAN_BRIDGE_UNSUPPORTED_ID,
    MCU_CAN_BRIDGE_INVALID_VERSION,
    MCU_CAN_BRIDGE_NONZERO_RESERVED,
    MCU_CAN_BRIDGE_INVALID_WIRE_FIELD,
    MCU_CAN_BRIDGE_UNEXPECTED_DIRECTION,
    MCU_CAN_BRIDGE_CORE_REJECTED,
    MCU_CAN_BRIDGE_HAL_SEND_FAILED,
    MCU_CAN_BRIDGE_STATUS_COUNT
} mcu_can_bridge_status_t;

typedef enum {
    MCU_CAN_BRIDGE_OUTCOME_NONE = 0,
    MCU_CAN_BRIDGE_OUTCOME_NO_FRAME,
    MCU_CAN_BRIDGE_OUTCOME_REJECTED,
    MCU_CAN_BRIDGE_OUTCOME_SESSION_CLOSED,
    MCU_CAN_BRIDGE_OUTCOME_COMMAND_HANDLED,
    MCU_CAN_BRIDGE_OUTCOME_STOP_HANDLED,
    MCU_CAN_BRIDGE_OUTCOME_COUNT
} mcu_can_bridge_outcome_t;

typedef struct {
    mcu_can_bridge_outcome_t outcome;
    mcu_can_bridge_status_t status;
    bool request_decoded;
    bool ordinary_event_dispatched;
    bool stop_path_entered;
    bool response_available;
    bool response_handed_off;
    mcu_wire_frame_t request;
    mcu_wire_frame_t response;
} mcu_can_bridge_record_t;

/* 这两个转换函数是原始 HAL 封装与 Wire V1 之间的唯一映射。
 * 转换失败时输出保持不变。 */
mcu_can_bridge_status_t mcu_can_bridge_encode(const mcu_wire_frame_t *frame,
                                              hal_can_frame *encoded);
mcu_can_bridge_status_t mcu_can_bridge_decode(const hal_can_frame *encoded,
                                              mcu_wire_frame_t *frame);

/* 编码一个完整的 Wire V1 帧并交给目标 HAL。HAL 返回 true 仅表示
 * 传输交接完成，不表示总线送达或执行器证明。 */
mcu_can_bridge_status_t mcu_can_bridge_send(const mcu_wire_frame_t *frame);

/* 处理一个已收到的原始封装，不调用 hal_can_send()。
 * 此函数由 Host/QEMU 逻辑测试共用。对于 STOP，dedup 可以为 null 或损坏，
 * 因为安全路径相互独立；此时普通命令会失败即拒绝。
 * 若返回被接受的 STOP 响应，传输交接确认仍由调用方负责。 */
bool mcu_can_bridge_process_frame(mcu_command_dedup_t *dedup,
                                  mcu_state_machine_t *machine,
                                  mcu_watchdog_t *watchdog,
                                  const hal_can_frame *encoded,
                                  uint64_t now_us,
                                  mcu_can_bridge_record_t *record);

/* 从非阻塞 HAL 至多取出一帧，在处理普通命令之前将 STOP 直接送入
 * 看门狗路径，并发送任何响应。被接受的 STOP_ACK 交接成功即确认
 * 其受限的 core 槽位。所属目标必须串行化调用并配置控制器/FIFO 优先级；
 * 本函数不会隐藏无界的接收队列。 */
bool mcu_can_bridge_poll(mcu_command_dedup_t *dedup,
                         mcu_state_machine_t *machine,
                         mcu_watchdog_t *watchdog,
                         uint64_t now_us,
                         mcu_can_bridge_record_t *record);

#endif /* MCU_CAN_BRIDGE_H */
