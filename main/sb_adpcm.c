// main/sb_adpcm.c —— IMA-ADPCM(4bit)编解码。纯逻辑,可主机测试。
#include "sb_adpcm.h"

#include <string.h>

// 标准 IMA-ADPCM 步长表(89 项)。
static const int16_t STEP_TABLE[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41,
    45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209,
    230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876,
    963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749,
    3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630,
    9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623,
    27086, 29794, 32767,
};

// 差分低 3 位对应的步长索引增量;高 1 位是符号位。
static const int8_t INDEX_TABLE[16] = {
    -1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8,
};

void sb_adpcm_init(sb_adpcm_enc_t *enc)
{
    if (enc == NULL) {
        return;
    }
    enc->predictor = 0;
    enc->index = 0;
}

static int32_t clamp_i16(int32_t v)
{
    if (v > 32767) {
        return 32767;
    }
    if (v < -32768) {
        return -32768;
    }
    return v;
}

// 把一次 4bit 差分作用到预测值上,返回重建后的采样值(编码/解码共用,保证一致)。
static int32_t adpcm_step(int32_t predictor, int32_t index, uint8_t nibble)
{
    const int32_t step = STEP_TABLE[index];
    int32_t diff = step >> 3;
    if (nibble & 1) {
        diff += step >> 2;
    }
    if (nibble & 2) {
        diff += step >> 1;
    }
    if (nibble & 4) {
        diff += step;
    }
    if (nibble & 8) {
        predictor -= diff;
    } else {
        predictor += diff;
    }
    return clamp_i16(predictor);
}

static int32_t next_index(int32_t index, uint8_t nibble)
{
    index += INDEX_TABLE[nibble];
    if (index < 0) {
        index = 0;
    } else if (index > 88) {
        index = 88;
    }
    return index;
}

size_t sb_adpcm_encode_block(sb_adpcm_enc_t *enc, const int16_t *pcm,
                             size_t samples, uint8_t *out, size_t cap)
{
    if (enc == NULL || pcm == NULL || out == NULL) {
        return 0;
    }
    if (samples == 0 || samples > SB_AUDIO_FRAME_SAMPLES || (samples & 1u) != 0) {
        return 0;
    }
    const size_t total = SB_ADPCM_HEADER_LEN + samples / 2;
    if (cap < total) {
        return 0;
    }

    // 每块以第一个采样重新起始,块之间互不依赖。
    enc->predictor = pcm[0];
    // 步长索引【不】重置:跨块保持自适应。每块都回 0 会让开头几步用极小步长,
    // 信号上升沿跟不上(ADPCM 的"攻击"问题)。索引随块头一起传给解码端,仍然自洽。
    out[0] = (uint8_t)(enc->predictor & 0xFF);
    out[1] = (uint8_t)((enc->predictor >> 8) & 0xFF);
    out[2] = (uint8_t)enc->index;
    out[3] = 0;

    for (size_t i = 0; i < samples; i++) {
        const int32_t sample = pcm[i];
        const int32_t step = STEP_TABLE[enc->index];
        int32_t diff = sample - enc->predictor;
        uint8_t nibble = 0;
        if (diff < 0) {
            nibble = 8;
            diff = -diff;
        }
        // 逐位逼近:与解码端使用同一套加法,保证重建值完全一致。
        int32_t delta = step >> 3;
        if (diff >= step) {
            nibble |= 4;
            diff -= step;
            delta += step;
        }
        if (diff >= (step >> 1)) {
            nibble |= 2;
            diff -= step >> 1;
            delta += step >> 1;
        }
        if (diff >= (step >> 2)) {
            nibble |= 1;
            delta += step >> 2;
        }
        enc->predictor += (nibble & 8) ? -delta : delta;
        enc->predictor = clamp_i16(enc->predictor);
        enc->index = next_index(enc->index, nibble);

        uint8_t *slot = &out[SB_ADPCM_HEADER_LEN + i / 2];
        if ((i & 1u) == 0) {
            *slot = nibble;           // 低半字节 = 先出现的采样
        } else {
            *slot |= (uint8_t)(nibble << 4);
        }
    }
    return total;
}

size_t sb_adpcm_decode_block(const uint8_t *in, size_t len, int16_t *pcm,
                             size_t max_samples)
{
    if (in == NULL || pcm == NULL || len <= SB_ADPCM_HEADER_LEN) {
        return 0;
    }
    const size_t samples = (len - SB_ADPCM_HEADER_LEN) * 2;
    if (samples > max_samples) {
        return 0;
    }
    int32_t predictor = (int16_t)((uint16_t)in[0] | ((uint16_t)in[1] << 8));
    int32_t index = in[2];
    if (index > 88) {
        return 0;
    }
    for (size_t i = 0; i < samples; i++) {
        const uint8_t byte = in[SB_ADPCM_HEADER_LEN + i / 2];
        const uint8_t nibble = (i & 1u) ? (byte >> 4) : (byte & 0x0Fu);
        predictor = adpcm_step(predictor, index, nibble);
        index = next_index(index, nibble);
        pcm[i] = (int16_t)predictor;
    }
    return samples;
}
