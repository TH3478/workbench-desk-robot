/* CAN 桥（原始 HAL/Wire V1 包络映射）测试套件的公共接口。
 * 被测契约见 can_bridge_tests.c 的文件横幅
 * （docs/architecture/mcu-can-hal-boundary-v1.md）。
 */
#ifndef MCU_CAN_BRIDGE_TESTS_H
#define MCU_CAN_BRIDGE_TESTS_H

#include "state_machine_tests.h"

/* 运行 CAN 桥套件：report 为 NULL 时直接返回。 */
void mcu_can_bridge_run_tests(mcu_test_report_t *report);

#endif /* MCU_CAN_BRIDGE_TESTS_H */
