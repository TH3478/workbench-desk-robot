/* Startup for the QEMU virt machine. Task FW2.
 *
 * Order matters and each step has a reason:
 *   1. sp before anything else — a C call with a garbage sp corrupts memory
 *      silently rather than trapping.
 *   2. .bss zeroed before main — C guarantees statics start at zero. QEMU
 *      happens to hand us zeroed RAM; the real board does not.
 *   3. mtvec set before enabling anything that can trap, so a fault lands in
 *      our handler instead of looping at the reset vector.
 */

    .section .text.entry, "ax"
    .global _start
    .type _start, @function

/* 启动向量 _start：QEMU 以 -bios none -kernel 加载 ELF 后直接跳转到
 * ORIGIN（0x80000000），link.ld 用 KEEP(*(.text.entry)) 保证本段落在
 * 镜像最前。完整流程：关中断 → 设置 sp/gp/fp → 清零 .bss → 安装
 * mtvec → 画栈哨兵 → call main → hal_report_exit → 停机。
 */
_start:
    /* Disable interrupts until the vector table and stack are valid. */
    csrw    mie, zero
    csrw    mip, zero

    /* 1. Stack pointer. */
    /* 栈顶来自 link.ld 的 __stack_top；栈向下生长，main_qemu.c 的
     * check_stack_sane 会验证 sp 仍在该区间内。 */
    la      sp, __stack_top

    /* Frame pointer and global pointer: gp is used for .sdata addressing.
       .option norelax stops the assembler rewriting these into gp-relative
       loads before gp itself is set up. */
.option push
.option norelax
    la      gp, __global_pointer$
.option pop
    mv      fp, sp

    /* 2. Zero .bss. */
    /* 按 4 字节步长把 __bss_start..__bss_end 清零：C 要求静态变量
     * 以零启动；空区间（相等）直接跳过循环。 */
    la      a0, __bss_start
    la      a1, __bss_end
    beq     a0, a1, bss_done
bss_loop:
    sw      zero, 0(a0)
    addi    a0, a0, 4
    bltu    a0, a1, bss_loop
bss_done:

    /* .data is already in place: on this machine the ELF loads straight into
       RAM, so there is no flash-to-RAM copy. The CH32V307 crt0 (FW17) will
       need one, which is another reason crt0 is per-HAL. */

    /* 3. Trap vector, direct mode (low two bits 0). */
    /* 直接模式：mtvec 低 2 位为 0，陷阱一律跳进 trap_entry。 */
    la      t0, trap_entry
    csrw    mtvec, t0

    /* Paint the stack so FW9 can measure high-water mark by looking for the
       last untouched word. Cheap, and turns "probably fine" into a number. */
    la      t0, __stack_bottom
    la      t1, __stack_top
    li      t2, 0xC0FFEE00
paint_loop:
    sw      t2, 0(t0)
    addi    t0, t0, 4
    bltu    t0, t1, paint_loop

    call    main

    /* main returned: a0 holds the exit code. Report it, then halt. */
    /* 退出处理：a0 携带 main 的退出码，交给 hal_report_exit 打印并
     * 写 SiFive 测试终结器，随后 wfi 自旋停机——绝不返回。 */
    call    hal_report_exit

halt:
    wfi
    j       halt

    .size _start, . - _start


/* Trap handler. Anything arriving here in a safety MCU is a bug, so it reports
 * and halts rather than trying to continue. FW2's acceptance test is that an
 * illegal instruction actually lands here.
 */
/* 陷阱入口（mtvec 指向这里）。mcause 为机器定时器中断（最高位 1
 * 表示中断，原因码 7）时走定时器路径；其余一律按「不变量已破」
 * 报告 mcause/mepc 并停机。 */
    .align 4
    .global trap_entry
    .type trap_entry, @function
trap_entry:
    /* Preserve the caller-saved registers before inspecting mcause. In
     * particular, t0/t1 are used for dispatch and must still be restored to
     * their pre-interrupt values on the timer-return path. */
    /* 保存全部调用者保存寄存器（ra/t0..t6/a0..a7，共 16 × 4 字节），
     * 使定时器中断返回时上下文完整恢复。 */
    addi    sp, sp, -64
    sw      ra, 0(sp)
    sw      t0, 4(sp)
    sw      t1, 8(sp)
    sw      t2, 12(sp)
    sw      t3, 16(sp)
    sw      t4, 20(sp)
    sw      t5, 24(sp)
    sw      t6, 28(sp)
    sw      a0, 32(sp)
    sw      a1, 36(sp)
    sw      a2, 40(sp)
    sw      a3, 44(sp)
    sw      a4, 48(sp)
    sw      a5, 52(sp)
    sw      a6, 56(sp)
    sw      a7, 60(sp)

    /* 读取 mcause：0x80000007 = 机器定时器中断（第 31 位中断标志 +
     * 原因码 7）。命中则调用 hal_qemu.c 的定时器回调后 mret 返回；
     * 不命中则落入 unexpected_trap。 */
    csrr    t0, mcause
    li      t1, 0x80000007
    bne     t0, t1, unexpected_trap
    call    mcu_qemu_timer_interrupt
    lw      ra, 0(sp)
    lw      t0, 4(sp)
    lw      t1, 8(sp)
    lw      t2, 12(sp)
    lw      t3, 16(sp)
    lw      t4, 20(sp)
    lw      t5, 24(sp)
    lw      t6, 28(sp)
    lw      a0, 32(sp)
    lw      a1, 36(sp)
    lw      a2, 40(sp)
    lw      a3, 44(sp)
    lw      a4, 48(sp)
    lw      a5, 52(sp)
    lw      a6, 56(sp)
    lw      a7, 60(sp)
    addi    sp, sp, 64
    mret

unexpected_trap:
    /* 非定时器陷阱：把 mcause、mepc 传给 hal_report_trap，
     * 由它打印并以失败码写测试终结器。 */
    csrr    a0, mcause
    csrr    a1, mepc
    call    hal_report_trap

    /* No return. A trap means an invariant already broke. */
trap_halt:
    wfi
    j       trap_halt

    .size trap_entry, . - trap_entry
