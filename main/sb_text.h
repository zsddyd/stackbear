// main/sb_text.h —— 把过长的文本压进给定像素宽度(纯逻辑,不依赖 SDK/LVGL)。
//
// 为什么单独成文件:两处都需要"太长就截断并以 ... 结尾" —— DEVICE 页的 Branch
// 在未选中时只能显示安全长度的前缀,OTA 的失败原因也可能长过一整行。截断的边界
// (刚好放得下、放不下一个字符、连 ... 都放不下、UTF-8 不能从半个字符切开)全是
// 纯算术,却最容易写错。把"量宽度"做成回调,这里就只剩逻辑,可以主机穷举。
#pragma once

#include <stddef.h>

// 量一段文本需要的像素宽度。设备上是 lv_text_get_size(),主机测试里是"字符数×步长"。
typedef int (*sb_text_measure_fn)(const char *text, void *ctx);

// 把 text 压进 max_px:放得下就原样拷贝;放不下就保留最长的前缀并以 "..." 结尾。
//
// 语义细节:
//   - 返回写入 out 的字节数(不含结尾 '\0')。
//   - 连 "..." 都放不下时,若 "." 都放不下则写空串 —— 宁可空白,也不写出超宽的文本。
//   - 不会从多字节 UTF-8 字符中间切开(前缀长度按字符边界回退)。
//   - out 空间不足时按 cap-1 截断,始终以 '\0' 结尾。
//   - text 为 NULL 视为空串。
int sb_text_fit_ellipsis(char *out, size_t cap, const char *text, int max_px,
                         sb_text_measure_fn measure, void *ctx);

// 跑马灯窗口:把 src 当成环带(尾部接两个空格再接一遍 src),从 off 处取一段压进
// max_px。off 按字节自增,调用方每次刷新 +1 即可,走完一遍就从头开始 —— 这样每个
// 字符都会轮到最左边,长分支名总能看到全。
//
// 为什么不直接用 LVGL 的 SCROLL_CIRCULAR:那个模式在文字被重新 set_text 时会重启
// 动画,而本应用每 500ms 刷一次值,结果就是永远停在开头(实测确认)。自己按 tick
// 推进偏移既能与刷新节奏对齐,也能被主机测试穷举。
//
// 返回写入 out 的字节数。src 为 NULL/空时写空串。
int sb_text_marquee(char *out, size_t cap, const char *src, int off, int max_px,
                    sb_text_measure_fn measure, void *ctx);
