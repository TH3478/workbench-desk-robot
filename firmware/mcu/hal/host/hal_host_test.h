/* Host HAL 的测试辅助接口。任务 FW1。
 *
 * 职责：向 tests/ 暴露 x86_64 假 CAN 的注入/取出与计数能力，
 * 让 can_bridge_host_tests.c 能对 hal/host/hal_host.c 的固定测试
 * 队列做确定性断言。这些接口只存在于 host 构建，QEMU 与板卡
 * 目标没有等价物——它们既不是板载 CAN 驱动，也不构成物理送达
 * 的证据（docs/architecture/mcu-can-hal-boundary-v1.md 的
 * 传输交接证据边界）。
 */
#ifndef MCU_HAL_HOST_TEST_H
#define MCU_HAL_HOST_TEST_H

#include <stdbool.h>
#include <stdint.h>

#include "hal.h"

/* x86_64 假 HAL 专用的固定测试队列。它们建模受限的
 * 传输交接与确定性仲裁；既不是板载 CAN 驱动，
 * 也不构成物理送达的证据。 */
#define HAL_HOST_CAN_QUEUE_CAPACITY 16u

/* 复位假 CAN（标记未初始化并清空收发队列）。 */
void hal_host_can_reset(void);
/* 向假接收队列注入一帧，失败即拒绝（队列满/未初始化）。 */
bool hal_host_can_inject_rx(const hal_can_frame *frame);
/* 从假发送队列队首取出一帧（FIFO），供交接断言。 */
bool hal_host_can_take_tx(hal_can_frame *frame);
/* 待处理接收帧数。 */
uint8_t hal_host_can_rx_count(void);
/* 待取发送帧数。 */
uint8_t hal_host_can_tx_count(void);

#endif /* MCU_HAL_HOST_TEST_H */
