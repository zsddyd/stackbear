// main/sb_cache_slot.h —— 离线队列槽位的编解码(纯逻辑,不依赖 ESP-IDF,可主机测试)。
//
// 掉电可能中断一次写,所以每条槽都自带长度与 CRC:读回时 CRC 不符即判为无效,
// 直接跳过,不会把半截数据当成音频发出去。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SB_SLOT_SIZE      512u
#define SB_SLOT_HDR_LEN   16u
#define SB_SLOT_MAX_DATA  (SB_SLOT_SIZE - SB_SLOT_HDR_LEN)   // 496
#define SB_SLOT_MAGIC     0x5342u

// 槽头布局(16 字节):
//   0..1  magic u16
//   2     type  u8
//   3     预留  u8 = 0
//   4..7  seq   u32
//   8..11 crc   u32 (payload 的 CRC-32/IEEE)
//   12..13 len  u16  —— 用 u16 而不是 u8:一块 ADPCM 音频是 484 字节,装不下
//   14..15 预留 u16 = 0

// 把一块数据编码进槽(前 SB_SLOT_HDR_LEN 字节为头,其余为数据)。
// out 需 >= SB_SLOT_SIZE。返回写入字节数;参数非法返回 0。
size_t sb_slot_encode(uint8_t *out, size_t cap, uint8_t type, uint32_t seq,
                      const uint8_t *data, size_t len);

// 校验并解出一个槽。magic/长度/CRC 任一不符返回 false。data 可为 NULL(只校验)。
bool sb_slot_decode(const uint8_t *slot, size_t slot_len, uint8_t *type,
                    uint32_t *seq, uint8_t *data, size_t cap, size_t *data_len);

// payload 用的 CRC32(IEEE),与 firmware/cloud 两侧保持一致。
uint32_t sb_slot_crc32(const uint8_t *data, size_t len);
