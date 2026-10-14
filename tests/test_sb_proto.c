// tests/test_sb_proto.c —— sb_proto 私有帧协议的主机单元测试(不依赖 ESP-IDF)。
#include <stdio.h>
#include <string.h>

#include "sb_proto.h"

static int g_failures;

#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);         \
            g_failures++;                                                  \
        }                                                                  \
    } while (0)

static void test_roundtrip(void)
{
    const uint8_t payload[] = {0x01, 0x02, 0x03, 0x7F, 0x80, 0xFF};
    uint8_t frame[SB_PROTO_MAX_FRAME];

    const size_t n = sb_proto_encode(frame, sizeof(frame), SB_FRAME_AUDIO,
                                     SB_FLAG_LAST, 0x1234, payload, sizeof(payload));
    CHECK(n == SB_PROTO_OVERHEAD + sizeof(payload));

    sb_proto_frame_t out;
    const int used = sb_proto_decode(frame, n, &out);
    CHECK(used == (int)n);
    CHECK(out.type == SB_FRAME_AUDIO);
    CHECK(out.flags == SB_FLAG_LAST);
    CHECK(out.seq == 0x1234);
    CHECK(out.len == sizeof(payload));
    CHECK(memcmp(out.payload, payload, sizeof(payload)) == 0);
}

static void test_empty_payload(void)
{
    uint8_t frame[SB_PROTO_MAX_FRAME];
    const size_t n = sb_proto_encode(frame, sizeof(frame), SB_FRAME_PING, 0, 7, NULL, 0);
    CHECK(n == SB_PROTO_OVERHEAD);
    sb_proto_frame_t out;
    CHECK(sb_proto_decode(frame, n, &out) == (int)n);
    CHECK(out.type == SB_FRAME_PING);
    CHECK(out.len == 0);
}

static void test_truncated_returns_zero(void)
{
    const uint8_t payload[20] = {0};
    uint8_t frame[SB_PROTO_MAX_FRAME];
    const size_t n = sb_proto_encode(frame, sizeof(frame), SB_FRAME_AUDIO, 0, 1,
                                     payload, sizeof(payload));
    // 少收一个字节:crc 还没到齐
    CHECK(sb_proto_decode(frame, n - 1, NULL) == 0);
    // 只收到一部分 header
    CHECK(sb_proto_decode(frame, 3, NULL) == 0);
}

static void test_corruption_detected(void)
{
    const uint8_t payload[16] = {1, 2, 3, 4};
    uint8_t frame[SB_PROTO_MAX_FRAME];
    const size_t n = sb_proto_encode(frame, sizeof(frame), SB_FRAME_AUDIO, 0, 9,
                                     payload, sizeof(payload));
    frame[SB_PROTO_HEADER_LEN] ^= 0x5A;  // 翻一个 payload 位
    CHECK(sb_proto_decode(frame, n, NULL) == -1);
    frame[SB_PROTO_HEADER_LEN] ^= 0x5A;  // 复原
    CHECK(sb_proto_decode(frame, n, NULL) == (int)n);
    frame[8] = 0x7F;                     // 篡改长度到超出上限
    CHECK(sb_proto_decode(frame, n, NULL) == -1);
}

static void test_resync_scan(void)
{
    const uint8_t payload[5] = {9, 8, 7, 6, 5};
    uint8_t buf[64];
    const uint8_t noise[] = {0x00, 0x11, 0x53, 0x00, 0xAB};  // 含半个 magic,不能被误判
    memcpy(buf, noise, sizeof(noise));
    const size_t n = sb_proto_encode(buf + sizeof(noise), sizeof(buf) - sizeof(noise),
                                     SB_FRAME_HELLO, 0, 3, payload, sizeof(payload));
    CHECK(n > 0);

    const size_t total = sizeof(noise) + n;
    size_t offset = 0;
    int decoded = 0;
    while (offset < total) {
        const int used = sb_proto_decode(buf + offset, total - offset, NULL);
        if (used > 0) {
            decoded = used;
            break;
        }
        if (used < 0) {
            offset++;  // 坏数据/未对齐:滑一字节重找 magic
            continue;
        }
        break;  // 0:数据不足
    }
    CHECK(decoded == (int)n);
    CHECK(offset == sizeof(noise));
}

static void test_encode_rejects_overflow(void)
{
    static uint8_t small[8];
    const uint8_t payload[4] = {0};
    CHECK(sb_proto_encode(small, sizeof(small), SB_FRAME_AUDIO, 0, 0, payload,
                          sizeof(payload)) == 0);
    uint8_t big[SB_PROTO_MAX_FRAME];
    static uint8_t too_big[SB_PROTO_MAX_PAYLOAD + 1];
    CHECK(sb_proto_encode(big, sizeof(big), SB_FRAME_AUDIO, 0, 0, too_big,
                          SB_PROTO_MAX_PAYLOAD + 1) == 0);
}

int main(void)
{
    test_roundtrip();
    test_empty_payload();
    test_truncated_returns_zero();
    test_corruption_detected();
    test_resync_scan();
    test_encode_rejects_overflow();
    if (g_failures == 0) {
        printf("test_sb_proto: PASS\n");
        return 0;
    }
    printf("test_sb_proto: %d failure(s)\n", g_failures);
    return 1;
}
