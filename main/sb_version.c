// main/sb_version.c —— 版本信息实现。
#include "sb_version.h"

#include <stdio.h>

#include "esp_ota_ops.h"

const char *sb_version_short(void)
{
    // 只在首次调用时拼接,避免每次刷新界面都做格式化。
    static char buf[64];
    if (buf[0] == '\0') {
        snprintf(buf, sizeof(buf), "v%s+%.7s", SB_APP_VERSION, SB_GIT_COMMIT);
    }
    return buf;
}

const char *sb_version_slot(void)
{
    const esp_partition_t *p = esp_ota_get_running_partition();
    return p ? p->label : "?";
}
