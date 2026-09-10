/* Wire V1 帧编解码器测试套件的公共接口。
 * 被测契约见 frame_codec_tests.c 的文件横幅
 * （docs/architecture/mcu-wire-v1.md）。
 */
#ifndef MCU_FRAME_CODEC_TESTS_H
#define MCU_FRAME_CODEC_TESTS_H

#include "state_machine_tests.h"

/* 运行编解码器套件：report 为 NULL 时直接返回。 */
void mcu_frame_codec_run_tests(mcu_test_report_t *report);

#endif /* MCU_FRAME_CODEC_TESTS_H */
