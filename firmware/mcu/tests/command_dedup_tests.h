/* 命令去重测试套件的公共接口。
 * 被测契约见 command_dedup_tests.c 的文件横幅
 * （docs/architecture/mcu-command-dedup-v1.md）。
 */
#ifndef MCU_COMMAND_DEDUP_TESTS_H
#define MCU_COMMAND_DEDUP_TESTS_H

#include "state_machine_tests.h"

/* 运行去重套件：report 为 NULL 时直接返回。 */
void mcu_command_dedup_run_tests(mcu_test_report_t *report);

#endif /* MCU_COMMAND_DEDUP_TESTS_H */
