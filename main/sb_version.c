// main/sb_version.c —— 版本信息实现。
#include "sb_version.h"

#include <stdio.h>

#include "esp_ota_ops.h"

// 纯版本号:OTA 身份串的一半,也是界面 "Firmware" 行显示的内容。
const char *sb_version_number(void)
{
    return SB_APP_VERSION;
}

// 提交短号:由 CMake 从 `git rev-parse --short=8` 注入,已是 8 位(或 git 为保证
// 唯一而给的更长值)。**不要在这里截断** —— 那会绕开 git 的唯一性保证,并让设备
// 与 OTA manifest 拼出不同长度的字符串,导致每次检查都判定"有新版本"。
const char *sb_version_commit(void)
{
    return SB_GIT_COMMIT;
}

// OTA 身份串。与 tools/make_ota_manifest.py 的 version 字段必须逐字相同。
const char *sb_version_short(void)
{
    // 只在首次调用时拼接,避免每次刷新界面都做格式化。
    static char buf[64];
    if (buf[0] == '\0') {
        snprintf(buf, sizeof(buf), "v%s+%s", SB_APP_VERSION, SB_GIT_COMMIT);
    }
    return buf;
}

const char *sb_version_slot(void)
{
    const esp_partition_t *p = esp_ota_get_running_partition();
    return p ? p->label : "?";
}
