/* QEMU virt 机器 HAL。任务 FW1/FW2。
 *
 * 职责：这是 rv32imac 模拟目标（`make qemu` / `make test-qemu`）
 * 的 core/hal.h 实现。core/ 的同一份源码在这里编译为 RISC-V
 * 镜像，在 QEMU 上证明真实定时器中断下的看门狗时序、跨序列
 * 回绕的去重、帧编解码器与 ID 分区强制等（firmware/mcu/README.md
 * 的「QEMU 证明什么、不证明什么」）。它不证明 CH32V307 寄存器
 * 行为、位时序与物理电气行为。
 *
 * 地址来自 QEMU hw/riscv/virt.c 的内存映射。它们只存在于这里、
 * 别处绝无——这正是本文件存在的全部理由。
 */
#include "hal.h"

/* NS16550A UART。 */
/* 16550 兼容 UART，内存映射在 0x10000000（QEMU virt 约定）。 */
#define UART0_BASE  0x10000000u
/* 发送保持寄存器（THR）：写入一个字节即送入移位寄存器发出。 */
#define UART_THR    (*(volatile uint8_t *)(UART0_BASE + 0x00))
/* 线路状态寄存器（LSR）：第 5 位 THRE 表示发送保持寄存器已空。 */
#define UART_LSR    (*(volatile uint8_t *)(UART0_BASE + 0x05))
#define UART_LSR_THRE 0x20u   /* 发送保持寄存器为空 */

/* CLINT：mtime 在本机器上是内存映射的 64 位计数器，以 10 MHz 走时。
 * 在 CH32V307 上它则是 mtime CSR，这正是 hal_now_us
 * 位于 HAL 之后、而非 core/ 中宏的原因。
 */
/* CLINT 即核内本地中断器：mtimecmp 与 mtime 匹配时触发机器
 * 定时器中断，由 crt0.S 分发给 mcu_qemu_timer_interrupt。 */
#define CLINT_BASE      0x02000000u
/* 偏移 0x4000：mtimecmp 寄存器，写入绝对滴答截止时间。 */
#define CLINT_MTIMECMP  (*(volatile uint64_t *)(CLINT_BASE + 0x4000))
/* 偏移 0xBFF8：mtime 寄存器，单调递增的 64 位滴答计数。 */
#define CLINT_MTIME     (*(volatile uint64_t *)(CLINT_BASE + 0xBFF8))
/* mtime 频率：10,000,000 滴答/秒。 */
#define MTIME_HZ        10000000ull

/* 实现 hal_putc：向 16550 UART 输出一个字节。
 * 先自旋等待 THRE（LSR 第 5 位）置位再写入 THR——在慢速控制台上
 * 该等待时间无上界，因此 core/ 仅把此路径用于测试输出，
 * 绝不用于安全路径。 */
void hal_putc(char c)
{
    while (!(UART_LSR & UART_LSR_THRE)) { }
    UART_THR = (uint8_t)c;
}

/* 实现 hal_puts：逐字节输出字符串，'\n' 展开为 CRLF。 */
void hal_puts(const char *s)
{
    for (; *s; s++) {
        if (*s == '\n') hal_putc('\r');
        hal_putc(*s);
    }
}

/* 实现 hal_put_u32：十进制输出无符号 32 位值。 */
void hal_put_u32(uint32_t v)
{
    /* 十进制输出，不用 printf：引入 stdio 会击穿体积预算，
     * 并拖入 FW9 所禁止的分配器。 */
    char buf[11];
    int i = 0;
    if (v == 0) { hal_putc('0'); return; }
    while (v > 0 && i < (int)sizeof(buf)) { buf[i++] = (char)('0' + (v % 10)); v /= 10; }
    while (i-- > 0) hal_putc(buf[i]);
}

/* 实现 hal_report_exit：打印结果并写 SiFive 测试终结器。 */
void hal_report_exit(int code)
{
    hal_puts(code == 0 ? "\n[mcu] PASS\n" : "\n[mcu] FAIL code=");
    if (code != 0) { hal_put_u32((uint32_t)code); hal_putc('\n'); }

    /* 把退出码交给测试框架，使 `make test-qemu` 在失败时判负。
     * QEMU 的 riscv virt 在 0x100000 处暴露 SiFive 测试终结器：
     *   0x5555 = 通过，0x3333 | (code << 16) = 失败。
     */
    volatile uint32_t *finisher = (volatile uint32_t *)0x100000u;
    *finisher = (code == 0) ? 0x5555u : (0x3333u | ((uint32_t)code << 16));
}

/* 实现 hal_report_trap：报告陷阱原因与地址，随后停机。 */
void hal_report_trap(uint32_t mcause, uint32_t mepc)
{
    /* 安全 MCU 中的陷阱绝非寻常。精确报告发生了什么、发生在哪里，
     * 然后让 crt0 停机——不做任何恢复尝试。 */
    hal_puts("\n[mcu] TRAP mcause=");
    hal_put_u32(mcause);
    hal_puts(" mepc=");
    hal_put_u32(mepc);
    hal_putc('\n');

    /* 以固定失败码 99 写测试终结器（0x3333 | 99 << 16）。 */
    volatile uint32_t *finisher = (volatile uint32_t *)0x100000u;
    *finisher = 0x3333u | (99u << 16);
}

/* 对 64 位定时器值做除法，同时避免把 __udivdi3 拉进独立运行的
 * rv32 镜像。每一半都只用 32 位移位处理。
 */
/* 参数按两个 32 位半字逐位做恢复余数除法。 */
static uint64_t divide_u64_by_10(uint64_t value)
{
    uint32_t words[2] = {(uint32_t)(value >> 32), (uint32_t)value};
    uint32_t quotient[2] = {0, 0};
    uint32_t remainder = 0;

    for (unsigned word = 0; word < 2; word++) {
        for (int bit = 31; bit >= 0; bit--) {
            remainder = (remainder << 1) | ((words[word] >> bit) & 1u);
            if (remainder >= 10u) {
                remainder -= 10u;
                quotient[word] |= 1u << bit;
            }
        }
    }

    return ((uint64_t)quotient[0] << 32) | quotient[1];
}

/* 本机器的 mtime 为 10 MHz，因此微秒数 = 滴答数 / 10。 */
/* 实现 hal_now_us：读 CLINT mtime 并换算为单调微秒。 */
uint64_t hal_now_us(void)
{
    return divide_u64_by_10(CLINT_MTIME);
}

/* 实现 hal_timer_arm_us：在绝对微秒截止时间武装单次定时器。 */
void hal_timer_arm_us(uint64_t deadline_us)
{
    /* 反向同理：乘以 10，而不是除以 1000000。 */
    CLINT_MTIMECMP = deadline_us * (MTIME_HZ / 1000000ull);
    /* 使能机器定时器中断（MTIE，第 7 位）。 */
    __asm__ volatile("csrs mie, %0" :: "r"(1u << 7));
}

/* 实现 hal_timer_disarm：清 MTIE 并把 mtimecmp 推到最大值，
 * 使中断不再触发。 */
void hal_timer_disarm(void)
{
    __asm__ volatile("csrc mie, %0" :: "r"(1u << 7));
    CLINT_MTIMECMP = (uint64_t)-1;
}

/* 实现 hal_timer_enable：设置完成后启用全局中断。 */
void hal_timer_enable(void)
{
    /* 在 mtvec 有效之后使能全局机器中断（MIE，第 3 位）。 */
    __asm__ volatile("csrs mstatus, %0" :: "r"(1u << 3));
}

/* --- CAN：FW10 将通过 PCI 把 CTU CAN FD 接到主机 vcan。尚未实现。-------
 * 宁可返回 false 也不假装成功：报告成功的桩会让 FW4 的测试
 * 对着空气通过。
 * 因此三个 hal_can_* 均为恒返回 false 的桩，`make test-qemu`
 * 的 CAN 传输部分显式保持 NOT_EXECUTED。
 */
bool hal_can_init(void) { return false; }
bool hal_can_send(const hal_can_frame *f) { (void)f; return false; }
bool hal_can_recv(hal_can_frame *out) { (void)out; return false; }

/* QEMU virt 没有 CH32V307 的 IWDG。此受限模型记录相同的启动、
 * 喂狗与过期判定，使 core 的喂狗策略可被观测，
 * 同时又不会把它伪装成物理硬件证据。 */
static bool qemu_wdt_running;
static bool qemu_wdt_expired;
static uint32_t qemu_wdt_timeout_ms;
static uint32_t qemu_wdt_feeds;
static uint64_t qemu_wdt_deadline_us;

/* 无符号半区间到期判定，与 host 假实现及 core/watchdog 的
 * uint64 回绕规则一致（docs/architecture/mcu-watchdog-v1.md）。 */
static bool qemu_deadline_reached(uint64_t now_us, uint64_t deadline_us)
{
    return (uint64_t)(now_us - deadline_us) < (UINT64_C(1) << 63);
}

/* 实现 hal_wdt_start：启动建模的硬件看门狗，期限为
 * 当前时间 + timeout_ms × 1000 微秒，并清零喂狗计数。 */
void hal_wdt_start(uint32_t timeout_ms)
{
    qemu_wdt_running = true;
    qemu_wdt_expired = false;
    qemu_wdt_timeout_ms = timeout_ms;
    qemu_wdt_feeds = 0u;
    qemu_wdt_deadline_us = hal_now_us() + (uint64_t)timeout_ms * 1000u;
}

/* 实现 hal_wdt_feed：刷新期限并计数。已过期或未启动时不喂，
 * 与硬件 IWDG「过期即锁存」的语义一致。 */
void hal_wdt_feed(void)
{
    if (!qemu_wdt_running || qemu_wdt_expired) {
        return;
    }
    qemu_wdt_feeds++;
    qemu_wdt_deadline_us = hal_now_us() + (uint64_t)qemu_wdt_timeout_ms * 1000u;
}

/* 实现 hal_wdt_feed_count：喂狗次数（证据计数器）。 */
uint32_t hal_wdt_feed_count(void)
{
    return qemu_wdt_feeds;
}

/* 实现 hal_wdt_is_expired：查询到期状态。到期是锁存的：
 * 一旦判定过期便保持为真，直到 hal_wdt_start 重新启动。 */
bool hal_wdt_is_expired(void)
{
    if (!qemu_wdt_running) {
        return false;
    }
    if (qemu_deadline_reached(hal_now_us(), qemu_wdt_deadline_us)) {
        qemu_wdt_expired = true;
    }
    return qemu_wdt_expired;
}
