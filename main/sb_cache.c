// main/sb_cache.c —— 基于 audioq 分区的定长槽位环形队列。
//
// 设计要点:
//  * 只用一个写指针和一个读指针,不需要在 flash 里维护索引;上电时扫描一遍有效槽
//    恢复计数(见 sb_cache_init)。
//  * 写满(达到配置上限)后【不覆盖】已有数据,新帧直接丢弃 —— 离线期间的录音宁可
//    保留最早的一段(通常是用户最想留的),这也是产品要求的行为。丢帧数在 dropped
//    里递增,界面会显示。
//  * 每条槽自带 CRC,掉电写坏的槽在补传时被识别并跳过。
#include "sb_cache.h"

#include <string.h>

#include "esp_log.h"
#include "esp_partition.h"
#include "sb_config.h"

static const char *TAG = "sb_cache";

static const esp_partition_t *s_part;
static uint32_t s_slots;        // 本配置下的可用槽数
static uint32_t s_head;         // 写入位置(下一个可写槽)
static uint32_t s_tail;         // 补传位置(最旧未发送槽)
static uint32_t s_count;        // 未补传条数
static uint32_t s_dropped;
static uint32_t s_write_seq;    // 全局递增序号,跨上电持续(取回时 +1)
static bool s_enabled;
static uint8_t s_scratch[SB_CACHE_SLOT_SIZE];

uint32_t sb_cache_pending(void) { return s_count; }
uint32_t sb_cache_dropped(void) { return s_dropped; }
uint32_t sb_cache_used_bytes(void) { return s_count * SB_CACHE_SLOT_SIZE; }
bool sb_cache_enabled(void) { return s_enabled; }

static esp_err_t read_slot(uint32_t index, uint8_t *buf)
{
    return esp_partition_read(s_part, (size_t)index * SB_CACHE_SLOT_SIZE, buf,
                              SB_CACHE_SLOT_SIZE);
}

static esp_err_t write_slot(uint32_t index, const uint8_t *buf)
{
    return esp_partition_write(s_part, (size_t)index * SB_CACHE_SLOT_SIZE, buf,
                               SB_CACHE_SLOT_SIZE);
}

esp_err_t sb_cache_init(void)
{
    const sb_config_t *cfg = sb_config_get();
    s_enabled = cfg->cache_enabled;
    s_dropped = 0;
    s_count = 0;
    s_head = 0;
    s_tail = 0;
    s_write_seq = 0;
    if (!s_enabled) {
        return ESP_ERR_INVALID_STATE;   // 未启用:调用方只记录 dropped
    }
    s_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY,
                                      SB_CACHE_PARTITION);
    if (s_part == NULL) {
        ESP_LOGE(TAG, "找不到分区 '%s'", SB_CACHE_PARTITION);
        s_enabled = false;
        return ESP_ERR_NOT_FOUND;
    }
    const uint32_t limit = (uint32_t)cfg->cache_limit_mb * 1024u * 1024u;
    const uint32_t usable = (limit < s_part->size) ? limit : s_part->size;
    s_slots = usable / SB_CACHE_SLOT_SIZE;
    if (s_slots < 2) {
        s_enabled = false;
        return ESP_ERR_INVALID_SIZE;
    }

    // 扫描恢复:从头扫到第一个无效槽,得到上电前已有的条数。定长环形 + 序号,
    // 足够判断顺序;这里只关心"有多少条待补传"。
    for (uint32_t i = 0; i < s_slots; i++) {
        if (read_slot(i, s_scratch) != ESP_OK) {
            break;
        }
        uint32_t seq = 0;
        if (!sb_slot_decode(s_scratch, SB_CACHE_SLOT_SIZE, NULL, &seq, NULL, 0, NULL)) {
            break;
        }
        s_count++;
        if (seq + 1 > s_write_seq) {
            s_write_seq = seq + 1;
        }
    }
    s_head = s_count % s_slots;
    s_tail = 0;
    ESP_LOGI(TAG, "离线队列就绪: %u 槽上限, 上电恢复 %u 条待补传, 上限 %u MB",
             (unsigned)s_slots, (unsigned)s_count, (unsigned)cfg->cache_limit_mb);
    return ESP_OK;
}

esp_err_t sb_cache_push(const uint8_t *data, size_t len)
{
    if (!s_enabled) {
        return ESP_ERR_INVALID_STATE;
    }
    if (data == NULL || len == 0 || len > SB_SLOT_MAX_DATA) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_count >= s_slots) {
        s_dropped++;    // 存满:保留已存内容,丢弃新帧
        return ESP_ERR_NO_MEM;
    }
    const size_t n = sb_slot_encode(s_scratch, sizeof(s_scratch), 0, s_write_seq, data, len);
    if (n == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = write_slot(s_head, s_scratch);
    if (err != ESP_OK) {
        return err;
    }
    s_write_seq++;
    s_head = (s_head + 1) % s_slots;
    s_count++;
    return ESP_OK;
}

esp_err_t sb_cache_peek(uint8_t *out, size_t cap, size_t *out_len, bool *is_last)
{
    if (!s_enabled) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_count == 0) {
        return ESP_ERR_NOT_FOUND;
    }
    if (read_slot(s_tail, s_scratch) != ESP_OK) {
        return ESP_FAIL;
    }
    size_t n = 0;
    if (!sb_slot_decode(s_scratch, SB_CACHE_SLOT_SIZE, NULL, NULL, out, cap, &n)) {
        // 坏槽(掉电中断):跳过,继续找下一条。
        sb_cache_commit();
        return sb_cache_peek(out, cap, out_len, is_last);
    }
    if (out_len) {
        *out_len = n;
    }
    if (is_last) {
        *is_last = (s_count <= 1);
    }
    return ESP_OK;
}

esp_err_t sb_cache_commit(void)
{
    if (!s_enabled || s_count == 0) {
        return ESP_ERR_NOT_FOUND;
    }
    s_tail = (s_tail + 1) % s_slots;
    s_count--;
    return ESP_OK;
}

esp_err_t sb_cache_clear(void)
{
    if (!s_enabled) {
        return ESP_ERR_INVALID_STATE;
    }
    s_head = 0;
    s_tail = 0;
    s_count = 0;
    return esp_partition_erase_range(s_part, 0,
                                     (size_t)s_slots * SB_CACHE_SLOT_SIZE);
}
