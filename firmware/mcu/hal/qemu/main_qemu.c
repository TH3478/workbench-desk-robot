/* QEMU 构建的入口点。任务 FW1/FW2 验收。
 *
 * 它位于 hal/qemu/ 而非 core/，原因有二：
 *   - 它读取 __stack_bottom/__stack_top，这些是 QEMU link.ld 符号
 *   - host 构建在 tests/ 中拥有自己的 main()，一次链接中出现两个
 *     main() 是错误
 *
 * core/ 保持不含入口点。这正是它能在全部三个目标上原样编译的原因。
 *
 * 这是冒烟测试，不是故障套件。它验证 FW1 与 FW2 真正关心的四件事：
 *   - 工具链能产出可启动的 rv32imac 镜像
 *   - crt0 设置好 sp 和 gp，并将 .bss 清零
 *   - UART 可用，后续测试才有上报途径
 *   - mtime 在走时，FW5 的看门狗才有可信的时钟
 */
#include "hal.h"
#include "can_bridge_tests.h"
#include "command_dedup_tests.h"
#include "frame_codec_tests.h"
#include "state_machine_tests.h"
#include "watchdog.h"
#include "watchdog_tests.h"

/* 有意保持未初始化：如果 crt0 跳过了 .bss 清零循环，这里就是垃圾数据，
 * 下面的检查便会失败。QEMU 恰好会分配清零的 RAM，因此本测试
 * 在该目标上可能因错误的原因通过——它在板子（FW17）上才真正发挥作用，
 * 因为那里 RAM 不会预先清零。
 */
static uint32_t bss_probe[4];

static mcu_state_machine_t timer_machine;
static mcu_watchdog_t timer_watchdog;
static volatile uint32_t timer_interrupts;
static volatile uint32_t timer_fault_records;
static volatile bool timer_record_available;
static mcu_watchdog_record_t timer_record;

static void copy_timer_record(const mcu_watchdog_record_t *source)
{
    timer_record.kind = source->kind;
    timer_record.fault = source->fault;
    timer_record.observed_at_us = source->observed_at_us;
    timer_record.deadline_us = source->deadline_us;
    timer_record.command_id = source->command_id;
    timer_record.retry_count = source->retry_count;
    timer_record.frame.kind = source->frame.kind;
    timer_record.frame.command_id = source->frame.command_id;
    timer_record.frame.sequence_no = source->frame.sequence_no;
    timer_record.frame.opcode = source->frame.opcode;
    timer_record.frame.retry_count = source->frame.retry_count;
    timer_record.frame.result_code = source->frame.result_code;
    timer_record.frame.fault_code = source->frame.fault_code;
    timer_record.frame.device_mode = source->frame.device_mode;
}

/* 由 crt0.S 中的机器定时器路径调用。它有意保持极简：这里运行与 Host
 * 相同的零分配轮询，随后是 HAL 自有的硬件看门狗喂狗，
 * 以及下一次单次定时器的武装。 */
void mcu_qemu_timer_interrupt(void)
{
    mcu_watchdog_record_t record;
    uint64_t now_us = hal_now_us();

    timer_interrupts++;
    if (mcu_watchdog_poll(&timer_watchdog, &timer_machine, now_us, &record)) {
        copy_timer_record(&record);
        timer_record_available = true;
        timer_fault_records++;
    }
    if (mcu_watchdog_should_feed_hardware(&timer_watchdog, &timer_machine)) {
        hal_wdt_feed();
    }
    hal_timer_arm_us(now_us + MCU_HEARTBEAT_PERIOD_US);
}

static int check_bss_zeroed(void)
{
    for (unsigned i = 0; i < 4; i++) {
        if (bss_probe[i] != 0) {
            hal_puts("[mcu] .bss not zeroed at index ");
            hal_put_u32(i);
            hal_putc('\n');
            return 1;
        }
    }
    hal_puts("[mcu] ok   .bss zeroed\n");
    return 0;
}

static int check_clock_advances(void)
{
    uint64_t t0 = hal_now_us();

    /* 自旋而非休眠：这里没有调度器。volatile 防止 -Os 优化
     * 把循环删掉。 */
    for (volatile uint32_t i = 0; i < 200000; i++) { }

    uint64_t t1 = hal_now_us();
    if (t1 <= t0) {
        hal_puts("[mcu] clock did not advance\n");
        return 2;
    }
    hal_puts("[mcu] ok   clock advanced ");
    hal_put_u32((uint32_t)(t1 - t0));
    hal_puts(" us\n");
    return 0;
}

static int check_stack_sane(void)
{
    /* sp 应位于链接器保留的区域之内。此处的越界偏差要到很久之后
     * 才会表现为内存损坏，所以要趁还能打印时尽早检查。 */
    extern char __stack_bottom[], __stack_top[];
    uintptr_t sp;
    __asm__ volatile("mv %0, sp" : "=r"(sp));

    if (sp <= (uintptr_t)__stack_bottom || sp > (uintptr_t)__stack_top) {
        hal_puts("[mcu] sp outside the linked stack region\n");
        return 3;
    }
    hal_puts("[mcu] ok   sp inside .stack, headroom ");
    hal_put_u32((uint32_t)(sp - (uintptr_t)__stack_bottom));
    hal_puts(" bytes\n");
    return 0;
}

static int run_state_machine_tests(void)
{
    mcu_test_report_t report;

    mcu_state_machine_run_tests(&report);
    hal_puts("[mcu] state-machine assertions=");
    hal_put_u32(report.assertions);
    hal_puts(" failures=");
    hal_put_u32(report.failures);
    hal_puts(" first_failure=");
    hal_put_u32(report.first_failure);
    hal_putc('\n');
    return report.failures == 0u ? 0 : 4;
}

static int run_frame_codec_tests(void)
{
    mcu_test_report_t report;

    mcu_frame_codec_run_tests(&report);
    hal_puts("[mcu] frame-codec assertions=");
    hal_put_u32(report.assertions);
    hal_puts(" failures=");
    hal_put_u32(report.failures);
    hal_puts(" first_failure=");
    hal_put_u32(report.first_failure);
    hal_putc('\n');
    return report.failures == 0u ? 0 : 5;
}

static int run_watchdog_tests(void)
{
    mcu_test_report_t report;

    mcu_watchdog_run_tests(&report);
    hal_puts("[mcu] watchdog assertions=");
    hal_put_u32(report.assertions);
    hal_puts(" failures=");
    hal_put_u32(report.failures);
    hal_puts(" first_failure=");
    hal_put_u32(report.first_failure);
    hal_putc('\n');
    return report.failures == 0u ? 0 : 6;
}

static int run_command_dedup_tests(void)
{
    mcu_test_report_t report;

    mcu_command_dedup_run_tests(&report);
    hal_puts("[mcu] command-dedup assertions=");
    hal_put_u32(report.assertions);
    hal_puts(" failures=");
    hal_put_u32(report.failures);
    hal_puts(" first_failure=");
    hal_put_u32(report.first_failure);
    hal_putc('\n');
    return report.failures == 0u ? 0 : 10;
}

static int run_can_bridge_tests(void)
{
    mcu_test_report_t report;

    mcu_can_bridge_run_tests(&report);
    hal_puts("[mcu] can-bridge assertions=");
    hal_put_u32(report.assertions);
    hal_puts(" failures=");
    hal_put_u32(report.failures);
    hal_puts(" first_failure=");
    hal_put_u32(report.first_failure);
    hal_putc('\n');
    return report.failures == 0u ? 0 : 11;
}

static int run_qemu_timing_evidence(void)
{
    mcu_event_t begin_move;
    mcu_transition_result_t transition;
    uint64_t start_us;

    mcu_sm_init(&timer_machine);
    mcu_watchdog_init(&timer_watchdog, 100u);
    begin_move.kind = MCU_EVENT_BEGIN_MOVE;
    begin_move.fault_code = MCU_FAULT_NONE;
    begin_move.reset_authorized = false;
    begin_move.cause_cleared = false;
    mcu_sm_dispatch(&timer_machine, &begin_move, &transition);
    if (transition.result_code != MCU_RESULT_ACCEPTED) {
        hal_puts("[mcu] watchdog demo could not enter executing\n");
        return 7;
    }

    start_us = hal_now_us();
    if (!mcu_watchdog_note_activity(&timer_watchdog,
                                    &timer_machine,
                                    MCU_WATCHDOG_ACTIVITY_VALID_NEW,
                                    start_us)) {
        hal_puts("[mcu] watchdog demo could not arm link timeout\n");
        return 8;
    }
    hal_wdt_start(MCU_HARDWARE_WATCHDOG_PERIOD_MS);
    timer_interrupts = 0u;
    timer_fault_records = 0u;
    timer_record_available = false;
    hal_timer_arm_us(start_us + MCU_HEARTBEAT_PERIOD_US);
    hal_timer_enable();

    /* 三个定时器周期即可到达软件截止时间。第四个周期证明：
     * 故障发生后定时器仍持续运行，而 core 停止喂
     * 建模的硬件看门狗。 */
    while (timer_interrupts < 4u && !hal_wdt_is_expired()) {
        __asm__ volatile("wfi");
    }
    if (timer_interrupts < 3u || timer_fault_records != 1u || !timer_record_available ||
        timer_record.kind != MCU_WATCHDOG_RECORD_FAULT_TELEMETRY ||
        timer_machine.state != MCU_STATE_FAULT || hal_wdt_feed_count() == 0u) {
        hal_timer_disarm();
        hal_puts("[mcu] QEMU timer/watchdog evidence failed\n");
        return 9;
    }

    while (!hal_wdt_is_expired()) {
        __asm__ volatile("wfi");
    }
    hal_timer_disarm();
    hal_puts("[mcu] QEMU timer interrupts=");
    hal_put_u32(timer_interrupts);
    hal_puts(" fault_records=");
    hal_put_u32(timer_fault_records);
    hal_puts(" wdt_feeds=");
    hal_put_u32(hal_wdt_feed_count());
    hal_puts(" wdt_expired=1\n");
    return 0;
}

int main(void)
{
    hal_puts("\n[mcu] FW1/FW2 smoke test\n");

    int rc = 0;
    rc |= check_bss_zeroed();
    rc |= check_clock_advances();
    rc |= check_stack_sane();
    rc |= run_state_machine_tests();
    rc |= run_frame_codec_tests();
    rc |= run_watchdog_tests();
    rc |= run_command_dedup_tests();
    rc |= run_can_bridge_tests();
    rc |= run_qemu_timing_evidence();

    /* 平台无关的 HAL/Wire 边界已在上方覆盖。QEMU 的 CAN 传输明确保持
     * NOT_EXECUTED，因为这些 HAL 函数仍是返回 false 的桩。 */
    hal_puts("[mcu] CAN transport NOT_EXECUTED - QEMU HAL is a stub\n");

    return rc;
}
