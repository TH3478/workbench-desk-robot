#ifndef MCU_COMMAND_DEDUP_H
#define MCU_COMMAND_DEDUP_H

#include <stdbool.h>
#include <stdint.h>

#include "frame_codec.h"
#include "state_machine.h"
#include "watchdog.h"

/* 普通命令 ID 使用低 15 位作为序号。保留 8 个结果即可覆盖当前受限的
 * 主机实现（一个在途命令加若干重试），同时保持固件内存占用固定。 */
#define MCU_COMMAND_SERIAL_MASK 0x7fffu
#define MCU_COMMAND_SERIAL_HALF_RANGE 0x4000u
#define MCU_COMMAND_REPLAY_WINDOW_SIZE 8u
#define MCU_COMMAND_DEDUP_STORAGE_BUDGET_BYTES 256u

typedef enum {
    MCU_COMMAND_OUTCOME_NONE = 0,
    MCU_COMMAND_OUTCOME_SESSION_CLOSED,
    MCU_COMMAND_OUTCOME_ACCEPTED_NEW,
    MCU_COMMAND_OUTCOME_REJECTED_NEW,
    MCU_COMMAND_OUTCOME_REPLAYED,
    MCU_COMMAND_OUTCOME_REJECTED_CONFLICT,
    MCU_COMMAND_OUTCOME_REJECTED_STALE,
    MCU_COMMAND_OUTCOME_REJECTED_RETRY,
    MCU_COMMAND_OUTCOME_COUNT
} mcu_command_outcome_t;

typedef struct {
    mcu_command_outcome_t outcome;
    bool ack_available;
    /* 仅当序号上全新的普通命令到达安全状态机时为真。
     * 回放与关联性拒绝永远不会置位此标志。 */
    bool ordinary_event_dispatched;
    bool watchdog_refreshed;
    mcu_wire_frame_t ack;
} mcu_command_record_t;

typedef struct {
    bool valid;
    uint16_t command_id;
    mcu_wire_opcode_t opcode;
    uint8_t last_retry_count;
    mcu_wire_result_t result_code;
    mcu_wire_fault_t fault_code;
    mcu_wire_device_mode_t device_mode;
} mcu_command_replay_entry_t;

typedef struct {
    uint32_t initialized;
    bool session_open;
    bool has_last_accepted;
    uint16_t last_accepted_id;
    uint8_t next_slot;
    mcu_command_replay_entry_t entries[MCU_COMMAND_REPLAY_WINDOW_SIZE];
} mcu_command_dedup_t;

/* 启动初始化有意保持普通命令派发处于关闭状态。
 * STOP 仍归 mcu_watchdog_receive_stop() 所有，不受此处闸门限制。 */
void mcu_command_dedup_init(mcu_command_dedup_t *dedup);
bool mcu_command_dedup_is_valid(const mcu_command_dedup_t *dedup);

/* 该可信传输闸门只能在排队的会话前流量被丢弃之后打开。
 * 重新打开已激活的会话会被拒绝，以免调用方静默抹除回放历史。 */
bool mcu_command_dedup_open_session(mcu_command_dedup_t *dedup,
                                    bool queued_traffic_discarded);
bool mcu_command_dedup_close_session(mcu_command_dedup_t *dedup);

/* 接收一个完全解码的普通 COMMAND。结构上非法的输入与损坏的依赖
 * 会返回 false 且不修改输出。会话关闭时返回的记录不带 ACK。
 * 其余每次成功调用都返回关联的 ACK；
 * 只有序号上全新的命令才会派发普通事件。
 *
 * MOVE 与夹爪命令进入 MOVING，HOLD 进入 HOLDING，HEARTBEAT
 * 保持当前安全状态。只有被接受的、序号上全新的活动才能刷新软件看门狗。 */
bool mcu_command_dedup_receive(mcu_command_dedup_t *dedup,
                               mcu_state_machine_t *machine,
                               mcu_watchdog_t *watchdog,
                               const mcu_wire_frame_t *command,
                               uint64_t now_us,
                               mcu_command_record_t *record);

#endif /* MCU_COMMAND_DEDUP_H */
