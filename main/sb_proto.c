// main/sb_proto.c —— 私有帧协议的编解码实现(纯逻辑)。
#include "sb_proto.h"

uint16_t sb_proto_crc16(const uint8_t *data, size_t len)
{
    uint16_t crc = 0xFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (int bit = 0; bit < 8; bit++) {
            if (crc & 0x8000u) {
                crc = (uint16_t)((crc << 1) ^ 0x1021u);
            } else {
                crc = (uint16_t)(crc << 1);
            }
        }
    }
    return crc;
}

static void put_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)(v & 0xFFu);
}

static uint16_t get_u16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

size_t sb_proto_encode(uint8_t *out, size_t cap, uint8_t type, uint8_t flags,
                       uint16_t seq, const uint8_t *payload, uint16_t len)
{
    if (out == NULL) {
        return 0;
    }
    if (len > SB_PROTO_MAX_PAYLOAD) {
        return 0;
    }
    if (len > 0 && payload == NULL) {
        return 0;
    }
    const size_t total = SB_PROTO_OVERHEAD + (size_t)len;
    if (cap < total) {
        return 0;
    }

    out[0] = SB_PROTO_MAGIC0;
    out[1] = SB_PROTO_MAGIC1;
    out[2] = SB_PROTO_VERSION;
    out[3] = type;
    out[4] = flags;
    out[5] = 0;
    put_u16(&out[6], seq);
    put_u16(&out[8], len);
    for (uint16_t i = 0; i < len; i++) {
        out[SB_PROTO_HEADER_LEN + i] = payload[i];
    }
    const uint16_t crc = sb_proto_crc16(out, SB_PROTO_HEADER_LEN + (size_t)len);
    put_u16(&out[SB_PROTO_HEADER_LEN + len], crc);
    return total;
}

int sb_proto_decode(const uint8_t *buf, size_t len, sb_proto_frame_t *frame)
{
    if (buf == NULL || len < SB_PROTO_HEADER_LEN) {
        return 0;
    }
    if (buf[0] != SB_PROTO_MAGIC0 || buf[1] != SB_PROTO_MAGIC1) {
        return -1;
    }
    if (buf[2] != SB_PROTO_VERSION) {
        return -1;
    }
    const uint16_t payload_len = get_u16(&buf[8]);
    if (payload_len > SB_PROTO_MAX_PAYLOAD) {
        return -1;  // 畸形长度:当作坏数据重新对齐
    }
    const size_t total = SB_PROTO_OVERHEAD + (size_t)payload_len;
    if (len < total) {
        return 0;  // 还没收全
    }
    const uint16_t want = get_u16(&buf[SB_PROTO_HEADER_LEN + payload_len]);
    const uint16_t got = sb_proto_crc16(buf, SB_PROTO_HEADER_LEN + (size_t)payload_len);
    if (want != got) {
        return -1;
    }
    if (frame != NULL) {
        frame->type = buf[3];
        frame->flags = buf[4];
        frame->seq = get_u16(&buf[6]);
        frame->len = payload_len;
        frame->payload = &buf[SB_PROTO_HEADER_LEN];
    }
    return (int)total;
}
