// sb_version 的主机测试:版本号、提交号、以及两者拼成的 OTA 身份串。
//
// 身份串是设备判断"是否已是最新"的唯一依据,而它由**两个不同机制**产出:
// 固件里的 sb_version_short() 和 CI 的分支里 tools/make_ota_manifest.py。
// 一旦两边长度或格式不一致,设备就会永远认为有新版本、每次检查都重刷一遍
// (而且刷的还是同一个固件)。这里把格式钉死,CI 的 Python 侧另有
// tests/test_firmware.py 用同一份期望值校验。
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "sb_version.h"

// CMake 在真实构建里注入这几个宏,测试用固定值(见 tools/validate.sh 里的 -D),
// 这样"设备拼出来的身份串"就有了一个可断言的期望。

static void test_number_is_bare(void)
{
    // "Firmware" 行显示的就是它:不带 v、不带 commit。
    assert(strcmp(sb_version_number(), "0.7.10") == 0);
    assert(strchr(sb_version_number(), '+') == NULL);
}

static void test_commit_is_eight_hex_digits(void)
{
    const char *c = sb_version_commit();
    assert(strlen(c) == 8);
    for (int i = 0; i < 8; i++) {
        assert((c[i] >= '0' && c[i] <= '9') || (c[i] >= 'a' && c[i] <= 'f'));
    }
}

static void test_identity_is_the_two_halves(void)
{
    const char *id = sb_version_short();
    assert(strcmp(id, "v0.7.10+df4e58d1") == 0);
    // 身份串必须**恰好**是 "v" + 版本号 + "+" + 提交号,不能再有别的截断。
    char expect[64];
    snprintf(expect, sizeof expect, "v%s+%s", sb_version_number(), sb_version_commit());
    assert(strcmp(id, expect) == 0);
    // 重复调用必须稳定(内部只拼一次)。
    assert(sb_version_short() == id);
}

static void test_slot(void)
{
    const char *slot = sb_version_slot();
    assert(slot != NULL && slot[0] != '\0');
}

int main(void)
{
    test_number_is_bare();
    test_commit_is_eight_hex_digits();
    test_identity_is_the_two_halves();
    test_slot();
    puts("test_sb_version: PASS");
    return 0;
}
