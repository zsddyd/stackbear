// main/sb_opus.c —— libopus 封装。编码器状态较大,只在成功时保留句柄。
#include "sb_opus.h"

#include "esp_log.h"
#include "opus.h"

static const char *TAG = "sb_opus";

static OpusEncoder *s_enc;
static int s_err;

esp_err_t sb_opus_init(void)
{
    if (s_enc != NULL) {
        return ESP_OK;
    }
    // VOIP 模式针对语音优化;单声道 16 kHz,60 ms 帧(960 采样)。
    s_enc = opus_encoder_create(SB_OPUS_RATE, 1, OPUS_APPLICATION_VOIP, &s_err);
    if (s_enc == NULL || s_err != OPUS_OK) {
        ESP_LOGE(TAG, "opus_encoder_create 失败: %d", s_err);
        s_enc = NULL;
        return ESP_FAIL;
    }
    // complexity 0:这颗无 PSRAM 的 C3 还要跑 WiFi + LVGL,编码不能抢占太多算力。
    opus_encoder_ctl(s_enc, OPUS_SET_COMPLEXITY(0));
    opus_encoder_ctl(s_enc, OPUS_SET_DTX(1));          // 静音不产生包,省流量
    opus_encoder_ctl(s_enc, OPUS_SET_INBAND_FEC(0));
    opus_encoder_ctl(s_enc, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
    opus_encoder_ctl(s_enc, OPUS_SET_BITRATE(16000));  // 约 16 kbps ≈ 2 KB/s
    ESP_LOGI(TAG, "Opus 编码器就绪 (%d Hz, %d ms 帧)", SB_OPUS_RATE, SB_OPUS_FRAME_MS);
    return ESP_OK;
}

void sb_opus_deinit(void)
{
    if (s_enc != NULL) {
        opus_encoder_destroy(s_enc);
        s_enc = NULL;
    }
}

int sb_opus_encode(const int16_t *pcm, uint8_t *out, size_t cap)
{
    if (s_enc == NULL || pcm == NULL || out == NULL) {
        return -1;
    }
    if (cap < 400) {  // opus 60ms 语音包上限远小于此
        return -1;
    }
    const int n = opus_encode(s_enc, pcm, SB_OPUS_FRAME_SAMPLES, out, (opus_int32)cap);
    // opus_encode 返回 0 表示 DTX 静音包;负数是错误。
    return n < 0 ? -1 : n;
}
