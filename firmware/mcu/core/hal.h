/* HAL 边界。任务 FW1。
 *
 * core/ 调用这些接口，从不直接触碰寄存器。每个目标
 * （qemu、ch32v307、host）都实现此头文件，且仅此而已。
 *
 * 来自 ADR-0003 的规则：core/ 中出现 #ifdef CH32V307 就意味着这条边界
 * 划分错了。如果 core/ 需要板子能做而 QEMU 不能做的事，
 * 修复方式是在这里新增一个函数，并在三处各实现一遍。
 */
#ifndef MCU_HAL_H
#define MCU_HAL_H

#include <stdint.h>
#include <stdbool.h>

/* ---------------------------------------------------------------- 诊断输出
 * 字节出口。在 QEMU 上是 16550 UART；在板子上是 USART1；在 host 上是
 * stdout。core/ 仅将其用于测试输出——绝不用于安全路径，
 * 因为阻塞写入的时间无上界。
 */
void hal_putc(char c);
void hal_puts(const char *s);
void hal_put_u32(uint32_t v);

/* 当 main 返回时由 crt0 调用，也由陷阱处理程序调用。 */
void hal_report_exit(int code);
void hal_report_trap(uint32_t mcause, uint32_t mepc);

/* ---------------------------------------------------------------------- 时间
 * 单调递增的滴答计数。在 RISC-V 上由 mtime 支撑，在 host 上是普通计数器。
 *
 * 有意使用 uint64_t：32 位微秒计数器 71 分钟就会回绕，
 * 而一个每 71 分钟就行为异常一次的看门狗还不如永不工作的看门狗。
 * FW6 负责 CAN 序号回绕（其宽度由线上格式固定）；
 * 这里我们完全可以选择不回绕。
 */
uint64_t hal_now_us(void);

/* 在绝对时间点上武装一次单次定时器中断。由 FW5 使用。 */
void hal_timer_arm_us(uint64_t deadline_us);
void hal_timer_disarm(void);
/* 在设置完成后启用目标的全局定时器中断投递。 */
void hal_timer_enable(void);

/* ----------------------------------------------------------------------- CAN
 * Wire V1 解码之前的原始控制器封装。仲裁 ID 是标准 11 位 CAN 标识符；
 * 不是存放在负载字节 1..2 中的逻辑 16 位命令 ID。
 * 将这两个名称区分开，可防止 STOP 的 command_id（>= 0x8000）
 * 被写入 11 位控制器寄存器。
 *
 * flags 有意使用单个字节而非 C 位域，以便每个目标都能显式映射
 * 控制器元数据。Wire V1 仅接受 flags == NONE、DLC == 8 且
 * arbitration_id <= 0x7ff。桥接层会在解码或安全状态变更之前
 * 拒绝所有其他封装。
 */
#define HAL_CAN_STANDARD_ID_MAX 0x07ffu
#define HAL_CAN_CLASSIC_DLC_MAX 8u

typedef enum {
    HAL_CAN_FRAME_FLAG_NONE = 0u,
    HAL_CAN_FRAME_FLAG_EXTENDED_ID = 1u << 0,
    HAL_CAN_FRAME_FLAG_REMOTE = 1u << 1,
    HAL_CAN_FRAME_FLAG_ERROR = 1u << 2,
    HAL_CAN_FRAME_FLAG_FD = 1u << 3
} hal_can_frame_flag_t;

typedef struct {
    uint16_t arbitration_id;
    uint8_t dlc;
    uint8_t flags;
    uint8_t data[HAL_CAN_CLASSIC_DLC_MAX];
} hal_can_frame;

bool hal_can_init(void);
bool hal_can_send(const hal_can_frame *f);

/* 非阻塞。没有待处理帧时返回 false。 */
bool hal_can_recv(hal_can_frame *out);

/* ------------------------------------------------------------------ 看门狗
 * 硬件看门狗，与 core/ 中的软件超时相互独立。在板子上它是 IWDG（FW20），
 * 一旦启动就无法停止——这正是它的意义所在。
 * 在 QEMU 上对其建模的程度足以测试喂狗路径。
 */
void hal_wdt_start(uint32_t timeout_ms);
void hal_wdt_feed(void);
uint32_t hal_wdt_feed_count(void);
bool hal_wdt_is_expired(void);

#endif /* MCU_HAL_H */
