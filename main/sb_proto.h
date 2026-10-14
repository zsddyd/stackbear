// main/sb_proto.h —— 设备 <-> ECS 的私有二进制帧协议(纯逻辑,不依赖 ESP-IDF/LVGL)。
//
// 设计目标:在一条长连接上把连续的 Opus 音频小帧流式送到 ECS,抗丢字节、可重同步、
// 便于在主机上做单元测试。协议本身与传输无关,当前承载在裸 TCP(ECS 18080)上。
//
// 帧格式(全部大端,总开销 12 字节):
//   off 0  magic[2]  = 'S' 'B' (0x53 0x42)
//   off 2  version u8 = 1
//   off 3  type    u8
//   off 4  flags   u8   (bit0 = LAST,该会话最后一帧)
//   off 5  reserved u8  = 0
//   off 6  seq     u16  会话内自增,回绕
//   off 8  len     u16  payload 字节数
//   off 10 payload[len]
//   off 10+len crc16 u16  覆盖 header(除 CRC 自身)+ payload,CCITT-FALSE
//
// 之所以带 magic + 长度 + CRC:音频链路偶发丢字节时,接收端必须能跳过坏数据重新
// 对齐到下一帧,而不是永久错位。crc 用来丢弃被截断/损坏的帧。
#pragma once

#include <stddef.h>
#include <stdint.h>

#define SB_PROTO_MAGIC0      0x53u  // 'S'
#define SB_PROTO_MAGIC1      0x42u  // 'B'
#define SB_PROTO_VERSION     1u
#define SB_PROTO_HEADER_LEN  10u
#define SB_PROTO_CRC_LEN     2u
#define SB_PROTO_OVERHEAD    (SB_PROTO_HEADER_LEN + SB_PROTO_CRC_LEN)
#define SB_PROTO_MAX_PAYLOAD 1400u  // 单帧 payload 上限;Opus 60ms 帧约 180B,留足余量
#define SB_PROTO_MAX_FRAME   (SB_PROTO_OVERHEAD + SB_PROTO_MAX_PAYLOAD)

typedef enum {
    SB_FRAME_HELLO = 0x01,  // 会话开始:JSON 元数据(设备 id/编码/采样率/帧长)
    SB_FRAME_AUDIO = 0x02,  // Opus 音频帧
    SB_FRAME_END   = 0x03,  // 会话结束:JSON 统计
    SB_FRAME_PING  = 0x04,  // 保活
    SB_FRAME_ACK   = 0x80,  // 服务端 -> 设备:确认收到 seq
} sb_frame_type_t;

// AUDIO 帧标志位。除 LAST 外,SEG_START 用来标记"本块是一个新语音段的第一块"。
// 不新增帧型是刻意的:分段边界随音频帧一起走,不占额外带宽,断线重连也不会错位。
// 云端据此把每段的起始毫秒偏移与帧数记进会话 meta,供后续按段提交 ASR。
#define SB_FLAG_LAST      0x01u
#define SB_FLAG_SEG_START 0x02u

typedef struct {
    uint8_t  type;
    uint8_t  flags;
    uint16_t seq;
    uint16_t len;
    const uint8_t *payload;  // 指向输入缓冲,不拥有
} sb_proto_frame_t;

// CRC-16/CCITT-FALSE(poly 0x1021, init 0xFFFF)。用于帧完整性校验。
uint16_t sb_proto_crc16(const uint8_t *data, size_t len);

// 把一帧编码进 out(cap 字节)。返回写出的总字节数;参数非法或放不下时返回 0。
size_t sb_proto_encode(uint8_t *out, size_t cap, uint8_t type, uint8_t flags,
                       uint16_t seq, const uint8_t *payload, uint16_t len);

// 从 buf 起始位置尝试解出【一帧完整数据】。成功返回消耗的字节数(>0)并回填 frame
// (payload 指向 buf 内部);数据不足返回 0(应继续收);校验失败返回 -1(调用方
// 应向后滑动 1 字节重新寻找 magic)。frame 可为 NULL(只做长度/校验判断)。
int sb_proto_decode(const uint8_t *buf, size_t len, sb_proto_frame_t *frame);
