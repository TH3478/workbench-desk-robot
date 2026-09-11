/* can_bridge.h —— 原始 HAL CAN 封装与 Wire V1 编解码器之间的桥接接口。
 *
 * 职责：唯一原始包络映射的公共类型与入口——封装校验拒绝诊断、
 * STOP 优先路由结果与响应交接状态。
 *
 * 契约文档：docs/architecture/mcu-can-hal-boundary-v1.md（「唯一编码权威」
 *   「MCU 入口方向与路由」「响应交接」）；原始封装定义见 core/hal.h。
 *
 * 五个冻结仲裁标识符的语义与方向（mcu-wire-v1.md「仲裁标识符」）：
 *   MCU_CAN_ID_STOP      0x080  主机 → MCU，最高协议优先级（较低 ID 赢仲裁）；
 *   MCU_CAN_ID_STOP_ACK  0x081  MCU → 主机，关联的安全响应；
 *   MCU_CAN_ID_COMMAND   0x100  主机 → MCU，普通命令流量；
 *   MCU_CAN_ID_ACK       0x101  MCU → 主机，普通关联响应；
 *   MCU_CAN_ID_TELEMETRY 0x180  MCU → 主机，最低协议优先级。
 * 每种帧类型的 DLC 恒为 8；逻辑 16 位 command_id 留在载荷字节 1..2，
 * 绝不写入 11 位仲裁寄存器。
 *
 * 桥接生命周期不变量：上电时普通会话处于「未同步」状态
 * （mcu_command_dedup_t.session_open == false），普通 COMMAND 只会得到
 * SESSION_CLOSED 结果；只有所属传输丢弃排队的会话前流量并通过可信闸门
 * mcu_command_dedup_open_session() 之后才进入「已同步」状态。
 * STOP 路径独立于该闸门，上电后任何时刻都可处理。
 *
 * 编译目标：host、qemu、ch32v307 三目标共源；core/ 不含厂商或平台头文件
 *   （firmware/mcu/README.md「唯一规则」）。
 */
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
 * 转换失败时输出保持不变。
 *
 * 编码方向：逻辑帧 → flags == NONE、DLC 恰为 8 的标准 Classic CAN
 * 数据信封；仲裁 ID 由帧类型唯一决定（五个冻结 ID 之一），八个载荷
 * 字节由 mcu_frame_encode() 按网络字节序（大端）写入。
 * frame_id / sent_at_us / clock_id 是适配器/证据元数据，不在线上传输。
 *
 * 解码方向：先按冻结顺序校验 flags 恰为 NONE、arbitration_id <= 0x7ff、
 * DLC 恰为 8（Wire V1 每种帧的 DLC 恒为 8，接收侧只需比较长度即可
 * 在解析前拒绝错误 DLC），再复用 mcu_frame_decode() 校验版本、保留
 * 字节、枚举值、ID 分区与跨字段语义。 */
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
 * 若返回被接受的 STOP 响应，传输交接确认仍由调用方负责。
 *
 * MCU 入口方向闸门：只接受 STOP（0x080）与普通 COMMAND（0x100）；
 * ACK（0x101）、STOP_ACK（0x081）与遥测（0x180）即使编码合法，在
 * 本入口也是方向错误流量，绝不触及安全状态机。 */
bool mcu_can_bridge_process_frame(mcu_command_dedup_t *dedup,
                                  mcu_state_machine_t *machine,
                                  mcu_watchdog_t *watchdog,
                                  const hal_can_frame *encoded,
                                  uint64_t now_us,
                                  mcu_can_bridge_record_t *record);

/* 从非阻塞 HAL 至多取出一帧，在处理普通命令之前将 STOP 直接送入
 * 看门狗路径，并发送任何响应（ACK 0x101 或 STOP_ACK 0x081，
 * 均为 MCU → 主机方向）。被接受的 STOP_ACK 交接成功即确认
 * 其受限的 core 槽位。所属目标必须串行化调用并配置控制器/FIFO 优先级；
 * 本函数不会隐藏无界的接收队列。 */
bool mcu_can_bridge_poll(mcu_command_dedup_t *dedup,
                         mcu_state_machine_t *machine,
                         mcu_watchdog_t *watchdog,
                         uint64_t now_us,
                         mcu_can_bridge_record_t *record);

#endif /* MCU_CAN_BRIDGE_H */
