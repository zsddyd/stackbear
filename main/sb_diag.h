// main/sb_diag.h —— 轻量运行时诊断,便于在没有串口的环境下从云端看设备状态。
//
// 这颗板子常常只插在手机上(Termux),拿不到串口日志。把关键失败码与剩余堆塞进
// 上传的 HELLO 里,云端打印出来就等于一条"远程日志",排障不必再刷一次带额外
// 调试代码的固件。
#pragma once

#include <stdint.h>

// 失败码(0 = 正常)
enum {
    SB_REC_OK = 0,
    SB_REC_ERR_AUDIO_INIT = 1,     // bsp_audio_init 失败
    SB_REC_ERR_AUDIO_FORMAT = 2,   // bsp_audio_set_format 失败
    SB_REC_ERR_OPUS_INIT = 3,      // sb_opus_init 失败
    SB_REC_ERR_TASK = 4,           // 录音任务创建失败
};

typedef struct {
    volatile int32_t rec_err;    // 最近一次录音链路的失败码
    volatile int32_t opus_err;   // opus_encoder_create 返回的库错误码
    volatile uint32_t free_heap; // 发送 HELLO 时的剩余堆
} sb_diag_t;

extern sb_diag_t g_sb_diag;
