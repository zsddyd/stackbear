// main/sb_adpcm.h —— IMA-ADPCM 4bit 编码(替代 Opus)。
//
// 为什么换掉 Opus:这颗无 PSRAM 的 ESP32-C3 同时要跑 WiFi + LVGL,libopus 的编码器
// 既需要约 25KB 常驻状态,又把 CELT/SILK 的临时缓冲区 alloca 到任务栈(几十 KB),
// 在这台设备上反复导致 ESP_ERR_NO_MEM 与 Stack protection fault。IMA-ADPCM 的编码
// 状态只有几个字节、没有大 scratch,压缩比约 4:1(16kHz 单声道 ≈ 7.8 KB/s),
// 对"录音留档后转文字"这个用途足够。
//
// 块格式(每个 60ms 帧一个自包含块,便于丢块后仍能解码):
//   [0..1] 初始预测值 predictor(int16, 小端)
//   [2]    初始步长索引 index(uint8, 0..88)
//   [3]    保留 0
//   [4..]  4bit 差分,每字节两个采样,【低半字节是先出现的采样】
// 每块解码互不依赖,所以流里丢一个块只影响那 60ms。
#pragma once

#include <stddef.h>
#include <stdint.h>

#define SB_AUDIO_RATE          16000
#define SB_AUDIO_FRAME_MS      60
#define SB_AUDIO_FRAME_SAMPLES ((SB_AUDIO_RATE / 1000) * SB_AUDIO_FRAME_MS)  // 960

#define SB_ADPCM_HEADER_LEN   4
#define SB_ADPCM_BLOCK_BYTES  (SB_ADPCM_HEADER_LEN + SB_AUDIO_FRAME_SAMPLES / 2)  // 484
#define SB_ADPCM_MAX_BYTES    SB_ADPCM_BLOCK_BYTES

typedef struct {
    int32_t predictor;   // 当前重建值
    int32_t index;       // 步长表索引 0..88
} sb_adpcm_enc_t;

// 无动态内存、无大栈帧:编码器只有这两个字段。
void sb_adpcm_init(sb_adpcm_enc_t *enc);

// 编码一个块。out 需 >= SB_ADPCM_BLOCK_BYTES。samples 必须是偶数且在
// [1, SB_AUDIO_FRAME_SAMPLES] 内。返回写入字节数;参数非法返回 0。
size_t sb_adpcm_encode_block(sb_adpcm_enc_t *enc, const int16_t *pcm,
                             size_t samples, uint8_t *out, size_t cap);

// 解码一个块(与上面格式对称),主要供主机测试与云端参考实现对照。
// 返回解码出的采样数;格式非法返回 0。
size_t sb_adpcm_decode_block(const uint8_t *in, size_t len, int16_t *pcm,
                             size_t max_samples);
