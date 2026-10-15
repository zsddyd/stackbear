// main/sb_uplink.h —— 通过裸 TCP 长连接把 Opus 音频帧按私有协议送到 ECS。
#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    SB_UL_IDLE = 0,   // 未启动
    SB_UL_CONNECTING,
    SB_UL_UP,         // 已连上并可发送
    SB_UL_ERROR,
} sb_uplink_state_t;

// 创建队列与上传任务。真正的连接在任务里异步建立,本函数不阻塞。
esp_err_t sb_uplink_start(void);
void sb_uplink_stop(void);
bool sb_uplink_running(void);

sb_uplink_state_t sb_uplink_state(void);
uint32_t sb_uplink_frames_sent(void);
uint32_t sb_uplink_frames_dropped(void);
int sb_uplink_queue_len(void);

// 开始一个新会话:下一次发送前会先发 HELLO。
void sb_uplink_begin_session(void);
// 结束当前会话:排空队列后发 END 并断开。
void sb_uplink_end_session(void);

// 入队一个音频帧(不阻塞)。flags 取 sb_proto.h 的 SB_FLAG_*(如 SB_FLAG_SEG_START
// 标记新语音段的第一块)。队列满时丢弃并计数,返回 false。
bool sb_uplink_push_audio(const uint8_t *data, size_t len, uint8_t flags);
