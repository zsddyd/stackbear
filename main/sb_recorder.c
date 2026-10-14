// main/sb_recorder.c —— 常开录音实现。
//
// 独立任务 + 静态栈:录音是长期驻留任务,动态建栈在反复启停时会制造不可回收的堆碎片。
// 采集以 20 ms 帧为粒度喂给 VAD;VAD 判定为语音的帧累计成 60 ms 的 Opus 帧后编码上传。
// 静音期间一个包都不发,这就是"常开但省电省流量"的关键。
#include "sb_recorder.h"

#include <string.h>

#include "bsp_audio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sb_diag.h"
#include "sb_adpcm.h"
#include "sb_uplink.h"
#include "sb_vad.h"

static const char *TAG = "sb_recorder";

#define VAD_FRAME_SAMPLES 320                                   // 20 ms @16k
#define VAD_FRAME_BYTES   (VAD_FRAME_SAMPLES * sizeof(int16_t))
#define FRAMES_PER_BLOCK (SB_AUDIO_FRAME_SAMPLES / VAD_FRAME_SAMPLES)  // 3 个 20ms 帧
// 静态栈:录音任务要调 bsp_audio_init()/esp_codec_dev_open 并跑 libopus 编码。
// 串口实测 24 KB 仍会在编码循环里触发 "Stack protection fault",这里加到 32 KB,
// 并在首个编码帧后打印高水位,便于按真实占用再收敛(不要继续靠猜)。
#define REC_TASK_STACK_WORDS 8192                               // 32 KiB

static StackType_t s_stack[REC_TASK_STACK_WORDS];
static StaticTask_t s_tcb;
static TaskHandle_t s_task;
static volatile bool s_running;
static volatile bool s_paused;
static volatile uint32_t s_segments;
static volatile uint32_t s_frames;

static void recorder_task(void *arg)
{
    (void)arg;
    static int16_t pcm[VAD_FRAME_SAMPLES];
    static int16_t codec_pcm[SB_AUDIO_FRAME_SAMPLES];
    static uint8_t enc_out[SB_ADPCM_MAX_BYTES];
    sb_adpcm_enc_t enc;
    sb_vad_t vad;
    sb_adpcm_init(&enc);
    sb_vad_init(&vad, NULL);

    int fill = 0;  // opus 帧内已累计的采样数
    if (bsp_audio_init() != ESP_OK) {
        ESP_LOGE(TAG, "音频初始化失败,录音任务退出");
        g_sb_diag.rec_err = SB_REC_ERR_AUDIO_INIT;
        s_running = false;
        vTaskDelete(NULL);
        return;
    }
    if (bsp_audio_set_format(SB_AUDIO_RATE, 16, 1) != ESP_OK) {
        ESP_LOGE(TAG, "音频格式设置失败");
        g_sb_diag.rec_err = SB_REC_ERR_AUDIO_FORMAT;
        s_running = false;
        vTaskDelete(NULL);
        return;
    }
    g_sb_diag.rec_err = SB_REC_OK;

    while (s_running) {
        if (s_paused) {
            // 暂停期间把 opus 帧内残留清零,恢复后从干净边界开始。
            fill = 0;
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        if (bsp_audio_read(pcm, VAD_FRAME_BYTES) != ESP_OK) {
            ESP_LOGW(TAG, "读取 PCM 失败");
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        const sb_vad_ev_t ev = sb_vad_process(&vad, pcm, VAD_FRAME_SAMPLES);
        if (ev == SB_VAD_SPEECH_END) {
            s_segments++;
        }
        // 只在语音段(含拖尾)内累计;纯静音帧直接丢,不占 opus 帧。
        if (ev == SB_VAD_SILENCE) {
            fill = 0;
            continue;
        }
        memcpy(&codec_pcm[fill * VAD_FRAME_SAMPLES], pcm, VAD_FRAME_BYTES);
        fill++;
        if (fill < FRAMES_PER_BLOCK) {
            continue;
        }
        fill = 0;
        const size_t n = sb_adpcm_encode_block(&enc, codec_pcm, SB_AUDIO_FRAME_SAMPLES,
                                               enc_out, sizeof(enc_out));
        if (n > 0) {
            sb_uplink_push_audio(enc_out, n);
            s_frames++;
            if (s_frames == 1) {
                // 打印真实栈高水位(md 小 = 用得少),据此调整 REC_TASK_STACK_WORDS。
                ESP_LOGI(TAG, "首帧已编码 %u 字节(ADPCM);栈高水位剩余 %u 字",
                         (unsigned)n, (unsigned)uxTaskGetStackHighWaterMark(NULL));
            }
        }
    }
    s_task = NULL;
    s_running = false;
    vTaskDelete(NULL);
}

esp_err_t sb_recorder_start(void)
{
    if (s_running) {
        return ESP_OK;
    }
    s_running = true;
    s_paused = false;
    s_task = xTaskCreateStatic(recorder_task, "sb_rec", REC_TASK_STACK_WORDS, NULL,
                               5, s_stack, &s_tcb);
    if (s_task == NULL) {
        s_running = false;
        g_sb_diag.rec_err = SB_REC_ERR_TASK;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "录音任务已启动");
    return ESP_OK;
}

void sb_recorder_stop(void)
{
    if (!s_running) {
        return;
    }
    s_running = false;
    for (int i = 0; i < 50 && s_task != NULL; i++) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

bool sb_recorder_running(void)
{
    return s_running;
}

void sb_recorder_set_paused(bool paused)
{
    s_paused = paused;
}

bool sb_recorder_paused(void)
{
    return s_paused;
}

uint32_t sb_recorder_segments(void)
{
    return s_segments;
}

uint32_t sb_recorder_frames(void)
{
    return s_frames;
}
