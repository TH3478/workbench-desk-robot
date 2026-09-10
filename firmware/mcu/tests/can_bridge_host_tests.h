/* CAN 桥宿主测试套件（假 HAL 传输路径）的公共接口。
 * 被测契约见 can_bridge_host_tests.c 的文件横幅
 * （docs/architecture/mcu-can-hal-boundary-v1.md 的传输交接部分）。
 * 仅 Host 构建使用本入口。
 */
#ifndef MCU_CAN_BRIDGE_HOST_TESTS_H
#define MCU_CAN_BRIDGE_HOST_TESTS_H

#include "state_machine_tests.h"

/* 运行宿主套件：report 为 NULL 时直接返回。 */
void mcu_can_bridge_host_run_tests(mcu_test_report_t *report);

#endif /* MCU_CAN_BRIDGE_HOST_TESTS_H */
