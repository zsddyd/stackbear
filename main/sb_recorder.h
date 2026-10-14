// main/sb_recorder.h —— 常开录音任务:采集 -> VAD 分段 -> Opus 编码 -> 交给上传。
#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

// 启动录音任务(静态栈,避免无 PSRAM 机器上反复分配导致堆碎片)。
esp_err_t sb_recorder_start(void);
void sb_recorder_stop(void);

bool sb_recorder_running(void);
void sb_recorder_set_paused(bool paused);
bool sb_recorder_paused(void);

// 统计:累计判定出的语音段数、已编码发出的 Opus 帧数。
uint32_t sb_recorder_segments(void);
uint32_t sb_recorder_frames(void);
