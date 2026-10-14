// tests/test_sb_vad.c —— sb_vad 常开 VAD 的主机单元测试(不依赖 ESP-IDF)。
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sb_vad.h"

static int g_failures;

#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);         \
            g_failures++;                                                  \
        }                                                                  \
    } while (0)

#define FRAME_SAMPLES 320  // 20 ms @ 16 kHz

static void fill(int16_t *buf, size_t n, int16_t v)
{
    for (size_t i = 0; i < n; i++) {
        buf[i] = v;
    }
}

static void fill_alternating(int16_t *buf, size_t n, int16_t amp)
{
    for (size_t i = 0; i < n; i++) {
        buf[i] = (i & 1) ? amp : (int16_t)(-amp);
    }
}

static void test_rms(void)
{
    int16_t buf[FRAME_SAMPLES];
    fill(buf, FRAME_SAMPLES, 0);
    CHECK(sb_vad_rms(buf, FRAME_SAMPLES) == 0);
    fill(buf, FRAME_SAMPLES, 100);
    CHECK(sb_vad_rms(buf, FRAME_SAMPLES) == 100);
    fill(buf, FRAME_SAMPLES, -100);
    CHECK(sb_vad_rms(buf, FRAME_SAMPLES) == 100);
    // 交替 +-1000 的 RMS 就是 1000
    fill_alternating(buf, FRAME_SAMPLES, 1000);
    const uint32_t r = sb_vad_rms(buf, FRAME_SAMPLES);
    CHECK(r >= 999 && r <= 1001);
    CHECK(sb_vad_rms(NULL, 0) == 0);
}

static void test_silence_then_speech_then_end(void)
{
    sb_vad_t vad;
    sb_vad_cfg_t cfg;
    sb_vad_default_cfg(&cfg);
    cfg.hangover_frames = 4;
    cfg.min_speech_frames = 3;
    sb_vad_init(&vad, &cfg);

    int16_t quiet[FRAME_SAMPLES];
    int16_t loud[FRAME_SAMPLES];
    fill(quiet, FRAME_SAMPLES, 5);       // 极低底噪
    fill_alternating(loud, FRAME_SAMPLES, 3000);

    // 最初若干静音帧
    for (int i = 0; i < 10; i++) {
        CHECK(sb_vad_process(&vad, quiet, FRAME_SAMPLES) == SB_VAD_SILENCE);
    }
    // 连续 5 帧大声 -> 一直 SPEECH
    for (int i = 0; i < 5; i++) {
        CHECK(sb_vad_process(&vad, loud, FRAME_SAMPLES) == SB_VAD_SPEECH);
    }
    // 转为静音:拖尾期内仍是 SPEECH
    for (int i = 0; i < 4; i++) {
        CHECK(sb_vad_process(&vad, quiet, FRAME_SAMPLES) == SB_VAD_SPEECH);
    }
    // 拖尾耗尽:封段
    CHECK(sb_vad_process(&vad, quiet, FRAME_SAMPLES) == SB_VAD_SPEECH_END);
    // 之后回到静音
    CHECK(sb_vad_process(&vad, quiet, FRAME_SAMPLES) == SB_VAD_SILENCE);
}

static void test_short_blip_is_rejected(void)
{
    sb_vad_t vad;
    sb_vad_cfg_t cfg;
    sb_vad_default_cfg(&cfg);
    cfg.hangover_frames = 3;
    cfg.min_speech_frames = 10;  // 要求至少 10 帧才算一句话
    sb_vad_init(&vad, &cfg);

    int16_t quiet[FRAME_SAMPLES];
    int16_t loud[FRAME_SAMPLES];
    fill(quiet, FRAME_SAMPLES, 5);
    fill_alternating(loud, FRAME_SAMPLES, 4000);

    for (int i = 0; i < 20; i++) {
        sb_vad_process(&vad, quiet, FRAME_SAMPLES);
    }
    // 只说了 2 帧
    CHECK(sb_vad_process(&vad, loud, FRAME_SAMPLES) == SB_VAD_SPEECH);
    CHECK(sb_vad_process(&vad, loud, FRAME_SAMPLES) == SB_VAD_SPEECH);
    // 静音拖尾后不应产生 SPEECH_END,而是回到静音
    sb_vad_ev_t ev = SB_VAD_SILENCE;
    for (int i = 0; i < 8; i++) {
        ev = sb_vad_process(&vad, quiet, FRAME_SAMPLES);
    }
    CHECK(ev == SB_VAD_SILENCE);
}

static void test_gap_shorter_than_hangover_keeps_segment(void)
{
    sb_vad_t vad;
    sb_vad_cfg_t cfg;
    sb_vad_default_cfg(&cfg);
    cfg.hangover_frames = 10;
    cfg.min_speech_frames = 2;
    sb_vad_init(&vad, &cfg);

    int16_t quiet[FRAME_SAMPLES];
    int16_t loud[FRAME_SAMPLES];
    fill(quiet, FRAME_SAMPLES, 5);
    fill_alternating(loud, FRAME_SAMPLES, 3000);

    for (int i = 0; i < 10; i++) {
        sb_vad_process(&vad, quiet, FRAME_SAMPLES);
    }
    // 说话 -> 3 帧停顿(小于 10 帧拖尾) -> 又说话,应始终处于同一段
    for (int i = 0; i < 4; i++) {
        CHECK(sb_vad_process(&vad, loud, FRAME_SAMPLES) == SB_VAD_SPEECH);
    }
    for (int i = 0; i < 3; i++) {
        CHECK(sb_vad_process(&vad, quiet, FRAME_SAMPLES) == SB_VAD_SPEECH);
    }
    for (int i = 0; i < 4; i++) {
        CHECK(sb_vad_process(&vad, loud, FRAME_SAMPLES) == SB_VAD_SPEECH);
    }
    CHECK(vad.in_speech == 1);
}

int main(void)
{
    test_rms();
    test_silence_then_speech_then_end();
    test_short_blip_is_rejected();
    test_gap_shorter_than_hangover_keeps_segment();
    if (g_failures == 0) {
        printf("test_sb_vad: PASS\n");
        return 0;
    }
    printf("test_sb_vad: %d failure(s)\n", g_failures);
    return 1;
}
