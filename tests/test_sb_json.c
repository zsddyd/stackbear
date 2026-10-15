// tests/test_sb_json.c —— OTA 清单字段提取的主机测试。
//
// 这里的第一条用例就是当年让 OTA 一直 "check failed" 的回归:key 后面是冒号而不是
// 直接跟引号。真实 manifest 还带缩进和换行。
#include <stdio.h>
#include <string.h>

#include "sb_json.h"

static int g_failures;

#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);         \
            g_failures++;                                                  \
        }                                                                  \
    } while (0)

static void test_real_manifest(void)
{
    // 与 tools/make_ota_manifest.py 的缩进输出一致(含换行与空格)。
    const char *manifest =
        "{\n"
        "  \"version\": \"v0.5.0+98e800a\",\n"
        "  \"sha256\": \"b773039f9e00c0c2ceb03b2264f0872d59014bee8f2b5571e39c5f03330b5058\",\n"
        "  \"size\": 1417728,\n"
        "  \"url\": \"http://8.137.56.181:18080/firmware/app-98e800a.bin\",\n"
        "  \"published_name\": \"app-98e800a.bin\"\n"
        "}\n";
    char buf[128];

    CHECK(sb_json_str(manifest, "\"version\"", buf, sizeof(buf)));
    CHECK(strcmp(buf, "v0.5.0+98e800a") == 0);

    CHECK(sb_json_str(manifest, "\"sha256\"", buf, sizeof(buf)));
    CHECK(strcmp(buf, "b773039f9e00c0c2ceb03b2264f0872d59014bee8f2b5571e39c5f03330b5058") == 0);

    CHECK(sb_json_str(manifest, "\"url\"", buf, sizeof(buf)));
    CHECK(strcmp(buf, "http://8.137.56.181:18080/firmware/app-98e800a.bin") == 0);
}

static void test_tight_json_no_spaces(void)
{
    // 单行无空格形式(某些服务器会压缩)也必须能取到。
    const char *j = "{\"url\":\"http://x/y.bin\",\"version\":\"v1\"}";
    char buf[64];
    CHECK(sb_json_str(j, "\"url\"", buf, sizeof(buf)));
    CHECK(strcmp(buf, "http://x/y.bin") == 0);
    CHECK(sb_json_str(j, "\"version\"", buf, sizeof(buf)));
    CHECK(strcmp(buf, "v1") == 0);
}

static void test_missing_and_non_string(void)
{
    const char *j = "{\"size\": 1024, \"name\": \"x\"}";
    char buf[32];
    CHECK(!sb_json_str(j, "\"absent\"", buf, sizeof(buf)));   // 键不存在
    CHECK(!sb_json_str(j, "\"size\"", buf, sizeof(buf)));     // 值不是字符串
    CHECK(sb_json_str(j, "\"name\"", buf, sizeof(buf)));
}

static void test_refuses_truncation(void)
{
    // 值比缓冲大时必须失败,而不是返回被截断的 sha256(那会导致校验永远不过)。
    const char *j = "{\"sha256\":\"0123456789abcdef0123456789abcdef\"}";
    char small[8];
    CHECK(!sb_json_str(j, "\"sha256\"", small, sizeof(small)));
}

static void test_first_match_wins(void)
{
    const char *j = "{\"url\":\"first\",\"other\":{\"url\":\"second\"}}";
    char buf[16];
    CHECK(sb_json_str(j, "\"url\"", buf, sizeof(buf)));
    CHECK(strcmp(buf, "first") == 0);
}

int main(void)
{
    test_real_manifest();
    test_tight_json_no_spaces();
    test_missing_and_non_string();
    test_refuses_truncation();
    test_first_match_wins();
    if (g_failures == 0) {
        printf("test_sb_json: PASS\n");
        return 0;
    }
    printf("test_sb_json: %d failure(s)\n", g_failures);
    return 1;
}
