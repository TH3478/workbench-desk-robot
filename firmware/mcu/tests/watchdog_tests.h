/* 看门狗与 STOP 时序测试套件的公共接口。
 * 被测契约见 watchdog_tests.c 的文件横幅
 * （docs/architecture/mcu-watchdog-v1.md）。
 */
#ifndef MCU_WATCHDOG_TESTS_H
#define MCU_WATCHDOG_TESTS_H

#include "state_machine_tests.h"

/* 运行看门狗套件：report 为 NULL 时直接返回。 */
void mcu_watchdog_run_tests(mcu_test_report_t *report);

#endif /* MCU_WATCHDOG_TESTS_H */
