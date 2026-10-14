// main/sb_vad.h —— 常开语音活动检测(VAD)纯逻辑实现,不依赖 ESP-IDF/LVGL,可主机测试。
//
// 目标是让"常开录音"省电省流量:静音不上传,只把有人说话的段落切出来编码上传。
// 采用「短时能量 + 迟滞 + 拖尾(悬挂) + 自适应噪声底」的经典做法:
//   - 进入阈值 = max(enter_floor, 噪声底 * noise_gain);退出阈值 = 进入阈值的一半,
//     形成迟滞,避免在阈值附近来回抖动把一段话切成很多碎段;
//   - 语音结束后要连续 hangover_frames 帧静音才判定段落结束(拖尾),容忍词间停顿;
//   - 噪声底只在「非语音且能量不高」时缓慢更新,避免把说话本身当成噪声底抬高。
#pragma once

#include <stddef.h>
#include <stdint.h>

typedef enum {
    SB_VAD_SILENCE = 0,     // 当前帧静音
    SB_VAD_SPEECH  = 1,     // 当前帧属于语音段
    SB_VAD_SPEECH_END = 2,  // 刚刚判定语音段结束(本帧为静音),调用方应封段上传
} sb_vad_ev_t;

typedef struct {
    uint32_t enter_floor;      // 进入语音的 RMS 绝对下限(防止极小噪声底导致误触发)
    uint32_t exit_floor;       // 退出语音的 RMS 绝对下限(通常 <= enter_floor)
    uint16_t noise_gain_x16;   // 进入阈值 = max(enter_floor, 噪声底 * noise_gain_x16 / 16)
    uint8_t  hangover_frames;  // 语音后需连续静音帧数才封段
    uint8_t  min_speech_frames;// 语音段最短帧数,短于此的抖动当作噪声丢弃
} sb_vad_cfg_t;

typedef struct {
    sb_vad_cfg_t cfg;
    uint32_t noise_rms;      // 自适应噪声底(RMS)
    uint8_t  in_speech;
    uint8_t  hangover;
    uint16_t speech_frames;
    uint16_t segment_frames; // 本段累计帧数(含拖尾),供上层估算时长
} sb_vad_t;

// 出厂缺省参数:面向 16 kHz 近场麦克风、20 ms 帧。
void sb_vad_default_cfg(sb_vad_cfg_t *cfg);

void sb_vad_init(sb_vad_t *vad, const sb_vad_cfg_t *cfg);

// 整数 RMS:sqrt(mean(x^2))。纯整数运算,便于确定性测试。
uint32_t sb_vad_rms(const int16_t *pcm, size_t samples);

// 当前有效进入阈值(RMS)。主要供日志与调参查看。
uint32_t sb_vad_enter_threshold(const sb_vad_t *vad);

// 处理一帧 PCM。返回本帧的 VAD 事件。
sb_vad_ev_t sb_vad_process(sb_vad_t *vad, const int16_t *pcm, size_t samples);
