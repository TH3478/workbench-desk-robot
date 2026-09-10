/* 状态机测试套件的公共接口。对应 Issue #53 的 C 安全状态机。
 *
 * 定义共享测试报告结构 mcu_test_report_t 与套件入口
 * mcu_state_machine_run_tests；其余套件头文件都包含本头以
 * 复用报告结构（被测契约见 state_machine_tests.c 的文件横幅）。
 */
#ifndef MCU_STATE_MACHINE_TESTS_H
#define MCU_STATE_MACHINE_TESTS_H

#include <stdint.h>

/* 共享测试报告：断言总数、失败总数与首个失败断言序号。
 * Host 与 QEMU 的验收都断言 failures == 0。 */
typedef struct {
    uint32_t assertions;
    uint32_t failures;
    uint32_t first_failure;
} mcu_test_report_t;

/* 运行状态机套件：report 为 NULL 时直接返回（不计数）。 */
void mcu_state_machine_run_tests(mcu_test_report_t *report);

#endif /* MCU_STATE_MACHINE_TESTS_H */
