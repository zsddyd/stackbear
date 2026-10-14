// main/sb_vad.c —— 常开 VAD 纯逻辑实现。
#include "sb_vad.h"

// 噪声底更新系数:1/32 的指数滑动平均。取值兼顾"跟得上环境变化"和"不被单帧尖峰带跑"。
#define SB_VAD_NOISE_ALPHA_SHIFT 5

void sb_vad_default_cfg(sb_vad_cfg_t *cfg)
{
    if (cfg == NULL) {
        return;
    }
    // 这些值是 16 kHz / 20 ms 帧下的保守起点,真机可用 Button 页式的电压/能量日志再调。
    cfg->enter_floor = 300;      // 约 -41 dBFS 级别,低于此视为纯底噪
    cfg->exit_floor = 160;
    cfg->noise_gain_x16 = 48;    // 噪声底 x3
    cfg->hangover_frames = 25;   // 8 帧 = 500 ms 拖尾
    cfg->min_speech_frames = 5;  // 100 ms 以内的抖动不算一句话
}

void sb_vad_init(sb_vad_t *vad, const sb_vad_cfg_t *cfg)
{
    if (vad == NULL) {
        return;
    }
    sb_vad_cfg_t local;
    if (cfg == NULL) {
        sb_vad_default_cfg(&local);
        cfg = &local;
    }
    vad->cfg = *cfg;
    vad->noise_rms = cfg->enter_floor;  // 以进入下限作为初始噪声底,避免冷启动狂触发
    vad->in_speech = 0;
    vad->hangover = 0;
    vad->speech_frames = 0;
    vad->segment_frames = 0;
}

// 整数平方根(牛顿迭代),避免引入 <math.h> 与浮点,保证主机与目标行为一致。
static uint32_t isqrt_u64(uint64_t x)
{
    if (x == 0) {
        return 0;
    }
    uint64_t r = x;
    uint64_t r2 = (r + 1) / 2;
    while (r2 < r) {
        r = r2;
        r2 = (r + x / r) / 2;
    }
    return (uint32_t)r;
}

uint32_t sb_vad_rms(const int16_t *pcm, size_t samples)
{
    if (pcm == NULL || samples == 0) {
        return 0;
    }
    uint64_t acc = 0;
    for (size_t i = 0; i < samples; i++) {
        const int32_t s = pcm[i];
        acc += (uint64_t)(s * s);
    }
    return isqrt_u64(acc / samples);
}

uint32_t sb_vad_enter_threshold(const sb_vad_t *vad)
{
    if (vad == NULL) {
        return 0;
    }
    uint32_t adaptive = (uint32_t)(((uint64_t)vad->noise_rms * vad->cfg.noise_gain_x16) / 16u);
    return adaptive > vad->cfg.enter_floor ? adaptive : vad->cfg.enter_floor;
}

sb_vad_ev_t sb_vad_process(sb_vad_t *vad, const int16_t *pcm, size_t samples)
{
    if (vad == NULL) {
        return SB_VAD_SILENCE;
    }
    const uint32_t rms = sb_vad_rms(pcm, samples);
    const uint32_t enter = sb_vad_enter_threshold(vad);
    uint32_t exit = enter / 2;
    if (exit < vad->cfg.exit_floor) {
        exit = vad->cfg.exit_floor;
    }

    if (!vad->in_speech) {
        // 非语音:缓慢跟踪噪声底,但能量明显偏高(可能有人刚要说话)时不抬高噪声底。
        if (rms < (enter * 2u)) {
            const uint32_t step = vad->noise_rms >> SB_VAD_NOISE_ALPHA_SHIFT;
            if (rms > vad->noise_rms) {
                vad->noise_rms += step;
                if (vad->noise_rms > rms) {
                    vad->noise_rms = rms;
                }
            } else {
                vad->noise_rms -= step;
                if (vad->noise_rms < rms) {
                    vad->noise_rms = rms;
                }
            }
            // 噪声底不得低于绝对下限,否则安静环境下会过度敏感。
            if (vad->noise_rms < vad->cfg.enter_floor) {
                vad->noise_rms = vad->cfg.enter_floor;
            }
        }
        if (rms >= enter) {
            vad->in_speech = 1;
            vad->hangover = 0;
            vad->speech_frames = 1;
            vad->segment_frames = 1;
            return SB_VAD_SPEECH;
        }
        return SB_VAD_SILENCE;
    }

    // 语音中:高于退出阈值就继续,并清零拖尾;否则累计拖尾帧。
    vad->segment_frames++;
    if (rms >= exit) {
        vad->hangover = 0;
        vad->speech_frames++;
        return SB_VAD_SPEECH;
    }

    if (vad->hangover < 255) {
        vad->hangover++;
    }
    // hangover_frames 表示"仍属于本段的静音帧数";第 hangover_frames+1 帧静音才封段,
    // 这样拖尾静音本身也留在音频里,词尾不会被削掉。
    if (vad->hangover > vad->cfg.hangover_frames) {
        const uint16_t effective = (uint16_t)(vad->speech_frames);
        vad->in_speech = 0;
        vad->hangover = 0;
        vad->speech_frames = 0;
        const uint16_t seg = vad->segment_frames;
        vad->segment_frames = 0;
        // 太短的段落视为噪声抖动,丢弃(不封段),但仍报静音。
        if (effective < vad->cfg.min_speech_frames) {
            return SB_VAD_SILENCE;
        }
        (void)seg;
        return SB_VAD_SPEECH_END;
    }
    return SB_VAD_SPEECH;  // 拖尾期内仍算语音
}
