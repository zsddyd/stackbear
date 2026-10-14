// main/sb_config.h —— 应用持久化配置(NVS)。WiFi 凭据、服务器地址、设备 id。
#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#define SB_SSID_MAX   33   // 32 + '\0'
#define SB_PASS_MAX   65   // 64 + '\0'
#define SB_HOST_MAX   64
#define SB_DEVID_MAX  16

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
