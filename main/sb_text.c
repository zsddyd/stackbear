// main/sb_text.c —— 文本按宽度截断(实现,见 sb_text.h)。
#include "sb_text.h"

#include <string.h>

#define ELLIPSIS "..."
#define ELLIPSIS_LEN (sizeof(ELLIPSIS) - 1)

// 把长度 n 收缩到一个 UTF-8 字符边界:不能把多字节字符切成半个(否则画出来是乱码)。
static size_t utf8_floor(const char *s, size_t n)
{
    while (n > 0 && ((unsigned char)s[n] & 0xC0) == 0x80) {
        n--;   // 落在续字节上,继续往前退
    }
    return n;
}

// 逐字节加长前缀,返回"前缀 + ..."仍放得下的最大前缀长度。宽度随前缀单调不减,
// 一旦超了就可以停 —— 这样不需要假设字体,也不依赖任何测量步长。
static size_t longest_prefix(const char *text, size_t limit, int max_px,
                             sb_text_measure_fn measure, void *ctx)
{
    char buf[160];
    size_t best = 0;
    for (size_t n = 0; n <= limit; n++) {
        if (n < limit && ((unsigned char)text[n] & 0xC0) == 0x80) {
            continue;   // 不停在字符中间
        }
        memcpy(buf, text, n);
        memcpy(buf + n, ELLIPSIS, ELLIPSIS_LEN);
        buf[n + ELLIPSIS_LEN] = '\0';
        if (measure(buf, ctx) <= max_px) {
            best = n;
        } else {
            break;
        }
    }
    return best;
}

int sb_text_fit_ellipsis(char *out, size_t cap, const char *text, int max_px,
                         sb_text_measure_fn measure, void *ctx)
{
    if (out == NULL || cap == 0) {
        return 0;
    }
    out[0] = '\0';
    if (text == NULL || measure == NULL) {
        return 0;
    }
    const size_t len = strlen(text);
    if (len == 0) {
        return 0;
    }

    const size_t room = cap - 1;        // 给结尾 '\0' 留位
    if (measure(text, ctx) <= max_px) {
        const size_t n = len < room ? len : room;
        memcpy(out, text, n);
        out[n] = '\0';
        return (int)n;
    }

    // 放不下:先确认省略号本身放得下,否则宁可给空串 —— 写一个超出宽度的文本
    // 会直接盖到左边的 Key 上,那比空白更糟。
    size_t keep = 0;
    if (measure(ELLIPSIS, ctx) <= max_px) {
        const size_t limit = len < room - ELLIPSIS_LEN ? len : room - ELLIPSIS_LEN;
        keep = longest_prefix(text, utf8_floor(text, limit), max_px, measure, ctx);
        keep = utf8_floor(text, keep);
    }
    memcpy(out, text, keep);
    if (measure(ELLIPSIS, ctx) <= max_px) {
        memcpy(out + keep, ELLIPSIS, ELLIPSIS_LEN);
        out[keep + ELLIPSIS_LEN] = '\0';
        return (int)(keep + ELLIPSIS_LEN);
    }
    out[keep] = '\0';
    return (int)keep;
}

// 环带尾部补两个空格,让"末尾"和"开头"之间有个可见的间隔,不至于连成一串。
#define MARQUEE_GAP "  "

int sb_text_marquee(char *out, size_t cap, const char *src, int off, int max_px,
                    sb_text_measure_fn measure, void *ctx)
{
    if (out == NULL || cap == 0) {
        return 0;
    }
    out[0] = '\0';
    if (src == NULL || measure == NULL) {
        return 0;
    }
    const size_t len = strlen(src);
    if (len == 0) {
        return 0;
    }
    const size_t cycle = len + sizeof(MARQUEE_GAP) - 1;
    if (off < 0) {
        off = 0;
    }
    off = (int)((unsigned)off % cycle);

    // 以环带方式取最多 cap-1 个字符,再交给"截断到宽度"统一收口 —— 这样结尾多余
    // 的部分自然被 ... 收掉,窗口不会超宽。
    char ring[192];
    size_t n = 0;
    for (size_t i = 0; i < cycle && n + 1 < sizeof(ring) && n + 1 < cap; i++) {
        const size_t idx = ((size_t)off + i) % cycle;
        ring[n++] = (idx < len) ? src[idx] : MARQUEE_GAP[idx - len];
    }
    ring[n] = '\0';
    return sb_text_fit_ellipsis(out, cap, ring, max_px, measure, ctx);
}
