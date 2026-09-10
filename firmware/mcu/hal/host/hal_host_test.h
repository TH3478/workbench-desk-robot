#ifndef MCU_HAL_HOST_TEST_H
#define MCU_HAL_HOST_TEST_H

#include <stdbool.h>
#include <stdint.h>

#include "hal.h"

/* x86_64 假 HAL 专用的固定测试队列。它们建模受限的
 * 传输交接与确定性仲裁；既不是板载 CAN 驱动，
 * 也不构成物理送达的证据。 */
#define HAL_HOST_CAN_QUEUE_CAPACITY 16u

void hal_host_can_reset(void);
bool hal_host_can_inject_rx(const hal_can_frame *frame);
bool hal_host_can_take_tx(hal_can_frame *frame);
uint8_t hal_host_can_rx_count(void);
uint8_t hal_host_can_tx_count(void);

#endif /* MCU_HAL_HOST_TEST_H */
