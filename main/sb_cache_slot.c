// main/sb_cache_slot.c —— 槽位编解码实现。
#include "sb_cache_slot.h"

#include <string.h>

uint32_t sb_slot_crc32(const uint8_t *data, size_t len)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; bit++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1u)));
        }
    }
    return crc ^ 0xFFFFFFFFu;
}

static void put_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)(v & 0xFFu);
}

static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static uint16_t get_u16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static uint32_t get_u32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

size_t sb_slot_encode(uint8_t *out, size_t cap, uint8_t type, uint32_t seq,
                      const uint8_t *data, size_t len)
{
    if (out == NULL || cap < SB_SLOT_SIZE || len > SB_SLOT_MAX_DATA) {
        return 0;
    }
    if (len > 0 && data == NULL) {
        return 0;
    }
    memset(out, 0, SB_SLOT_SIZE);   // 未用部分清零,避免残留旧数据被误读
    put_u16(out, SB_SLOT_MAGIC);
    out[2] = type;
    out[3] = 0;                     // 预留
    put_u32(out + 4, seq);
    put_u32(out + 8, len ? sb_slot_crc32(data, len) : 0);
    put_u16(out + 12, (uint16_t)len);
    put_u16(out + 14, 0);           // 预留
    if (len) {
        memcpy(out + SB_SLOT_HDR_LEN, data, len);
    }
    return SB_SLOT_SIZE;
}

bool sb_slot_decode(const uint8_t *slot, size_t slot_len, uint8_t *type,
                    uint32_t *seq, uint8_t *data, size_t cap, size_t *data_len)
{
    if (slot == NULL || slot_len < SB_SLOT_SIZE) {
        return false;
    }
    if (get_u16(slot) != SB_SLOT_MAGIC) {
        return false;
    }
    const uint16_t len = get_u16(slot + 12);
    if (len > SB_SLOT_MAX_DATA) {
        return false;
    }
    const uint8_t *payload = slot + SB_SLOT_HDR_LEN;
    const uint32_t want = get_u32(slot + 8);
    if (len == 0) {
        if (want != 0) {
            return false;
        }
    } else if (sb_slot_crc32(payload, len) != want) {
        return false;   // 写入被掉电打断的槽
    }
    if (type) {
        *type = slot[2];
    }
    if (seq) {
        *seq = get_u32(slot + 4);
    }
    if (data_len) {
        *data_len = len;
    }
    if (data != NULL && cap < len) {
        return false;
    }
    if (data != NULL && len) {
        memcpy(data, payload, len);
    }
    return true;
}
