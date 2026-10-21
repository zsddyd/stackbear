// main/sb_config.h —— 应用持久化配置(NVS)。WiFi 凭据、服务器地址、设备 id。
#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#define SB_SSID_MAX   33   // 32 + '\0'
#define SB_PASS_MAX   65   // 64 + '\0'
#define SB_HOST_MAX   64
#define SB_DEVID_MAX  16

// 离线音频队列分区(partitions.csv 的 audioq)约 2 MB;按用户约束,可选上限
// 只到 2 MB(队列大小从 1 MB 起)。
#define SB_CACHE_PARTITION   "audioq"
#define SB_CACHE_MAX_LIMIT_MB 2u

// 出厂默认服务器(可用 Kconfig/编译期覆盖)。云端服务地址在 NVS 里可改。
#ifndef SB_DEFAULT_SRV_HOST
#define SB_DEFAULT_SRV_HOST "8.137.56.181"
#endif
#ifndef SB_DEFAULT_SRV_PORT
#define SB_DEFAULT_SRV_PORT 18080
#endif

typedef struct {
    char     ssid[SB_SSID_MAX];
    char     pass[SB_PASS_MAX];
    char     srv_host[SB_HOST_MAX];
    uint16_t srv_port;
    char     dev_id[SB_DEVID_MAX];
    bool     provisioned;   // 是否已成功配网(有有效凭据)
    // 离线音频队列(可选):网络不通时把已编码的语音写进 flash,联网后补传。
    // 默认关闭,避免不做离线缓存时白白消耗 flash 擦写寿命。
    bool     cache_enabled;
    uint8_t  cache_limit_mb;   // 1..3,实际可用容量上限
    // 背光常态亮度(%)与空闲调暗超时(秒)。取值域、步进与档位表都在
    // main/sb_display_cfg.h —— 那里是纯逻辑并有主机测试,夹取规则只有一份。
    uint8_t  brightness;       // SB_BRIGHT_MIN..SB_BRIGHT_MAX
    uint16_t dim_timeout_s;    // 空闲这么多秒后调暗;0 = 不调暗
} sb_config_t;

// 初始化 NVS 命名空间并读入配置;缺失项用默认值补全(默认服务器来自编译期
// SB_DEFAULT_SRV_HOST/PORT)。设备 id 首次生成并落盘。
esp_err_t sb_config_init(void);

// 当前配置的只读指针(初始化后有效)。
const sb_config_t *sb_config_get(void);

// 保存 WiFi 凭据。仅在调用方已确认能连上目标热点后调用,避免写坏凭据。
esp_err_t sb_config_set_wifi(const char *ssid, const char *pass);

// 保存服务器地址(host/port)。
esp_err_t sb_config_set_server(const char *host, uint16_t port);

// 清除已保存的 WiFi 凭据(保留设备 id 与服务器地址)。
esp_err_t sb_config_clear_wifi(void);

// 离线音频队列设置。limit_mb 会被限制在 1..SB_CACHE_MAX_LIMIT_MB。
esp_err_t sb_config_set_cache(bool enabled, uint8_t limit_mb);

// 背光常态亮度(%)。percent 会被夹到 SB_BRIGHT_MIN..SB_BRIGHT_MAX。
// 空闲调暗超时(秒),只接受 sb_display_cfg.h 档位表里的值,表外的值取最近档位;
// 0 表示不调暗。
esp_err_t sb_config_set_brightness(uint8_t percent);
esp_err_t sb_config_set_dim_timeout(uint16_t seconds);
