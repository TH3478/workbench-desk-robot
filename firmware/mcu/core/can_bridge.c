/* can_bridge.c —— 原始 HAL CAN 封装与 Wire V1 逻辑帧之间的唯一映射实现。
 *
 * 职责：编码时构造 flags == NONE 的标准 Classic CAN 数据信封；解码时先
 * 做严格封装校验（flags、11 位仲裁 ID、DLC 恰为 8）再复用
 * mcu_frame_decode()。MCU 入口只接受 STOP 与普通 COMMAND：STOP 分支在
 * 普通命令之前路由到看门狗路径，COMMAND 走回放窗口；响应经
 * hal_can_send() 交接，被接受的 STOP_ACK 交接成功才确认挂起槽位。
 *
 * 契约文档：docs/architecture/mcu-can-hal-boundary-v1.md（「唯一编码权威」
 *   「MCU 入口方向与路由」「响应交接」）；载荷与 ID 布局来自
 *   docs/architecture/mcu-wire-v1.md（「仲裁标识符」「载荷布局」）。
 *
 * 编译目标：host、qemu、ch32v307 三目标共源；core/ 不含厂商或平台头文件
 *   （firmware/mcu/README.md「唯一规则」）。
 */

#include "can_bridge.h"

/* 编译期不变量：HAL 载荷容量与 Wire V1 DLC（8）一致；
 * 五个冻结仲裁 ID 全部落在 11 位标准 ID 范围（<= 0x7ff）内。 */
_Static_assert(HAL_CAN_CLASSIC_DLC_MAX == MCU_WIRE_DLC,
               "HAL Classic CAN payload must match Wire V1 DLC");
_Static_assert(MCU_CAN_ID_TELEMETRY <= HAL_CAN_STANDARD_ID_MAX,
               "Wire V1 identifiers must fit the standard CAN envelope");

/* ---- 帧与记录的复制/清零 ---- */

/* 把逻辑帧置为安全中性初值（kind == COMMAND、opcode == reserved）。 */
static void clear_wire_frame(mcu_wire_frame_t *frame)
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

/* 逐字段复制逻辑帧。 */
static void copy_wire_frame(mcu_wire_frame_t *destination,
                            const mcu_wire_frame_t *source)
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

/* 逐字段复制原始 HAL 封装（含全部 8 个数据字节）。 */
static void copy_hal_frame(hal_can_frame *destination,
                           const hal_can_frame *source)
{
    unsigned i;

    destination->arbitration_id = source->arbitration_id;
    destination->dlc = source->dlc;
    destination->flags = source->flags;
    for (i = 0u; i < HAL_CAN_CLASSIC_DLC_MAX; i++) {
        destination->data[i] = source->data[i];
    }
}

/* 桥接记录清零：无结果、无请求/响应、无交接。 */
static void clear_record(mcu_can_bridge_record_t *record)
{
    record->outcome = MCU_CAN_BRIDGE_OUTCOME_NONE;
    record->status = MCU_CAN_BRIDGE_OK;
    record->request_decoded = false;
    record->ordinary_event_dispatched = false;
    record->stop_path_entered = false;
    record->response_available = false;
    record->response_handed_off = false;
    clear_wire_frame(&record->request);
    clear_wire_frame(&record->response);
}

/* 逐字段复制桥接记录（含内嵌的两帧逻辑帧）。 */
static void copy_record(mcu_can_bridge_record_t *destination,
                        const mcu_can_bridge_record_t *source)
{
    destination->outcome = source->outcome;
    destination->status = source->status;
    destination->request_decoded = source->request_decoded;
    destination->ordinary_event_dispatched = source->ordinary_event_dispatched;
    destination->stop_path_entered = source->stop_path_entered;
    destination->response_available = source->response_available;
    destination->response_handed_off = source->response_handed_off;
    copy_wire_frame(&destination->request, &source->request);
    copy_wire_frame(&destination->response, &source->response);
}

/* 编解码拒绝原因到桥接状态码的映射；缓冲区过小在桥接路径视为
 * core 拒绝（MCU_CAN_BRIDGE_CORE_REJECTED）。 */
static mcu_can_bridge_status_t map_codec_status(mcu_codec_status_t status)
{
    switch (status) {
    case MCU_CODEC_OK:
        return MCU_CAN_BRIDGE_OK;
    case MCU_CODEC_INVALID_ARGUMENT:
        return MCU_CAN_BRIDGE_INVALID_ARGUMENT;
    case MCU_CODEC_INVALID_LENGTH:
        return MCU_CAN_BRIDGE_INVALID_DLC;
    case MCU_CODEC_UNSUPPORTED_ID:
        return MCU_CAN_BRIDGE_UNSUPPORTED_ID;
    case MCU_CODEC_INVALID_VERSION:
        return MCU_CAN_BRIDGE_INVALID_VERSION;
    case MCU_CODEC_NONZERO_RESERVED:
        return MCU_CAN_BRIDGE_NONZERO_RESERVED;
    case MCU_CODEC_INVALID_FIELD:
        return MCU_CAN_BRIDGE_INVALID_WIRE_FIELD;
    case MCU_CODEC_BUFFER_TOO_SMALL:
    default:
        return MCU_CAN_BRIDGE_CORE_REJECTED;
    }
}

/* ---- 编码与解码 ---- */

/* 编码 Wire V1 帧并构造标准 Classic CAN 信封：flags == NONE、
 * DLC 由编解码器确定；只有编解码成功且仲裁 ID 落在 11 位范围内
 * 才写出输出，失败即拒绝且输出保持不变。 */
mcu_can_bridge_status_t mcu_can_bridge_encode(const mcu_wire_frame_t *frame,
                                              hal_can_frame *encoded)
{
    hal_can_frame local;
    uint8_t encoded_length = 0u;
    mcu_codec_status_t codec_status;

    if (frame == 0 || encoded == 0) {
        return MCU_CAN_BRIDGE_INVALID_ARGUMENT;
    }

    local.arbitration_id = 0u;
    local.dlc = 0u;
    local.flags = (uint8_t)HAL_CAN_FRAME_FLAG_NONE;
    codec_status = mcu_frame_encode(frame,
                                    &local.arbitration_id,
                                    local.data,
                                    sizeof(local.data),
                                    &encoded_length);
    if (codec_status != MCU_CODEC_OK) {
        return map_codec_status(codec_status);
    }
    if (local.arbitration_id > HAL_CAN_STANDARD_ID_MAX) {
        return MCU_CAN_BRIDGE_INVALID_ARBITRATION_ID;
    }
    if (encoded_length != MCU_WIRE_DLC || encoded_length > HAL_CAN_CLASSIC_DLC_MAX) {
        return MCU_CAN_BRIDGE_CORE_REJECTED;
    }

    local.dlc = encoded_length;
    copy_hal_frame(encoded, &local);
    return MCU_CAN_BRIDGE_OK;
}

/* 原始封装校验顺序（mcu-can-hal-boundary-v1.md「唯一编码权威」）：
 * flags 恰为 NONE → arbitration_id <= 0x7ff → DLC 恰为 8 → 复用
 * mcu_frame_decode()。任一失败即拒绝，输出保持不变；
 * 被拒绝的帧绝不产生状态机事件。 */
mcu_can_bridge_status_t mcu_can_bridge_decode(const hal_can_frame *encoded,
                                              mcu_wire_frame_t *frame)
{
    mcu_wire_frame_t decoded;
    mcu_codec_status_t codec_status;

    if (encoded == 0 || frame == 0) {
        return MCU_CAN_BRIDGE_INVALID_ARGUMENT;
    }
    if (encoded->flags != (uint8_t)HAL_CAN_FRAME_FLAG_NONE) {
        return MCU_CAN_BRIDGE_INVALID_FLAGS;
    }
    if (encoded->arbitration_id > HAL_CAN_STANDARD_ID_MAX) {
        return MCU_CAN_BRIDGE_INVALID_ARBITRATION_ID;
    }
    if (encoded->dlc != MCU_WIRE_DLC) {
        return MCU_CAN_BRIDGE_INVALID_DLC;
    }

    codec_status = mcu_frame_decode(encoded->arbitration_id,
                                    encoded->data,
                                    encoded->dlc,
                                    &decoded);
    if (codec_status != MCU_CODEC_OK) {
        return map_codec_status(codec_status);
    }

    copy_wire_frame(frame, &decoded);
    return MCU_CAN_BRIDGE_OK;
}

/* 编码并交给目标 HAL。hal_can_send() == true 只表示传输交接完成，
 * 不证明仲裁、线上送达、远端接收或执行器动作。 */
mcu_can_bridge_status_t mcu_can_bridge_send(const mcu_wire_frame_t *frame)
{
    hal_can_frame encoded;
    mcu_can_bridge_status_t status = mcu_can_bridge_encode(frame, &encoded);

    if (status != MCU_CAN_BRIDGE_OK) {
        return status;
    }
    return hal_can_send(&encoded) ? MCU_CAN_BRIDGE_OK
                                  : MCU_CAN_BRIDGE_HAL_SEND_FAILED;
}

/* 安全依赖合法性：状态机与看门狗对象都必须是合法状态。 */
static bool safety_dependencies_are_valid(const mcu_state_machine_t *machine,
                                          const mcu_watchdog_t *watchdog)
{
    return mcu_sm_is_valid(machine) && mcu_watchdog_is_valid(watchdog);
}

/* ---- MCU 入口路由 ---- */

/* 处理一个已收到的原始封装（不调用 hal_can_send()）。
 * 解码成功后的路由顺序：STOP 先行（绕过会话闸门与回放窗口，
 * 经 mcu_watchdog_receive_stop() 派发）；随后是普通 COMMAND
 * （经 mcu_command_dedup_receive()，dedup 为 null 或损坏时
 * 失败即拒绝）；ACK / STOP_ACK / 遥测在 MCU 入口是方向错误
 * 流量，返回 UNEXPECTED_DIRECTION，绝不触及安全状态。 */
bool mcu_can_bridge_process_frame(mcu_command_dedup_t *dedup,
                                  mcu_state_machine_t *machine,
                                  mcu_watchdog_t *watchdog,
                                  const hal_can_frame *encoded,
                                  uint64_t now_us,
                                  mcu_can_bridge_record_t *record)
{
    mcu_can_bridge_record_t local;
    mcu_can_bridge_status_t status;

    if (machine == 0 || watchdog == 0 || encoded == 0 || record == 0 ||
        !safety_dependencies_are_valid(machine, watchdog)) {
        return false;
    }

    clear_record(&local);
    status = mcu_can_bridge_decode(encoded, &local.request);
    if (status != MCU_CAN_BRIDGE_OK) {
        local.outcome = MCU_CAN_BRIDGE_OUTCOME_REJECTED;
        local.status = status;
        copy_record(record, &local);
        return true;
    }
    local.request_decoded = true;

    /* STOP 有意在普通路径之前先行检查。它绕过会话闸门与回放窗口，
     * 且永远不会被转换为命令。 */
    if (local.request.kind == MCU_WIRE_FRAME_STOP) {
        mcu_watchdog_record_t stop_record;

        local.stop_path_entered = true;
        if (!mcu_watchdog_receive_stop(watchdog,
                                       machine,
                                       &local.request,
                                       now_us,
                                       &stop_record)) {
            local.outcome = MCU_CAN_BRIDGE_OUTCOME_REJECTED;
            local.status = MCU_CAN_BRIDGE_CORE_REJECTED;
            copy_record(record, &local);
            return true;
        }
        local.outcome = MCU_CAN_BRIDGE_OUTCOME_STOP_HANDLED;
        local.response_available = true;
        copy_wire_frame(&local.response, &stop_record.frame);
        copy_record(record, &local);
        return true;
    }

    if (local.request.kind == MCU_WIRE_FRAME_COMMAND) {
        mcu_command_record_t command_record;

        if (dedup == 0 || !mcu_command_dedup_is_valid(dedup) ||
            !mcu_command_dedup_receive(dedup,
                                       machine,
                                       watchdog,
                                       &local.request,
                                       now_us,
                                       &command_record)) {
            local.outcome = MCU_CAN_BRIDGE_OUTCOME_REJECTED;
            local.status = MCU_CAN_BRIDGE_CORE_REJECTED;
            copy_record(record, &local);
            return true;
        }
        local.ordinary_event_dispatched = command_record.ordinary_event_dispatched;
        if (!command_record.ack_available) {
            local.outcome = MCU_CAN_BRIDGE_OUTCOME_SESSION_CLOSED;
            copy_record(record, &local);
            return true;
        }
        local.outcome = MCU_CAN_BRIDGE_OUTCOME_COMMAND_HANDLED;
        local.response_available = true;
        copy_wire_frame(&local.response, &command_record.ack);
        copy_record(record, &local);
        return true;
    }

    /* ACK、STOP_ACK 与遥测在 Wire V1 下均由 MCU 发出。携带这些 ID 之一的帧
     * 出现在 MCU 入口时，虽然编码合法但方向错误，绝不能到达安全状态。 */
    local.outcome = MCU_CAN_BRIDGE_OUTCOME_REJECTED;
    local.status = MCU_CAN_BRIDGE_UNEXPECTED_DIRECTION;
    copy_record(record, &local);
    return true;
}

/* ---- 单帧轮询 ---- */

/* 每次调用至多从非阻塞 hal_can_recv() 取出一帧并处理；有响应时经
 * hal_can_send() 交接，被接受的 STOP_ACK 交接成功后立即确认挂起
 * 槽位（确认失败记为 CORE_REJECTED）。调用方必须串行化本入口并
 * 配置控制器过滤器/FIFO 优先级；core 不隐藏无界接收队列。 */
bool mcu_can_bridge_poll(mcu_command_dedup_t *dedup,
                         mcu_state_machine_t *machine,
                         mcu_watchdog_t *watchdog,
                         uint64_t now_us,
                         mcu_can_bridge_record_t *record)
{
    hal_can_frame encoded;
    mcu_can_bridge_record_t local;
    mcu_can_bridge_status_t send_status;

    if (machine == 0 || watchdog == 0 || record == 0 ||
        !safety_dependencies_are_valid(machine, watchdog)) {
        return false;
    }
    if (!hal_can_recv(&encoded)) {
        clear_record(&local);
        local.outcome = MCU_CAN_BRIDGE_OUTCOME_NO_FRAME;
        local.status = MCU_CAN_BRIDGE_NO_FRAME;
        copy_record(record, &local);
        return true;
    }
    if (!mcu_can_bridge_process_frame(dedup,
                                      machine,
                                      watchdog,
                                      &encoded,
                                      now_us,
                                      &local)) {
        return false;
    }
    if (!local.response_available) {
        copy_record(record, &local);
        return true;
    }

    send_status = mcu_can_bridge_send(&local.response);
    if (send_status != MCU_CAN_BRIDGE_OK) {
        local.status = send_status;
        copy_record(record, &local);
        return true;
    }
    local.response_handed_off = true;

    if (local.response.kind == MCU_WIRE_FRAME_STOP_ACK &&
        local.response.result_code == MCU_WIRE_RESULT_ACCEPTED &&
        !mcu_watchdog_confirm_stop_ack(watchdog,
                                       local.response.command_id,
                                       local.response.retry_count,
                                       now_us)) {
        local.status = MCU_CAN_BRIDGE_CORE_REJECTED;
    }
    copy_record(record, &local);
    return true;
}
