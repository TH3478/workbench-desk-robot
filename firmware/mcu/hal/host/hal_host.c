/* x86_64 Host HAL。实现 core/hal.h 的全部接口。
 *
 * 职责：这是确定性的主机侧测试目标。core/ 的同一份源码在这里以
 * host 编译器构建，供 `make test-host` 快速验证逻辑契约
 * （docs/architecture/mcu-protocol-v1.md、mcu-wire-v1.md、
 * mcu-watchdog-v1.md、mcu-command-dedup-v1.md 所冻结的行为）。
 *
 * 时间、定时器与 CAN 都是假实现：时钟不会自己走动，由测试注入
 * `now_us`；CAN 收发走定长固定测试队列（hal_host_test.h）。因此
 * 本文件只证明逻辑与边界行为，不构成任何物理板卡证据。
 *
 * 与 hal/qemu/hal_qemu.c 同契约：core/ 从不直接触碰寄存器，本文件
 * 就是 host 目标那一份实现。
 */
#include "hal.h"
#include "hal_host_test.h"

#include <stdio.h>

/* 假时钟的当前时间（微秒）。它从不自行走动：测试通过构造各自
 * 的假时钟（如 watchdog_tests.c 的 fake_clock_t）驱动 core 的
 * 时间输入，本静态量保持 0，只用于看门狗假实现的内部换算。 */
static uint64_t host_now_us;
/* 单次定时器的武装截止时间；UINT64_MAX 表示已解除。 */
static uint64_t host_timer_deadline_us;
static uint32_t host_wdt_timeout_ms;
static uint64_t host_wdt_deadline_us;
static uint32_t host_wdt_feeds;
static bool host_wdt_running;
static bool host_can_initialized;
/* 定长收发队列：容量由 HAL_HOST_CAN_QUEUE_CAPACITY 固定为 16。 */
static hal_can_frame host_can_rx[HAL_HOST_CAN_QUEUE_CAPACITY];
static hal_can_frame host_can_tx[HAL_HOST_CAN_QUEUE_CAPACITY];
static uint8_t host_can_rx_count;
static uint8_t host_can_tx_count;

/* 逐字段拷贝一个 HAL CAN 帧，避免结构体整体赋值带来的填充字节问题。 */
static void copy_can_frame(hal_can_frame *destination,
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

/* 无符号半区间比较：now_us >= deadline_us（含相等）视为到期。
 * 与 core/watchdog 的 uint64 回绕规则一致，任何计时窗口都短于
 * 计数器区间的一半（docs/architecture/mcu-watchdog-v1.md）。 */
static bool host_deadline_reached(uint64_t now_us, uint64_t deadline_us)
{
    return (uint64_t)(now_us - deadline_us) < (UINT64_C(1) << 63);
}

/* 实现 hal_putc：字节出口走 stdout，仅供测试输出使用。
 * 与 QEMU 版不同，这里的写入时间无上界，绝不用于安全路径。 */
void hal_putc(char c)
{
    (void)putchar((int)c);
}

/* 实现 hal_puts：向 stdout 写 C 字符串，空指针直接跳过。 */
void hal_puts(const char *s)
{
    if (s != 0) {
        (void)fputs(s, stdout);
    }
}

/* 实现 hal_put_u32：十进制打印无符号 32 位值（printf %u）。 */
void hal_put_u32(uint32_t value)
{
    (void)printf("%u", (unsigned)value);
}

/* 实现 hal_report_exit：host 版把退出码交给 main 的返回值，
 * 因此这里无事可做。 */
void hal_report_exit(int code)
{
    (void)code;
}

/* 实现 hal_report_trap：host 构建不会产生 RISC-V 陷阱，
 * 保留参数引用以维持接口一致。 */
void hal_report_trap(uint32_t mcause, uint32_t mepc)
{
    (void)mcause;
    (void)mepc;
}

/* 实现 hal_now_us：返回假时钟。测试通过辅助接口显式推进它，
 * 保证每次断言都可复现。 */
uint64_t hal_now_us(void)
{
    return host_now_us;
}

/* 实现 hal_timer_arm_us：记录绝对截止时间。host 构建没有中断，
 * 到期判定留给测试直接对 host_now_us 做比较。 */
void hal_timer_arm_us(uint64_t deadline_us)
{
    host_timer_deadline_us = deadline_us;
}

/* 实现 hal_timer_disarm：把截止时间推到 UINT64_MAX 表示「永不触发」。 */
void hal_timer_disarm(void)
{
    host_timer_deadline_us = UINT64_MAX;
}

/* 实现 hal_timer_enable：host 没有全局中断控制器，空操作。 */
void hal_timer_enable(void)
{
}

/* 实现 hal_can_init：复位假 CAN 收发队列并标记已初始化，恒成功。 */
bool hal_can_init(void)
{
    host_can_initialized = true;
    host_can_rx_count = 0u;
    host_can_tx_count = 0u;
    return true;
}

/* 实现 hal_can_send：把帧追加到假发送队列（FIFO）。
 * 队列满或未初始化时返回 false——失败即拒绝，不丢帧、不伪造成功。 */
bool hal_can_send(const hal_can_frame *frame)
{
    if (!host_can_initialized || frame == 0 ||
        host_can_tx_count >= HAL_HOST_CAN_QUEUE_CAPACITY) {
        return false;
    }

    copy_can_frame(&host_can_tx[host_can_tx_count], frame);
    host_can_tx_count++;
    return true;
}

/* 实现 hal_can_recv：非阻塞地取出一帧。该假实现建模一组在接收方
 * 轮询之前已完成仲裁的帧：标准 ID 较小者胜出；ID 相等时保持插入
 * 顺序。这是确定性逻辑证据，不是物理总线时序。 */
bool hal_can_recv(hal_can_frame *frame)
{
    uint8_t selected = 0u;
    uint8_t index;

    if (!host_can_initialized || frame == 0 || host_can_rx_count == 0u) {
        return false;
    }

    /* 该假实现建模一组在接收方轮询之前已完成仲裁的帧。
     * 标准 ID 较小者胜出；ID 相等时保持插入顺序。
     * 这是确定性逻辑证据，不是物理总线时序。 */
    for (index = 1u; index < host_can_rx_count; index++) {
        if (host_can_rx[index].arbitration_id <
            host_can_rx[selected].arbitration_id) {
            selected = index;
        }
    }
    copy_can_frame(frame, &host_can_rx[selected]);
    /* 前移后续元素，维持队列紧凑。 */
    for (index = selected; index + 1u < host_can_rx_count; index++) {
        copy_can_frame(&host_can_rx[index], &host_can_rx[index + 1u]);
    }
    host_can_rx_count--;
    return true;
}

/* 测试辅助：把假 CAN 复位为未初始化、队列清空。 */
void hal_host_can_reset(void)
{
    host_can_initialized = false;
    host_can_rx_count = 0u;
    host_can_tx_count = 0u;
}

/* 测试辅助：向假接收队列注入一帧，模拟「轮询前总线已收到」的输入。
 * 队列满、未初始化或空指针时拒绝。 */
bool hal_host_can_inject_rx(const hal_can_frame *frame)
{
    if (!host_can_initialized || frame == 0 ||
        host_can_rx_count >= HAL_HOST_CAN_QUEUE_CAPACITY) {
        return false;
    }

    copy_can_frame(&host_can_rx[host_can_rx_count], frame);
    host_can_rx_count++;
    return true;
}

/* 测试辅助：从假发送队列队首取出一帧（FIFO），供断言交接内容。 */
bool hal_host_can_take_tx(hal_can_frame *frame)
{
    uint8_t index;

    if (!host_can_initialized || frame == 0 || host_can_tx_count == 0u) {
        return false;
    }

    copy_can_frame(frame, &host_can_tx[0]);
    for (index = 0u; index + 1u < host_can_tx_count; index++) {
        copy_can_frame(&host_can_tx[index], &host_can_tx[index + 1u]);
    }
    host_can_tx_count--;
    return true;
}

/* 测试辅助：当前假接收队列的待处理帧数。 */
uint8_t hal_host_can_rx_count(void)
{
    return host_can_rx_count;
}

/* 测试辅助：当前假发送队列的待取帧数。 */
uint8_t hal_host_can_tx_count(void)
{
    return host_can_tx_count;
}

/* 实现 hal_wdt_start：以假时钟启动软件建模的硬件看门狗。
 * 到期时间 = 当前假时间 + timeout_ms × 1000 微秒。 */
void hal_wdt_start(uint32_t timeout_ms)
{
    host_wdt_timeout_ms = timeout_ms;
    host_wdt_running = true;
    host_wdt_deadline_us = host_now_us + (uint64_t)timeout_ms * 1000u;
    host_wdt_feeds = 0u;
}

/* 实现 hal_wdt_feed：刷新到期时间并递增喂狗计数。
 * 看门狗未启动时是空操作。 */
void hal_wdt_feed(void)
{
    if (!host_wdt_running) {
        return;
    }
    host_wdt_feeds++;
    host_wdt_deadline_us = host_now_us + (uint64_t)host_wdt_timeout_ms * 1000u;
}

/* 实现 hal_wdt_feed_count：返回喂狗次数，供证据断言使用。 */
uint32_t hal_wdt_feed_count(void)
{
    return host_wdt_feeds;
}

/* 实现 hal_wdt_is_expired：以半区间规则判定假时钟是否越过
 * 到期时间；未启动恒为未到期。 */
bool hal_wdt_is_expired(void)
{
    return host_wdt_running && host_deadline_reached(host_now_us, host_wdt_deadline_us);
}
