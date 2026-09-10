/* watchdog.h —— 平台无关时序安全路径接口（软件链路看门狗与 STOP ACK 期限）。
 *
 * 职责：受控时序常量（心跳周期、软件链路期限、STOP_ACK 交接期限、
 * 硬件看门狗周期）、活动分类、记录类型与对象状态。
 *
 * 契约文档：docs/architecture/mcu-watchdog-v1.md（「受控常量」「软件链路
 *   看门狗」「STOP 确认时序」「硬件看门狗与 HAL 边界」）。
 *
 * 编译目标：host、qemu、ch32v307 三目标共源；core/ 不含厂商或平台头文件
 *   （firmware/mcu/README.md「唯一规则」）。
 */
#ifndef MCU_WATCHDOG_H
#define MCU_WATCHDOG_H

#include <stdbool.h>
#include <stdint.h>

#include "frame_codec.h"
#include "state_machine.h"

/* 受控的 Wire V1 时序实现常量。取值有意小到足以支撑确定性的 QEMU 证据，
 * 并非物理 CH32V307 延迟或急停测量值。 */
#define MCU_HEARTBEAT_PERIOD_US 50000ull
#define MCU_SOFTWARE_WATCHDOG_TIMEOUT_US 150000ull
#define MCU_STOP_ACK_DEADLINE_US 10000ull
#define MCU_HARDWARE_WATCHDOG_PERIOD_MS 500u

typedef enum {
    MCU_WATCHDOG_ACTIVITY_VALID_NEW = 0,
    MCU_WATCHDOG_ACTIVITY_RETRY,
    MCU_WATCHDOG_ACTIVITY_DUPLICATE,
    MCU_WATCHDOG_ACTIVITY_STALE,
    MCU_WATCHDOG_ACTIVITY_MALFORMED,
    MCU_WATCHDOG_ACTIVITY_STOP,
    MCU_WATCHDOG_ACTIVITY_COUNT
} mcu_watchdog_activity_t;

typedef enum {
    MCU_WATCHDOG_RECORD_NONE = 0,
    MCU_WATCHDOG_RECORD_FAULT_TELEMETRY,
    MCU_WATCHDOG_RECORD_STOP_ACK,
    MCU_WATCHDOG_RECORD_STOP_TIMEOUT,
    MCU_WATCHDOG_RECORD_COUNT
} mcu_watchdog_record_kind_t;

typedef enum {
    MCU_WATCHDOG_FAULT_NONE = 0,
    MCU_WATCHDOG_FAULT_WATCHDOG_EXPIRED,
    MCU_WATCHDOG_FAULT_STOP_TIMEOUT,
    MCU_WATCHDOG_FAULT_COUNT
} mcu_watchdog_fault_t;

typedef struct {
    mcu_watchdog_record_kind_t kind;
    mcu_watchdog_fault_t fault;
    uint64_t observed_at_us;
    uint64_t deadline_us;
    uint16_t command_id;
    uint8_t retry_count;
    mcu_wire_frame_t frame;
} mcu_watchdog_record_t;

typedef struct {
    uint32_t initialized;
    uint32_t next_telemetry_sequence;

    bool link_watchdog_armed;
    bool watchdog_cause_active;
    bool watchdog_record_emitted;
    uint64_t link_deadline_us;

    bool stop_ack_pending;
    bool stop_timeout_cause_active;
    bool stop_timeout_record_emitted;
    uint64_t stop_deadline_us;
    uint16_t stop_command_id;
    /* 最近一次发出的 STOP_ACK 尝试的重试计数。 */
    uint8_t stop_retry_count;
    uint64_t stop_ack_observed_at_us;
    /* 缓存的语义 ACK；协议级重试在此只更新尝试元数据，
     * 从不改动 result_code、fault_code 或 device_mode。 */
    mcu_wire_frame_t stop_ack_frame;
} mcu_watchdog_t;

void mcu_watchdog_init(mcu_watchdog_t *watchdog, uint32_t first_telemetry_sequence);
bool mcu_watchdog_is_valid(const mcu_watchdog_t *watchdog);

/* 只有已校验完整帧并接受了序号上全新命令的调用方才可传入 VALID_NEW。
 * 重试、重复、过期、畸形与 STOP 活动永远不会刷新执行看门狗。 */
bool mcu_watchdog_note_activity(mcu_watchdog_t *watchdog,
                                const mcu_state_machine_t *machine,
                                mcu_watchdog_activity_t activity,
                                uint64_t now_us);

/* 轮询无需分配且具有确定性。每次调用至多发布一条记录，
 * 无截止时间触发时输出保持不变。 */
bool mcu_watchdog_poll(mcu_watchdog_t *watchdog,
                       mcu_state_machine_t *machine,
                       uint64_t now_us,
                       mcu_watchdog_record_t *record);

/* 合法的 STOP 在本函数返回之前即被派发。返回的 ACK 可立即交给传输层；
 * 完全相同的链路重试会重放缓存记录，而携带相同命令 ID 且
 * retry_count 严格更大的协议重试则回显其新计数并保留缓存的结果字段。
 * retry_count 更小即为过期并被拒绝；待处理的重试序号不会回绕。
 * 两类重试都不会延长待处理的截止时间。
 * 调用方必须通过 mcu_watchdog_confirm_stop_ack() 确认交接。 */
bool mcu_watchdog_receive_stop(mcu_watchdog_t *watchdog,
                               mcu_state_machine_t *machine,
                               const mcu_wire_frame_t *stop,
                               uint64_t now_us,
                               mcu_watchdog_record_t *record);

/* 恰在截止时刻的确认仍在闭区间边界之内。
 * 迟到的确认会被拒绝，并使超时原因可通过 poll() 观测到。 */
bool mcu_watchdog_confirm_stop_ack(mcu_watchdog_t *watchdog,
                                   uint16_t command_id,
                                   uint8_t retry_count,
                                   uint64_t now_us);

/* 可信安全控制操作：此操作并不授权复位。它只是记录
 * 外部所有者已清除存续的时序原因。 */
void mcu_watchdog_mark_causes_cleared(mcu_watchdog_t *watchdog);

/* 复位仍须经过现有状态机的授权闸门与原因闸门。
 * 存续的时序原因会强制 cause_cleared=false，
 * 无论调用方请求的值是什么。 */
bool mcu_watchdog_request_reset(mcu_watchdog_t *watchdog,
                                mcu_state_machine_t *machine,
                                bool reset_authorized,
                                bool cause_cleared,
                                mcu_transition_result_t *result);

/* 仅当处于合法且无故障的状态、且不存在活动时序原因时，
 * 才允许喂硬件看门狗。挂死的循环或已超时的安全路径
 * 无法成功调用本函数。 */
bool mcu_watchdog_should_feed_hardware(const mcu_watchdog_t *watchdog,
                                       const mcu_state_machine_t *machine);

#endif /* MCU_WATCHDOG_H */
