// tests/test_sb_adpcm.c —— IMA-ADPCM 编解码往返测试(纯逻辑,主机可跑)。
#include <stdio.h>
#include <stdlib.h>

#include "sb_adpcm.h"

static int g_failures;

#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);         \
            g_failures++;                                                  \
        }                                                                  \
    } while (0)

// 一个 16kHz 单声道 60ms 的正弦波(约 440Hz),用来验证 ADPCM 能跟随信号。
static void fill_sine(int16_t *buf, size_t n, int amp)
{
    for (size_t i = 0; i < n; i++) {
        // 用整数近似 sin:三角波足够体现"跟随能力",且完全确定。
        const int period = 36;  // 16000/440 ≈ 36 采样/周期
        const int phase = (int)(i % (size_t)period);
        const int half = period / 2;
        const int tri = phase < half ? phase : period - phase;
        buf[i] = (int16_t)((tri * 4 * amp) / period - amp);
    }
}

static void test_block_size_and_header(void)
{
    int16_t pcm[SB_AUDIO_FRAME_SAMPLES];
    uint8_t out[SB_ADPCM_MAX_BYTES];
    sb_adpcm_enc_t enc;
    sb_adpcm_init(&enc);
    fill_sine(pcm, SB_AUDIO_FRAME_SAMPLES, 8000);

    const size_t n = sb_adpcm_encode_block(&enc, pcm, SB_AUDIO_FRAME_SAMPLES, out, sizeof(out));
    CHECK(n == SB_ADPCM_BLOCK_BYTES);
    CHECK(n == 4 + SB_AUDIO_FRAME_SAMPLES / 2);
    // 头部存的是第一采样的小端与初始 index=0。
    const int16_t pred = (int16_t)((uint16_t)out[0] | ((uint16_t)out[1] << 8));
    CHECK(pred == pcm[0]);
    CHECK(out[2] == 0);
    CHECK(out[3] == 0);
    // 压缩比:960 采样(1920 字节) -> 484 字节,略高于 4:1(含 4 字节块头)。
    CHECK(n * 4 <= SB_AUDIO_FRAME_SAMPLES * 2 + 32);
}

static void test_roundtrip_error_is_bounded(void)
{
    int16_t pcm[SB_AUDIO_FRAME_SAMPLES];
    int16_t dec[SB_AUDIO_FRAME_SAMPLES];
    uint8_t out[SB_ADPCM_MAX_BYTES];
    sb_adpcm_enc_t enc;
    sb_adpcm_init(&enc);
    fill_sine(pcm, SB_AUDIO_FRAME_SAMPLES, 8000);

    // 连编 4 块,只看最后一块(步长索引已经自适应到稳态),这才是实际质量。
    int max_err = 0;
    for (int block = 0; block < 4; block++) {
        fill_sine(pcm, SB_AUDIO_FRAME_SAMPLES, 8000);
        const size_t n = sb_adpcm_encode_block(&enc, pcm, SB_AUDIO_FRAME_SAMPLES,
                                               out, sizeof(out));
        CHECK(n == SB_ADPCM_BLOCK_BYTES);
        const size_t got = sb_adpcm_decode_block(out, n, dec, SB_AUDIO_FRAME_SAMPLES);
        CHECK(got == SB_AUDIO_FRAME_SAMPLES);
        if (block == 3) {
            for (size_t i = 0; i < SB_AUDIO_FRAME_SAMPLES; i++) {
                const int err = abs((int)pcm[i] - (int)dec[i]);
                if (err > max_err) {
                    max_err = err;
                }
            }
        }
    }
    // 有损编解码,稳态最大误差应远小于信号幅度(8000)。
    CHECK(max_err < 1500);
}

static void test_silence_roundtrip_is_exact(void)
{
    int16_t pcm[SB_AUDIO_FRAME_SAMPLES];
    int16_t dec[SB_AUDIO_FRAME_SAMPLES];
    uint8_t out[SB_ADPCM_MAX_BYTES];
    sb_adpcm_enc_t enc;
    sb_adpcm_init(&enc);
    for (size_t i = 0; i < SB_AUDIO_FRAME_SAMPLES; i++) {
        pcm[i] = 0;
    }
    const size_t n = sb_adpcm_encode_block(&enc, pcm, SB_AUDIO_FRAME_SAMPLES, out, sizeof(out));
    sb_adpcm_decode_block(out, n, dec, SB_AUDIO_FRAME_SAMPLES);
    for (size_t i = 0; i < SB_AUDIO_FRAME_SAMPLES; i++) {
        CHECK(dec[i] == 0);
    }
}

static void test_rejects_bad_args(void)
{
    uint8_t out[SB_ADPCM_MAX_BYTES];
    int16_t pcm[4] = {0};
    sb_adpcm_enc_t enc;
    sb_adpcm_init(&enc);
    CHECK(sb_adpcm_encode_block(&enc, pcm, 0, out, sizeof(out)) == 0);
    CHECK(sb_adpcm_encode_block(&enc, pcm, 3, out, sizeof(out)) == 0);           // 奇数
    CHECK(sb_adpcm_encode_block(&enc, pcm, 4, out, 4) == 0);                     // 缓冲太小
    CHECK(sb_adpcm_encode_block(&enc, pcm, SB_AUDIO_FRAME_SAMPLES + 2, out,
                                sizeof(out)) == 0);                              // 超一帧
    CHECK(sb_adpcm_decode_block(out, 2, pcm, 4) == 0);                           // 长度非法
}

int main(void)
{
    test_block_size_and_header();
    test_roundtrip_error_is_bounded();
    test_silence_roundtrip_is_exact();
    test_rejects_bad_args();
    if (g_failures == 0) {
        printf("test_sb_adpcm: PASS\n");
        return 0;
    }
    printf("test_sb_adpcm: %d failure(s)\n", g_failures);
    return 1;
}
