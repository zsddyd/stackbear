// sb_text 的主机测试:按像素宽度截断。测量函数用"每字符固定步长",这样宽度是
// 确定的,边界(刚好放得下 / 差一个字符 / 连 ... 都放不下 / UTF-8 不从中间切)
// 都能穷举。
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "sb_text.h"

// 等宽测量:每个字节 10px。ASCII 单字节字符因此等于每个字符 10px。
static int measure(const char *text, void *ctx)
{
    (void)ctx;
    return (int)strlen(text) * 10;
}

// 只把多字节字符按"2 字节 = 20px"量,用来验 UTF-8 边界。
static int measure_utf8(const char *text, void *ctx)
{
    (void)ctx;
    int px = 0;
    for (const unsigned char *p = (const unsigned char *)text; *p;) {
        if ((*p & 0x80) == 0) {
            p += 1;
        } else if ((*p & 0xE0) == 0xC0) {
            p += 2;
        } else {
            p += 3;
        }
        px += 10;
    }
    return px;
}

static void test_fits_untouched(void)
{
    char out[64];
    // 刚好放得下:不动它。
    assert(sb_text_fit_ellipsis(out, sizeof out, "main", 40, measure, NULL) == 4);
    assert(strcmp(out, "main") == 0);
    // 富余量也不动它。
    assert(sb_text_fit_ellipsis(out, sizeof out, "main", 999, measure, NULL) == 4);
    assert(strcmp(out, "main") == 0);
    // 空串、NULL 都安全。
    assert(sb_text_fit_ellipsis(out, sizeof out, "", 40, measure, NULL) == 0);
    assert(strcmp(out, "") == 0);
    assert(sb_text_fit_ellipsis(out, sizeof out, NULL, 40, measure, NULL) == 0);
    assert(strcmp(out, "") == 0);
}

static void test_truncates_with_ellipsis(void)
{
    char out[64];
    const char *branch = "feature/always-on-recorder";   // 26 字节 = 260px
    // 给 120px:前缀 + "..." 最多 12 字节 → 前缀 9 字节("feature/a")。
    int n = sb_text_fit_ellipsis(out, sizeof out, branch, 120, measure, NULL);
    assert(n == 12);
    assert(strcmp(out, "feature/a...") == 0);
    assert(measure(out, NULL) <= 120);

    // 给 60px:前缀 3 字节 + "..." = "fea..."。
    assert(sb_text_fit_ellipsis(out, sizeof out, branch, 60, measure, NULL) == 6);
    assert(strcmp(out, "fea...") == 0);
    assert(measure(out, NULL) <= 60);

    // 结果必须永远不超宽 —— 这是这个函数唯一的硬约束。
    for (int px = 0; px <= 300; px += 1) {
        const int wrote = sb_text_fit_ellipsis(out, sizeof out, branch, px, measure, NULL);
        assert(wrote >= 0 && (size_t)wrote < sizeof out);
        assert((int)strlen(out) == wrote);
        assert(measure(out, NULL) <= px || wrote == 0);
    }
}

static void test_ellipsis_itself_does_not_fit(void)
{
    char out[64];
    // 30px 只放得下 "...":前缀为空。
    assert(sb_text_fit_ellipsis(out, sizeof out, "feature/x", 30, measure, NULL) == 3);
    assert(strcmp(out, "...") == 0);
    // 20px 连 "..." 都放不下:宁可空串,也不能写出 30px 的文本盖到左边。
    assert(sb_text_fit_ellipsis(out, sizeof out, "feature/x", 20, measure, NULL) == 0);
    assert(strcmp(out, "") == 0);
    assert(sb_text_fit_ellipsis(out, sizeof out, "feature/x", 0, measure, NULL) == 0);
    assert(strcmp(out, "") == 0);
}

static void test_respects_capacity(void)
{
    char out[8];   // 只装得下 7 字节
    const int n = sb_text_fit_ellipsis(out, sizeof out, "feature/always-on-recorder",
                                       9999, measure, NULL);
    assert(n == 7);
    assert(strcmp(out, "feature") == 0);   // 放得下就原样拷,但受容量限制
    char tight[5];
    const int m = sb_text_fit_ellipsis(tight, sizeof tight, "feature/x", 9999, measure, NULL);
    assert(m == 4 && strcmp(tight, "feat") == 0);
}

static void test_utf8_boundary(void)
{
    char out[64];
    // "中文分支名" 是 5 个三字节字符。给 40px = 4 个字 + "..."?4 字 = 40px,
    // 加 ...(30px) 就超了,所以前缀只能小到放得下为止。
    const char *zh = "\xe4\xb8\xad\xe6\x96\x87\xe5\x88\x86\xe6\x94\xaf\xe5\x90\x8d";
    for (int px = 0; px <= 200; px += 1) {
        sb_text_fit_ellipsis(out, sizeof out, zh, px, measure_utf8, NULL);
        // 绝不能把某个字符切成半个:字节数必须是 3 的倍数(要么纯前缀+纯 ...)。
        const char *dots = strstr(out, "...");
        const size_t prefix = dots ? (size_t)(dots - out) : strlen(out);
        assert(prefix % 3 == 0);
        assert(measure_utf8(out, NULL) <= px || out[0] == '\0');
    }
}

static void test_marquee_covers_every_character(void)
{
    char out[64];
    const char *src = "feature/always-on-recorder";
    // 逐格推进;每个字符至少要在窗口里出现过一次 —— 这正是"看全"的含义。
    // (窗口末尾的 ... 是截断收口,不影响"某个字符是否出现过"。)
    int seen[64] = {0};
    const int len = (int)strlen(src);
    for (int off = 0; off < 200; off++) {
        const int n = sb_text_marquee(out, sizeof out, src, off, 130, measure, NULL);
        assert(n >= 0 && (size_t)n < sizeof out);
        assert(measure(out, NULL) <= 130 || n == 0);
        for (int j = 0; j < len; j++) {
            if (strchr(out, src[j]) != NULL) {
                seen[j] = 1;
            }
        }
    }
    for (int j = 0; j < len; j++) {
        assert(seen[j] == 1);
    }
}

static void test_marquee_cycles_and_survives_bad_offsets(void)
{
    char a[64], b[64];
    const char *src = "main";
    // 环带长度 = 文本 + 两个空格 = 6;off 与 off+cycle 必须给出同一窗口。
    sb_text_marquee(a, sizeof a, src, 3, 999, measure, NULL);
    sb_text_marquee(b, sizeof b, src, 3 + 6, 999, measure, NULL);
    assert(strcmp(a, b) == 0);
    // 负偏移与超大偏移都不能读到环带外(只求不越界、不崩)。
    assert(sb_text_marquee(a, sizeof a, src, -5, 999, measure, NULL) >= 0);
    assert(sb_text_marquee(a, sizeof a, src, 100000, 999, measure, NULL) >= 0);
    assert(sb_text_marquee(a, sizeof a, NULL, 0, 100, measure, NULL) == 0);
    assert(strcmp(a, "") == 0);
    assert(sb_text_marquee(a, sizeof a, "", 0, 100, measure, NULL) == 0);
    // 放不下时也要被 ... 收口,不能超宽。
    const int n = sb_text_marquee(a, sizeof a, "feature/always-on-recorder", 0, 50,
                                  measure, NULL);
    assert(measure(a, NULL) <= 50 || n == 0);
}

int main(void)
{
    test_fits_untouched();
    test_truncates_with_ellipsis();
    test_ellipsis_itself_does_not_fit();
    test_respects_capacity();
    test_utf8_boundary();
    test_marquee_covers_every_character();
    test_marquee_cycles_and_survives_bad_offsets();
    puts("test_sb_text: PASS");
    return 0;
}
