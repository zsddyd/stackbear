// main/sb_cache.h —— 离线音频队列(可选)。
//
// 网络不通时把已编码的语音块写进 flash 上的 audioq 分区,网络恢复后按顺序补传。
// 默认关闭(见 sb_config.h),因为持续写 flash 会消耗擦写寿命;打开后只在上行
// 不通时写盘,联网时仍走实时 TCP,不做无谓写入。
//
// 存储形式是**定长槽位的环形缓冲**,不是文件系统:
//   slot 0..N-1,每槽 SB_CACHE_SLOT_SIZE 字节,槽头记录类型/长度/序号。
//   这样只需要 esp_partition 读写,不必引入 LittleFS/SPIFFS 依赖,掉电后靠 CRC
//   判断槽是否完整。容量由配置的上限决定(1..3 MB);写满后**保留已存内容,丢弃新帧**,
//   并累加 dropped 计数供界面显示。
#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sb_cache_slot.h"
#define SB_CACHE_SLOT_SIZE SB_SLOT_SIZE
#define SB_CACHE_SLOT_MAGIC SB_SLOT_MAGIC

// 初始化:定位分区并按上限算出可用槽数。分区缺失或未启用时返回 ESP_ERR_NOT_FOUND。
esp_err_t sb_cache_init(void);

// 入队一块音频。返回 ESP_OK;队列满(达到配置上限)时返回 ESP_ERR_NO_MEM 且不写入,
// 由调用方计入 dropped。未启用时返回 ESP_ERR_INVALID_STATE。
esp_err_t sb_cache_push(const uint8_t *data, size_t len);

// 取出一块最旧的数据供补传。peek=true 时只读不推进读指针(补传成功后再 commit)。
// 没有数据返回 ESP_ERR_NOT_FOUND。
esp_err_t sb_cache_peek(uint8_t *out, size_t cap, size_t *out_len, bool *is_last);
// 提交(删除)当前最旧的一条,推进读指针。
esp_err_t sb_cache_commit(void);

// 清空队列(读指针=写指针)。
esp_err_t sb_cache_clear(void);

// 统计:待补传条数 / 已丢弃条数 / 已使用字节数。
uint32_t sb_cache_pending(void);
uint32_t sb_cache_dropped(void);
uint32_t sb_cache_used_bytes(void);
bool sb_cache_enabled(void);
