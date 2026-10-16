// 主机测试用的最小替身:让 main/sb_version.c 脱离 ESP-IDF 也能编译。
#pragma once
typedef struct { const char *label; } esp_partition_t;
const esp_partition_t *esp_ota_get_running_partition(void);
