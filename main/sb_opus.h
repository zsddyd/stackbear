// main/sb_opus.h —— libopus 编码封装(16 kHz 单声道 VOIP)。
#pragma once

#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>

#define SB_OPUS_RATE      16000
#define SB_OPUS_FRAME_MS  60
#define SB_OPUS_FRAME_SAMPLES ((SB_OPUS_RATE / 1000) * SB_OPUS_FRAME_MS)  // 960

// 创建编码器(complexity 0,开启 DTX 省流量)。可重复调用(已创建则复用)。
esp_err_t sb_opus_init(void);
void sb_opus_deinit(void);

// 编码一帧 SB_OPUS_FRAME_SAMPLES 个 int16 采样到 out(cap 字节)。
// 返回编码字节数(>0);DTX 判定为静音包返回 0;失败返回 -1。
int sb_opus_encode(const int16_t *pcm, uint8_t *out, size_t cap);
