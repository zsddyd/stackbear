// main/sb_ota.h —— 从 ECS 拉取并安装 OTA 固件。
//
// 流程:问 ECS 要版本清单(JSON)→ 与本地版本比较 → 需要时把 OTA 镜像写进备用槽
// → 校验 sha256 → 标记下次从新槽启动 → 重启。新固件若未确认健康,bootloader
// 会自动回滚(见 sdkconfig 的 rollback 选项)。
//
// 用 http 而不是 https:ECS 现在只有公网 IP 和 18080 端口,没有证书。完整性由下载后
// 的 sha256 校验保证(期望值来自 version.json),不依赖 TLS。
#pragma once

#include "esp_err.h"
#include <stdbool.h>

typedef enum {
    SB_OTA_IDLE = 0,
    SB_OTA_CHECKING,     // 正在询问 ECS
    SB_OTA_UP_TO_DATE,   // 已是最新
    SB_OTA_DOWNLOADING,
    SB_OTA_FAILED,
} sb_ota_state_t;

// 启动后台检查(不阻塞调用方)。升级前会先结束当前录音会话。
esp_err_t sb_ota_check_start(void);

sb_ota_state_t sb_ota_state(void);
const char *sb_ota_message(void);   // 一行状态说明,供界面显示
bool sb_ota_busy(void);
