// tests/test_sb_cache_slot.c —— 离线队列槽位编解码的主机测试(不依赖 ESP-IDF)。
#include <stdio.h>
#include <string.h>

#include "sb_cache_slot.h"

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
    uint8_t slot[SB_SLOT_SIZE];
    const uint8_t payload[484] = {1, 2, 3, 4, 5};
    const size_t n = sb_slot_encode(slot, sizeof(slot), 0, 42, payload, sizeof(payload));
    CHECK(n == SB_SLOT_SIZE);

    uint8_t type = 0xFF;
    uint32_t seq = 0;
    uint8_t out[SB_SLOT_MAX_DATA];
    size_t out_len = 0;
    CHECK(sb_slot_decode(slot, n, &type, &seq, out, sizeof(out), &out_len));
    CHECK(type == 0);
    CHECK(seq == 42);
    CHECK(out_len == sizeof(payload));
    CHECK(memcmp(out, payload, sizeof(payload)) == 0);
}

static void test_empty_slot(void)
{
    uint8_t slot[SB_SLOT_SIZE];
    CHECK(sb_slot_encode(slot, sizeof(slot), 0, 7, NULL, 0) == SB_SLOT_SIZE);
    size_t len = 99;
    CHECK(sb_slot_decode(slot, SB_SLOT_SIZE, NULL, NULL, NULL, 0, &len));
    CHECK(len == 0);
}

static void test_corrupt_payload_is_rejected(void)
{
    uint8_t slot[SB_SLOT_SIZE];
    const uint8_t payload[100] = {9};
    sb_slot_encode(slot, sizeof(slot), 0, 1, payload, sizeof(payload));
    CHECK(sb_slot_decode(slot, SB_SLOT_SIZE, NULL, NULL, NULL, 0, NULL));
    slot[SB_SLOT_HDR_LEN + 10] ^= 0xA5;      // 数据被掉电写坏
    CHECK(!sb_slot_decode(slot, SB_SLOT_SIZE, NULL, NULL, NULL, 0, NULL));
}

static void test_bad_magic_and_length(void)
{
    uint8_t slot[SB_SLOT_SIZE];
    sb_slot_encode(slot, sizeof(slot), 0, 1, (const uint8_t *)"abc", 3);

    uint8_t saved = slot[0];
    slot[0] = 0x00;
    CHECK(!sb_slot_decode(slot, SB_SLOT_SIZE, NULL, NULL, NULL, 0, NULL));
    slot[0] = saved;

    // 长度字段超出单槽上限:必须拒绝而不是越界读(len 是 u16,存在 12..13)。
    slot[12] = 0xFF;
    slot[13] = 0xFF;
    CHECK(!sb_slot_decode(slot, SB_SLOT_SIZE, NULL, NULL, NULL, 0, NULL));
}

static void test_encode_rejects_oversize(void)
{
    uint8_t slot[SB_SLOT_SIZE];
    static uint8_t big[SB_SLOT_MAX_DATA + 1];
    CHECK(sb_slot_encode(slot, sizeof(slot), 0, 0, big, sizeof(big)) == 0);
    CHECK(sb_slot_encode(slot, 64, 0, 0, big, 8) == 0);   // 缓冲太小
}

static void test_crc32_known_vector(void)
{
    // "123456789" 的 CRC-32(IEEE)标准值是 0xCBF43926,用来钉住实现不被改错。
    const uint8_t v[] = "123456789";
    CHECK(sb_slot_crc32(v, 9) == 0xCBF43926u);
}

int main(void)
{
    test_roundtrip();
    test_empty_slot();
    test_corrupt_payload_is_rejected();
    test_bad_magic_and_length();
    test_encode_rejects_oversize();
    test_crc32_known_vector();
    if (g_failures == 0) {
        printf("test_sb_cache_slot: PASS\n");
        return 0;
    }
    printf("test_sb_cache_slot: %d failure(s)\n", g_failures);
    return 1;
}
