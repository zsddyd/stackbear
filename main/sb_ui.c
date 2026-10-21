// main/sb_ui.c —— 应用界面。刻意区别于基线 demo 的"像素菜单":深色底 + 圆角卡片 +
// 单一主状态,电量固定右上角。
//
// 结构:HOME 独占聚合状态卡,并在下方放两行可选动作(开始/停止录音、进设置);
// SETTINGS 是分组菜单;四个子页 + HELP 各讲一层信息。
// 所有列表页共用同一个 list_page_t + 通用刷新,避免每页重复样板代码。
#include "sb_ui.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "bsp_battery.h"
#include "bsp_display.h"
#include "esp_log.h"
#include "sb_cache.h"
#include "sb_config.h"
#include "sb_display_cfg.h"
#include "sb_nav.h"
#include "sb_net.h"
#include "sb_ota.h"
#include "sb_recorder.h"
#include "sb_scroll.h"
#include "sb_text.h"
#include "sb_uplink.h"
#include "sb_version.h"

static const char *TAG = "sb_ui";

// ---------------------------------------------------------------------------
// LVGL 池余量观测。
//
// 为什么需要它:LVGL 的静态池(见 sdkconfig.defaults 的 CONFIG_LV_MEM_SIZE)用尽时
// 不会崩溃报错 —— lv_malloc 返回 NULL,而池内多处调用(例如 lv_draw_add_task 写
// new_task->area)不检查返回值,直接写 NULL+偏移,于是表现为一次莫名的
// "Store access fault" 复位,而不是一句"内存不够"。
//
// 这块板子没有 PSRAM,池大小是静态切走的、不与系统堆共享,所以唯一的判据是实测:
// free_size 是当前余量,free_biggest_size 是**最大连续块**(决定"还能不能放下一个
// 大对象",比 free_size 更贴近崩溃条件),max_used 是历史高水位。三者一起看才分得清
// "池太小"和"碎片太多"。
//
// 用 ESP_LOGI 而不是 LVGL 自己的日志:两者互补 —— LVGL 日志(现已开启,见
// sdkconfig.defaults)只在失败那一刻说话,而这几行是**主动采样**,开机就能看到
// 余量趋势,不必等到崩了才有数据。
// ---------------------------------------------------------------------------
static void ui_mem_log(const char *where)
{
    lv_mem_monitor_t m;
    lv_mem_monitor(&m);
    ESP_LOGI(TAG, "LVGL 池[%s]: 总 %u B 空闲 %u B 最大连续 %u B 峰值 %u B "
                  "(已用 %u%%,碎片 %u%%)",
             where, (unsigned)m.total_size, (unsigned)m.free_size,
             (unsigned)m.free_biggest_size, (unsigned)m.max_used,
             (unsigned)m.used_pct, (unsigned)m.frag_pct);
}

// 调色板:一个地面、一个墨色、一个强调色 —— 不做渐变/发光/阴影。
#define C_BG      0x0E1216
#define C_CARD    0x1B222B
#define C_SEL     0x24303F   // 选中高亮的底色:比卡片亮一档,而不是加边框
#define C_TEXT    0xEDF2F7
#define C_MUTED   0x8A97A6
#define C_OK      0x35C46A
#define C_WARN    0xE8A33D
#define C_BAD     0xE0524A
#define C_ACCENT  0x3B82F6

// 破坏性动作的强调色。单独一个名字,是为了让"红色 = 会中断录音"这条约定只有
// 一处定义 —— 换配色时不会漏掉某个页面。
#define C_DANGER  C_BAD

// 一屏能放下的行数由几何决定:最后一行的底边必须落在屏幕底部圆角之上。这是
// **可视窗口**的大小,不是"页面条目上限"——条目可以更多,超出的部分交给
// sb_scroll 滚动(见 page_render_window)。
#define LIST_SLOTS 7
#define ROW_X      12
#define ROW_Y0     38
#define ROW_PITCH  34
#define ROW_H      30
#define ROW_W      216
#define SEL_MS     150   // 选中高亮滑动时长;按键连击时只重启这一个动画

// ---------------------------------------------------------------------------
// 通用控件
// ---------------------------------------------------------------------------

// 三个控件工厂的 NULL 约定:创建失败时 lv_obj_create()/lv_label_create() 返回 NULL。
// 后续的 lv_obj_set_*(o, ...) 内部虽有 LV_CHECK_OBJ 判空(默认 LV_USE_CHECK_ARG=y),
// 但**返回值会被继续使用**的地方(如把自己存进 s_batt_label[]、s_home_state 等句柄,
// 再由 refresh() 每 500ms 解引用)仍会踩空指针。所以工厂统一返回 NULL,调用点要么
// 接受"这个控件没画出来",要么显式判空。
//
// 与断言的关系:sdkconfig.defaults 现在开了 CONFIG_LV_USE_ASSERT_MALLOC,池耗尽时
// LVGL 会在 lv_malloc 处先断言停机并打印 "Out of memory"(可诊断),这些判空因此是
// **第二道防线**;若将来关掉断言(或失败来自非池原因),它们就是唯一的防线。
//
// 注意:这些判空**挡不住** LVGL 内部不检查返回值的路径(例如渲染时的
// lv_draw_add_task,见 v0.7.6 那次 Store access fault)。那类路径只能靠断言暴露、
// 靠"池足够大"避免 —— 所以真正的修复仍然是让池够用(见 ui_mem_log 的实测)。
static lv_obj_t *card(lv_obj_t *parent, int x, int y, int w, int h, uint32_t color)
{
    // 同 label():parent == NULL 会被 LVGL 当成"创建屏幕",必须挡在这里。
    if (parent == NULL) {
        return NULL;
    }
    lv_obj_t *o = lv_obj_create(parent);
    if (o == NULL) {
        ESP_LOGE(TAG, "创建卡片失败(池耗尽): %dx%d", w, h);
        return NULL;
    }
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_radius(o, 10, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    lv_obj_set_style_bg_color(o, lv_color_hex(color), 0);
    return o;
}

static lv_obj_t *label(lv_obj_t *parent, const char *text, const lv_font_t *font, uint32_t color)
{
    // parent == NULL 必须挡在这里:LVGL 把"父对象为 NULL"解释成"创建一个屏幕",
    // 于是上游一次失败的创建会在这里变出一个游离屏幕对象,而不是安静地失败。
    if (parent == NULL) {
        return NULL;
    }
    lv_obj_t *l = lv_label_create(parent);
    if (l == NULL) {
        ESP_LOGE(TAG, "创建文本失败(池耗尽): \"%s\"", text);
        return NULL;
    }
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    return l;
}

// 每个页面右上角都要有电量,所以它在这里统一创建 —— 而不是让每个页面自己记得加。
// 曾经只有 HOME 有,用户一进子页就看不到电量了(而"还剩多少电"恰恰是子页里最想
// 顺手确认的一件事)。每块屏幕是独立对象,不能共用一个标签,所以登记下来统一刷新。
#define BATT_MAX_SCREENS 9   // HOME + 6 个设置页 + HELP + 配网

static lv_obj_t *s_batt_label[BATT_MAX_SCREENS];
static int s_batt_count;

static lv_obj_t *base_screen(const char *title)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    if (scr == NULL) {
        ESP_LOGE(TAG, "创建屏幕失败(池耗尽): %s", title);
        return NULL;
    }
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(scr, lv_color_hex(C_BG), 0);
    lv_obj_set_style_border_width(scr, 0, 0);
    lv_obj_set_style_pad_all(scr, 0, 0);
    lv_obj_t *t = label(scr, title, &lv_font_montserrat_14, C_MUTED);
    if (t != NULL) {
        lv_obj_set_pos(t, 12, 10);
    }

    lv_obj_t *b = label(scr, "--%", &lv_font_montserrat_14, C_TEXT);
    if (b != NULL) {
        lv_obj_align(b, LV_ALIGN_TOP_RIGHT, -12, 10);
        if (s_batt_count < BATT_MAX_SCREENS) {
            s_batt_label[s_batt_count++] = b;
        }
    }
    return scr;
}

// 一次更新所有页面的电量。最宽的内容是 "100%"(35px),右对齐到 x=228,所以它占
// x=193..228;最长的标题 "STACKBEAR RECORDER" 到 x=185,两者不重叠。
static void battery_refresh(void)
{
    const int soc = bsp_battery_soc();
    for (int i = 0; i < s_batt_count; i++) {
        if (s_batt_label[i] == NULL) {
            continue;   // 该页的电量标签没建出来(池耗尽)
        }
        if (soc < 0) {
            lv_label_set_text(s_batt_label[i], "--%");
        } else {
            lv_label_set_text_fmt(s_batt_label[i], "%d%%", soc);
        }
    }
}

// ---------------------------------------------------------------------------
// 列表页:一行 = 名称(左)+ 值(右)。
//
// 光标由一个独立的高亮矩形表示,它在行之间缓动。绘制顺序用三级层次实现,
// 这样"选中行底色亮一档"的原始外观完全不变(见 house style:用底色而不是描边):
//
//   1. 每行的底色矩形(C_CARD,不透明)——先创建,在最底层
//   2. 高亮矩形(C_SEL,不透明)——后创建,盖在选中那行的底色之上
//   3. 名称/数值标签——最后创建且父对象是屏幕,因此画在高亮之上
//
// 标签不能放在行底色里当子对象:那样它们会先于第 2 层绘制而被高亮盖住。所以
// 标签挂在屏幕上,再用 lv_obj_align() 按行坐标对齐。
//
// 为什么不是 lv_obj_align_to():它按调用瞬间目标的**当前尺寸**算出绝对坐标并
// 直接落位,而数值标签建出来时还是占位符 "-"。之后 refresh() 把文字换成更长的
// 内容时,标签会以那个旧锚点为左边界向右扩张,右端被裁到屏幕外(这就是"每行
// 后边的值只露出前几个字母"的原因)。lv_obj_align() 设置的是对齐样式,布局每
// 次都会按当前尺寸重算,所以文字再长也仍贴着行右边缘,不会跑出屏幕。
// 行与屏幕的几何在同一屏内是常量,直接按行索引算 y 即可,不需要行的对象句柄。
// ---------------------------------------------------------------------------

// 一个列表页。**窗口是虚拟的**:无论页面声明多少条目,只创建 LIST_SLOTS 个行槽,
// 滚动时把槽重新指向 [scroll, scroll+LIST_SLOTS) 区间的条目。这样对象数不随条目
// 增长(7 行 × 3 个对象是固定成本),而页面想加多少行都行 —— 这也是"滚动能力
// 通用化"的关键:Network/Recorder/Device/Storage 今天最多 7 行,以后加内容自然就滚。
typedef struct {
    lv_obj_t *scr;
    lv_obj_t *sel;                        // 选中高亮矩形(C_SEL)
    lv_obj_t *rows[LIST_SLOTS];           // 行槽底色矩形(跟随滚动重排)
    lv_obj_t *names[LIST_SLOTS];          // 行槽名称标签
    lv_obj_t *vals[LIST_SLOTS];           // 行槽数值标签
    lv_obj_t *bar_track;                  // 滚动条轨道(内容不超屏时隐藏)
    lv_obj_t *bar_thumb;                  // 滚动条滑块
    const char *const *item_names;        // 该页的条目名(条目下标 -> 文案)
    sb_nav_page_t nav;                    // 该页在 sb_nav 里的身份:行是否可操作由它决定
    int slots;                            // 实际创建的行槽数 = min(条目数, LIST_SLOTS)
    sb_scroll_t win;                      // 条目数 / 可视行数 / 光标 / 窗口起点
} list_page_t;

static list_page_t s_menu, s_net, s_rec, s_dev, s_stor, s_disp;

// ---------------------------------------------------------------------------
// 滚动条指示。列表页与 Help 页共用:只在"内容确实超出可视区"时出现,否则整条
// 隐藏。用几何图形而不是箭头字符,是因为默认字体有没有那些字形不受我们控制
// (见"中文字形"那类坑),而矩形加偏移是纯算术,任何字体下都一样。
// ---------------------------------------------------------------------------

#define BAR_X      232
#define BAR_W      3
#define BAR_MIN_H  10

// 列表页的轨道纵向范围 = 行区范围:ROW_Y0 起,(LIST_SLOTS-1) 个行距 + 一行高。
#define BAR_TRACK_Y ROW_Y0
#define BAR_ROW_H   ((LIST_SLOTS - 1) * ROW_PITCH + ROW_H)

// offset/count 是条目口径(列表页)或像素口径(Help 页)都行 —— 这里只做比例映射。
static void bar_apply(lv_obj_t *track, lv_obj_t *thumb, int track_h,
                      int offset, int count, int view)
{
    if (track == NULL || thumb == NULL) {
        return;
    }
    if (view <= 0 || count <= view) {
        lv_obj_set_hidden(track, true);
        lv_obj_set_hidden(thumb, true);
        return;
    }
    lv_obj_set_hidden(track, false);
    lv_obj_set_hidden(thumb, false);

    int thumb_h = (int)((long)track_h * view / count);
    if (thumb_h < BAR_MIN_H) {
        thumb_h = BAR_MIN_H;
    }
    if (thumb_h > track_h) {
        thumb_h = track_h;
    }
    const int max_off = count - view;
    int y = 0;
    if (max_off > 0) {
        const int travel = track_h - thumb_h;
        if (offset < 0) {
            offset = 0;
        }
        if (offset > max_off) {
            offset = max_off;
        }
        y = (int)((long)travel * offset / max_off);
    }
    lv_obj_set_pos(thumb, BAR_X, BAR_TRACK_Y + y);
    lv_obj_set_size(thumb, BAR_W, thumb_h);
}

// 建一对轨道/滑块。轨道常驻(内容不超屏时隐藏),滑块是它上面的一个小矩形。
static void bar_create(lv_obj_t *parent, uint32_t track_color, uint32_t thumb_color,
                       int track_h, lv_obj_t **track_out, lv_obj_t **thumb_out)
{
    lv_obj_t *t = card(parent, BAR_X, BAR_TRACK_Y, BAR_W, track_h, track_color);
    lv_obj_t *b = card(parent, BAR_X, BAR_TRACK_Y, BAR_W, BAR_MIN_H, thumb_color);
    // 两者可能都是 NULL(card() 已判空并打过日志);bar_apply() 自己能处理 NULL,
    // 这里只需避免对 NULL 调 lv_obj_set_hidden。
    if (t != NULL) {
        lv_obj_set_hidden(t, true);
    }
    if (b != NULL) {
        lv_obj_set_hidden(b, true);
    }
    *track_out = t;
    *thumb_out = b;
}

// 一行的底色矩形。只负责画那一行的卡片轮廓,不承载子对象。
static lv_obj_t *row_bg(lv_obj_t *parent, int y)
{
    if (parent == NULL) {
        return NULL;
    }
    lv_obj_t *o = lv_obj_create(parent);
    if (o == NULL) {
        return NULL;
    }
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(o, ROW_X, y);
    lv_obj_set_size(o, ROW_W, ROW_H);
    lv_obj_set_style_radius(o, 10, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    lv_obj_set_style_bg_color(o, lv_color_hex(C_CARD), 0);
    return o;
}

// 把窗口内的行槽指向 [scroll, scroll+LIST_SLOTS) 的条目。窗口外的槽整只隐藏 ——
// 对象数固定,藏起来的槽下次滚动会立刻复用,所以条目再多也不增加对象。
static void page_render_window(list_page_t *p)
{
    for (int k = 0; k < p->slots; k++) {
        const int item = p->win.scroll + k;
        const bool show = item < p->win.count;
        lv_obj_t *objs[3] = {p->rows[k], p->names[k], p->vals[k]};
        for (int i = 0; i < 3; i++) {
            if (objs[i] == NULL) {
                continue;
            }
            if (show) {
                lv_obj_set_hidden(objs[i], false);
            } else {
                lv_obj_set_hidden(objs[i], true);
            }
        }
        if (!show) {
            continue;
        }
        // 名称随窗口滑入滑出:可操作与否只问 sb_nav,界面不重复定义规则。
        lv_label_set_text(p->names[k], p->item_names[item]);
        lv_obj_set_style_text_color(p->names[k],
                                    lv_color_hex(sb_nav_row_actionable(p->nav, item)
                                                     ? C_ACCENT
                                                     : C_MUTED),
                                    0);
        // 数值留空:真正的内容由 refresh_* 每 500ms 填,滚动后由 page_move 立刻
        // 补一次,避免滑进来的行短暂显示上一行的残留值。
        lv_label_set_text(p->vals[k], "");
    }
}

static void page_init(list_page_t *p, sb_nav_page_t nav, const char *title,
                      const char *const *names, int count)
{
    p->nav = nav;
    p->item_names = names;
    // 只建页面真正需要的槽:条目少的页(4 行)没必要养 7 套对象。窗口上限仍是
    // LIST_SLOTS,所以条目再多也只是滚动,不会再增加对象。
    p->slots = count < LIST_SLOTS ? count : LIST_SLOTS;
    p->win.count = count;
    p->win.visible = p->slots;
    p->win.cursor = 0;
    p->win.scroll = 0;
    sb_scroll_follow(&p->win);
    p->scr = base_screen(title);
    // 屏幕没建出来时,后续所有 lv_obj_create(p->scr) 都会把 NULL 父对象解释成
    // "再建一个屏幕" —— 那不是失败,而是悄悄多出几个游离屏幕对象,比直接失败更糟。
    // 所以这里必须早退,把整页留成"空页"。
    if (p->scr == NULL) {
        p->sel = NULL;
        p->bar_track = NULL;
        p->bar_thumb = NULL;
        for (int i = 0; i < p->slots; i++) {
            p->rows[i] = NULL;
            p->names[i] = NULL;
            p->vals[i] = NULL;
        }
        return;
    }

    // 第 1 层:行槽的卡片底色。槽数 = 该页可视行数,与滚动无关。
    for (int i = 0; i < p->slots; i++) {
        // 只需要对象存在,后续不再引用:标签按行索引算坐标,不依赖句柄。
        p->rows[i] = row_bg(p->scr, ROW_Y0 + i * ROW_PITCH);
    }

    // 第 2 层:选中高亮。它在行底色之后创建,所以盖在选中行的底色上;几何与
    // 行底色完全一致(同样的 x/y/w/h 与圆角),视觉上等同于把那一行的底色换成
    // C_SEL,但它是一个可以整体缓动的独立对象。
    p->sel = lv_obj_create(p->scr);
    if (p->sel != NULL) {
        lv_obj_remove_flag(p->sel, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_pos(p->sel, ROW_X, ROW_Y0);
        lv_obj_set_size(p->sel, ROW_W, ROW_H);
        lv_obj_set_style_radius(p->sel, 10, 0);
        lv_obj_set_style_border_width(p->sel, 0, 0);
        lv_obj_set_style_pad_all(p->sel, 0, 0);
        lv_obj_set_style_bg_color(p->sel, lv_color_hex(C_SEL), 0);
    }

    // 第 3 层:标签。挂在屏幕上(而非行底色里)才能绘制在高亮之上。
    for (int i = 0; i < p->slots; i++) {
        // 行垂直中心。字号固定为 14,用它的行高把文字居中。
        const int y_c = ROW_Y0 + i * ROW_PITCH + ROW_H / 2;
        p->names[i] = label(p->scr, "", &lv_font_montserrat_14, C_MUTED);
        lv_obj_t *n = p->names[i];
        if (n != NULL) {
            lv_obj_align(n, LV_ALIGN_TOP_LEFT, ROW_X + 10,
                         y_c - lv_font_get_line_height(&lv_font_montserrat_14) / 2);
        }
        p->vals[i] = label(p->scr, "", &lv_font_montserrat_14, C_TEXT);
        // 右边缘对齐到行右侧内缩 10px 处。偏移量从屏幕右边缘往前算,所以不写死
        // 屏幕宽度:屏幕是 240 宽时 240-(12+216-10)=22。
        if (p->vals[i] != NULL) {
            lv_obj_align(p->vals[i], LV_ALIGN_TOP_RIGHT,
                         -(lv_obj_get_width(p->scr) - (ROW_X + ROW_W - 10)),
                         y_c - lv_font_get_line_height(&lv_font_montserrat_14) / 2);
        }
    }

    bar_create(p->scr, C_CARD, C_SEL, BAR_ROW_H, &p->bar_track, &p->bar_thumb);
    page_render_window(p);
    bar_apply(p->bar_track, p->bar_thumb, BAR_ROW_H,
              p->win.scroll, p->win.count, p->slots);
}

// value 按"条目下标"寻址;落在窗口外的条目直接丢弃 —— 它会随着滚动被重新填。
static void page_set(list_page_t *p, int item, const char *value, uint32_t color)
{
    const int slot = item - p->win.scroll;
    if (slot < 0 || slot >= p->slots || p->vals[slot] == NULL) {
        return;
    }
    lv_label_set_text(p->vals[slot], value);
    lv_obj_set_style_text_color(p->vals[slot], lv_color_hex(color), 0);
}

static void page_set_fmt(list_page_t *p, int row, uint32_t color, const char *fmt, ...)
{
    char buf[64];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    page_set(p, row, buf, color);
}

static void sel_set_y(void *obj, int32_t y)
{
    lv_obj_set_y((lv_obj_t *)obj, y);
}

// 把选中高亮缓动到当前光标行。先删掉在跑的动画再从当前 y 重新滑:连按按键时
// 不会堆积动画节点,也不会从起始行回跳(起点取的是动画此刻的位置)。
static void page_slide_sel(list_page_t *p)
{
    if (p->sel == NULL) {
        return;
    }
    // 高亮停在**槽位**上,不是条目下标上:条目 item 在窗口里的槽是 item - scroll。
    const int32_t target = ROW_Y0 + (p->win.cursor - p->win.scroll) * ROW_PITCH;
    const int32_t from = lv_obj_get_y(p->sel);
    if (from == target) {
        return;
    }
    lv_anim_delete(p->sel, sel_set_y);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, p->sel);
    lv_anim_set_exec_cb(&a, sel_set_y);
    lv_anim_set_values(&a, from, target);
    lv_anim_set_duration(&a, SEL_MS);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
}

// 该页由哪个 refresh_* 填值。滚动后要立刻补一次,否则滑进窗口的行是空白的。
typedef void (*page_refresh_fn)(void);
static page_refresh_fn refresh_for(sb_screen_t screen);

static void page_move(list_page_t *p, int delta, sb_screen_t screen)
{
    const int before = p->win.cursor;
    sb_scroll_move(&p->win, delta);      // 夹边界 + 窗口跟随,边界情形全在 sb_scroll 里
    if (p->win.cursor == before) {
        return;                          // 已到边界:连刷新和动画都不做
    }
    page_render_window(p);
    // 立刻用实时状态填一次新滑进来的行,不等下一个 500ms 刷新。
    const page_refresh_fn rf = refresh_for(screen);
    if (rf != NULL) {
        rf();
    }
    page_slide_sel(p);
    bar_apply(p->bar_track, p->bar_thumb, BAR_ROW_H,
              p->win.scroll, p->win.count, p->slots);
}

// ---------------------------------------------------------------------------
// HOME:聚合状态 + 三个计数
// ---------------------------------------------------------------------------

static lv_obj_t *s_home_scr;
static lv_obj_t *s_home_state;
static lv_obj_t *s_home_sub;
static lv_obj_t *s_home_seg;
static lv_obj_t *s_home_sent;
static lv_obj_t *s_home_drop;

// HOME 也要能选东西(Stop / Settings),所以它复用同一套"行 + 高亮"模型。区别只有
// 一处:它固定两行、排在计数器卡片下面,且可操作行的名称用红色(危险)/强调色(普通)
// 区分 —— 表格化的列表用不上这种"这一行是危险动作"的强调。
#define HOME_ROW_Y0 240   // 计数器卡片下方;末行底边 304,仍在底部圆角之内
#define HOME_ROWS   2

static lv_obj_t *s_home_sel;
static lv_obj_t *s_home_row_name[HOME_ROWS];
static int s_home_cursor;

static void build_home(void)
{
    s_home_scr = base_screen("STACKBEAR RECORDER");   // 右上角的电量由 base_screen 建
    if (s_home_scr == NULL) {
        return;   // 池已耗尽:不建 HOME,由 sb_ui_init 的日志说明原因
    }

    lv_obj_t *c = card(s_home_scr, 12, 44, 216, 124, C_CARD);
    s_home_state = label(c, "STARTING", &lv_font_montserrat_20, C_TEXT);
    if (s_home_state != NULL) {
        lv_obj_align(s_home_state, LV_ALIGN_TOP_MID, 0, 34);
    }
    s_home_sub = label(c, "", &lv_font_montserrat_14, C_MUTED);
    if (s_home_sub != NULL) {
        lv_label_set_long_mode(s_home_sub, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(s_home_sub, 196);
        lv_obj_set_style_text_align(s_home_sub, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(s_home_sub, LV_ALIGN_BOTTOM_MID, 0, -12);
    }

    static const char *caps[3] = {"SEG", "SENT", "DROP"};
    lv_obj_t **vals[3] = {&s_home_seg, &s_home_sent, &s_home_drop};
    for (int i = 0; i < 3; i++) {
        lv_obj_t *k = card(s_home_scr, 12 + i * 74, 178, 68, 56, C_CARD);
        lv_obj_t *cap = label(k, caps[i], &lv_font_montserrat_14, C_MUTED);
        if (cap != NULL) {
            lv_obj_align(cap, LV_ALIGN_TOP_MID, 0, 6);
        }
        *vals[i] = label(k, "0", &lv_font_montserrat_14, C_TEXT);
        if (*vals[i] != NULL) {
            lv_obj_align(*vals[i], LV_ALIGN_BOTTOM_MID, 0, -8);
        }
    }

    // 两行可选动作。和高亮/标签的层次一样:底色 -> 高亮 -> 标签,标签挂屏幕才盖得住
    // 高亮。
    // 第一行的动词随录音状态变(Start / Stop),文字在 refresh_home_row() 里更新;
    // 这里只建对象与配色。红色只给"会打断录音"的那一行。
    static const char *names[HOME_ROWS] = {"Start recording", "Settings"};
    for (int i = 0; i < HOME_ROWS; i++) {
        (void)row_bg(s_home_scr, HOME_ROW_Y0 + i * ROW_PITCH);
    }
    s_home_sel = lv_obj_create(s_home_scr);
    if (s_home_sel != NULL) {
        lv_obj_remove_flag(s_home_sel, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_pos(s_home_sel, ROW_X, HOME_ROW_Y0);
        lv_obj_set_size(s_home_sel, ROW_W, ROW_H);
        lv_obj_set_style_radius(s_home_sel, 10, 0);
        lv_obj_set_style_border_width(s_home_sel, 0, 0);
        lv_obj_set_style_pad_all(s_home_sel, 0, 0);
        lv_obj_set_style_bg_color(s_home_sel, lv_color_hex(C_SEL), 0);
    }
    for (int i = 0; i < HOME_ROWS; i++) {
        const int y_c = HOME_ROW_Y0 + i * ROW_PITCH + ROW_H / 2;
        s_home_row_name[i] = label(s_home_scr, names[i], &lv_font_montserrat_14,
                                   i == 0 ? C_DANGER : C_ACCENT);
        if (s_home_row_name[i] != NULL) {
            lv_obj_align(s_home_row_name[i], LV_ALIGN_TOP_LEFT, ROW_X + 10,
                         y_c - lv_font_get_line_height(&lv_font_montserrat_14) / 2);
        }
    }
    s_home_cursor = 0;
}

// HOME 的光标滑动,与列表页同一个做法(从动画当前位置重设,连按不堆积)。
static void home_move(int delta)
{
    const int before = s_home_cursor;
    s_home_cursor += delta;
    if (s_home_cursor < 0) {
        s_home_cursor = 0;
    }
    if (s_home_cursor >= HOME_ROWS) {
        s_home_cursor = HOME_ROWS - 1;
    }
    if (s_home_cursor == before || s_home_sel == NULL) {
        return;
    }
    const int32_t target = HOME_ROW_Y0 + s_home_cursor * ROW_PITCH;
    const int32_t from = lv_obj_get_y(s_home_sel);
    lv_anim_delete(s_home_sel, sel_set_y);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_home_sel);
    lv_anim_set_exec_cb(&a, sel_set_y);
    lv_anim_set_values(&a, from, target);
    lv_anim_set_duration(&a, SEL_MS);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
}

// HOME 当前选中的动作。main 只问"用户选了什么",不关心它是第几行 —— "哪一行是
// 什么"仍然只在界面里定义一次。
sb_home_action_t sb_ui_home_action(void)
{
    return s_home_cursor == 0 ? SB_HOME_STARTSTOP : SB_HOME_SETTINGS;
}

// HOME 的状态是【聚合结论】:融合凭据、WiFi 链路、暂停、上行四层判断。
// 子页只看自己那一层,所以这个函数只在 HOME 用。
static const char *state_text(uint32_t *color)
{
    if (!sb_config_get()->provisioned) {
        *color = C_WARN;
        return "NOT SET UP";
    }
    switch (sb_net_state()) {
    case SB_NET_UP:
        break;
    case SB_NET_CONNECTING:
        *color = C_WARN;
        return "CONNECTING";
    default:
        if (sb_net_reconnecting()) {   // 退避重连中:不是故障
            *color = C_WARN;
            return "RECONNECTING";
        }
        *color = C_BAD;
        return "NO WI-FI";
    }
    if (sb_recorder_paused()) {
        *color = C_WARN;
        return "PAUSED";
    }
    switch (sb_uplink_state()) {
    case SB_UL_UP:
        *color = C_OK;
        return "RECORDING";
    case SB_UL_CONNECTING:
        *color = C_WARN;
        return "LINKING";
    case SB_UL_ERROR:
        *color = C_BAD;
        return "UPLOAD ERR";
    default:
        *color = C_WARN;
        return "IDLE";
    }
}

// ---------------------------------------------------------------------------
// 配网页
// ---------------------------------------------------------------------------

static lv_obj_t *s_prov_scr;
static lv_obj_t *s_prov_ap;
static char s_ap_name[40] = "";

static void build_provision(void)
{
    s_prov_scr = base_screen("SETUP MODE");
    if (s_prov_scr == NULL) {
        return;
    }
    lv_obj_t *c = card(s_prov_scr, 12, 44, 216, 176, C_CARD);
    lv_obj_t *l1 = label(c, "Join this Wi-Fi hotspot:", &lv_font_montserrat_14, C_MUTED);
    if (l1 != NULL) {
        lv_obj_align(l1, LV_ALIGN_TOP_LEFT, 14, 14);
    }
    s_prov_ap = label(c, "Passport-XXXX", &lv_font_montserrat_20, C_OK);
    if (s_prov_ap != NULL) {
        lv_obj_align(s_prov_ap, LV_ALIGN_TOP_LEFT, 14, 40);
    }
    lv_obj_t *l2 = label(c, "Password: passport1234", &lv_font_montserrat_14, C_TEXT);
    if (l2 != NULL) {
        lv_obj_align(l2, LV_ALIGN_TOP_LEFT, 14, 74);
    }
    lv_obj_t *l3 = label(c, "Then open in a browser:", &lv_font_montserrat_14, C_MUTED);
    if (l3 != NULL) {
        lv_obj_align(l3, LV_ALIGN_TOP_LEFT, 14, 104);
    }
    lv_obj_t *l4 = label(c, "http://192.168.4.1/", &lv_font_montserrat_14, C_ACCENT);
    if (l4 != NULL) {
        lv_obj_align(l4, LV_ALIGN_TOP_LEFT, 14, 128);
    }
    // 配网页是模态:UP/DOWN 与 OK 短按在这里刻意不做事。退出方式必须写在屏幕上,
    // 否则用户按上/下没有反应会以为设备卡住了(其他每个页面它们都有作用)。
    // 主推长按 DOWN —— 与其它页面的"长按返回"一致;长按 OK 也保留(它是进/出配网
    // 的同一个开关),所以文案里带上它。
    lv_obj_t *hint = label(s_prov_scr, "hold DOWN or OK to exit",
                           &lv_font_montserrat_14, C_MUTED);
    if (hint != NULL) {
        lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -8);
    }
}

// ---------------------------------------------------------------------------
// 页面构建
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Help:三行长按键说明,用与其它页面相同的"Key 左 + 值右"布局。
//
// 与别的列表页的区别只有一点:这里没有光标(没有可执行的动作),所以不做高亮,
// 三行都只是静态说明;进出的方式与子页一致(长按 DOWN 返回)。
//
// 只写"光看屏幕猜不出来"的三条。早先这里按页面分组写了三十多行,把
// "tap UP/DOWN = move cursor"、"tap OK = 选中"也说了一遍 —— 那些有高亮跟着动,
// 看屏幕就能学会,写出来只是噪音。
//
// 版式就是标准的"Key 左 + 值右",左右内缩各 10px,所以 Key 从 x=22 起、值右边缘
// 在 x=218。最宽的 Key 是 "Hold DOWN"(88px),值就在 x=120..218 之间,可用 98px。
// 三个取值分别是 80 / 84 / 90px(按真实字体量的),都能整段显示,不会被截断。
// ---------------------------------------------------------------------------

#define HELP_ROW_Y0 38

static lv_obj_t *s_help_scr;

static const char *const HELP_KEYS[]   = {"Hold UP", "Hold DOWN", "Hold OK"};
static const char *const HELP_VALUES[] = {"Start / stop", "Back a level", "Set up Wi-Fi"};

static void build_help(void)
{
    s_help_scr = base_screen("HELP");
    if (s_help_scr == NULL) {
        return;
    }

    for (int i = 0; i < 3; i++) {
        const int y = HELP_ROW_Y0 + i * ROW_PITCH;
        (void)row_bg(s_help_scr, y);
        const int y_c = y + ROW_H / 2;
        lv_obj_t *k = label(s_help_scr, HELP_KEYS[i], &lv_font_montserrat_14, C_MUTED);
        if (k != NULL) {
            lv_obj_align(k, LV_ALIGN_TOP_LEFT, ROW_X + 10,
                         y_c - lv_font_get_line_height(&lv_font_montserrat_14) / 2);
        }
        lv_obj_t *v = label(s_help_scr, HELP_VALUES[i], &lv_font_montserrat_14, C_TEXT);
        if (v != NULL) {
            lv_obj_align(v, LV_ALIGN_TOP_RIGHT,
                         -(lv_obj_get_width(s_help_scr) - (ROW_X + ROW_W - 10)),
                         y_c - lv_font_get_line_height(&lv_font_montserrat_14) / 2);
        }
    }
}

// ---------------------------------------------------------------------------
// DISPLAY:亮度与空闲调暗。
//
// 这一页和别的子页有一个结构上的差别:Brightness 行有**两种模式**。光标停在行上
// 时 UP/DOWN 仍然是"移动光标"(与全工程一致),按 OK 才进入"调节模式";进入之后
// UP/DOWN 变成增减亮度,再按 OK 或长按 DOWN 退出。之所以不直接把 UP/DOWN 绑成
// 增减,是因为那样一进页面就会调亮度 —— 用户想下楼看调暗档位,手一滑就把亮度改了。
//
// 提示文字是必须的:这个模式切换是屏幕本身教不出来的第三件事(前两件是长按 UP /
// 长按 OK,在 HELP 里),而 DISPLAY 页只有两行数据、下方本来就有空位。
// ---------------------------------------------------------------------------

#define DISPLAY_ROW_BRIGHT 0
#define DISPLAY_ROW_DIM    1

// 调节模式。跨按键回调与 LVGL 任务读写,标 volatile 以求稳妥。
static volatile bool s_bright_adjust;

// 背光"应该"处于的亮度。这是**唯一**的运行时亮度来源:界面显示它,空闲调暗恢复
// 背光时也用它。按键线程(note_input)与 LVGL 任务都要读它,所以做成单个 volatile
// 值 —— 若改成"调节期间读 A、平时读 B"的两处判断,按键线程就可能读到撕裂的组合,
// 恢复出一个谁也没设过的亮度。
//
// 它和 NVS 的关系:按键每调一格只改这个 RAM 值(直接作用于背光),**离开调节模式
// 时**才落一次盘。一次亮度调整最多走 18 步,每步一次 flash 擦写既拖慢按键手感、
// 又白白消耗 NVS 擦写寿命(仓库里"离线缓存默认关闭"就是同一考虑)。
static volatile uint8_t s_bright_live;

// 把攒下的亮度落盘。只在与已保存值不同时才写,避免无谓的 flash 擦写。
static void bright_commit(void)
{
    if (s_bright_live != sb_config_get()->brightness) {
        (void)sb_config_set_brightness(s_bright_live);
    }
}

// 行槽数只有 2,提示从第三行位置开始画。
#define DISPLAY_HINT_Y0 (ROW_Y0 + 2 * ROW_PITCH + 6)

// 三行提示都控制在 22 字符以内。取值区左起 x=22、右到 x=218(与 HELP 页同一版式),
// 可用 196px;按 HELP 页实测的小写文本宽度("Start / stop" 12 字符 80px ≈
// 6.7px/字符)折算,22 字符 ≈ 148px,余量充足。超宽会被屏幕右边缘裁掉,所以宁可写短句。
static void build_display_hint(lv_obj_t *scr)
{
    static const char *lines[] = {
        "OK: adjust brightness",
        "UP / DOWN: change it",
        "Hold DOWN: cancel",
    };
    for (int i = 0; i < 3; i++) {
        lv_obj_t *l = label(scr, lines[i], &lv_font_montserrat_14, C_MUTED);
        if (l != NULL) {
            lv_obj_set_pos(l, ROW_X + 10, DISPLAY_HINT_Y0 + i * 18);
        }
    }
}

// 空闲调暗的时间文案:0 档写 off,其余写成秒/分钟。
static void dim_text(char *buf, size_t cap, uint16_t seconds)
{
    if (seconds == 0) {
        strlcpy(buf, "off", cap);
    } else if (seconds < 60) {
        snprintf(buf, cap, "%u s", (unsigned)seconds);
    } else {
        snprintf(buf, cap, "%u min", (unsigned)(seconds / 60));
    }
}

static void refresh_disp(void)
{
    char buf[24];

    // 调节模式下把亮度值画成强调色,让"现在按上下是调它"在屏幕上有落点。
    snprintf(buf, sizeof buf, "%u%%", (unsigned)s_bright_live);
    page_set(&s_disp, DISPLAY_ROW_BRIGHT, buf,
             s_bright_adjust ? C_ACCENT : C_TEXT);

    dim_text(buf, sizeof buf, sb_config_get()->dim_timeout_s);
    page_set(&s_disp, DISPLAY_ROW_DIM, buf, C_TEXT);
}

static void build_pages(void)
{
    // 顺序:Recorder / Network / Storage / Device / Display / Help —— 最常看的排最前。
    static const char *menu_names[] = {"Recorder", "Network", "Storage", "Device",
                                       "Display", "Help"};
    page_init(&s_menu, SB_NAV_MENU, "SETTINGS", menu_names, 6);

    // 页面行数必须与 sb_nav(层级与光标边界的唯一事实来源)一致,否则光标会
    // 停到不存在的行、或漏掉最后一行。放在这里用编译期断言钉住。
    _Static_assert(sizeof(menu_names) / sizeof(menu_names[0]) == SB_MENU_ROWS,
                   "menu row count must match sb_nav");

    // NETWORK:Wi-Fi 与 Uplink 分列 —— "WiFi 通"不等于"能上传"。
    static const char *net_names[] = {"Wi-Fi", "SSID", "Device IP", "Server IP", "Uplink"};
    page_init(&s_net, SB_NAV_NET, "NETWORK", net_names, 5);
    _Static_assert(sizeof(net_names) / sizeof(net_names[0]) ==
                       SB_NAV_NET_ROWS, "net row count must match sb_nav");

    // RECORDER:只讲录音本身,不重复 WiFi 状态。
    static const char *rec_names[] = {"Recording", "Segments", "Sent", "Dropped"};
    page_init(&s_rec, SB_NAV_REC, "RECORDER", rec_names, 4);
    _Static_assert(sizeof(rec_names) / sizeof(rec_names[0]) ==
                       SB_NAV_REC_ROWS, "rec row count must match sb_nav");

    static const char *dev_names[] = {"Device ID", "Firmware", "Slot", "Commit",
                                      "Branch", "Built", "Check for update"};
    page_init(&s_dev, SB_NAV_DEV, "DEVICE", dev_names, 7);
    _Static_assert(sizeof(dev_names) / sizeof(dev_names[0]) ==
                       SB_NAV_DEV_ROWS, "dev row count must match sb_nav");

    static const char *stor_names[] = {"Cache", "Size", "Used", "Pending", "Dropped"};
    page_init(&s_stor, SB_NAV_STORAGE, "STORAGE", stor_names, 5);
    _Static_assert(sizeof(stor_names) / sizeof(stor_names[0]) ==
                       SB_NAV_STORAGE_ROWS, "storage row count must match sb_nav");

    // DISPLAY:只有两行设置。列表页本身没有"提示区",而"OK 选中亮度后 UP/DOWN 变成
    // 增减"这件事光看屏幕猜不出来,所以在行下方补两行静态提示(见 build_display_hint)。
    static const char *disp_names[] = {"Brightness", "Dim after"};
    page_init(&s_disp, SB_NAV_DISPLAY, "DISPLAY", disp_names, 2);
    _Static_assert(sizeof(disp_names) / sizeof(disp_names[0]) ==
                       SB_NAV_DISPLAY_ROWS, "display row count must match sb_nav");
    build_display_hint(s_disp.scr);
}

// ---------------------------------------------------------------------------
// 刷新:每个列表页只填自己的值
// ---------------------------------------------------------------------------

static void refresh_menu(void)
{
    const sb_config_t *cfg = sb_config_get();
    // 菜单右侧摘要:不进子页也能看出关键状态。
    page_set(&s_menu, SB_MENU_RECORDER, sb_recorder_paused() ? "paused" : "active",
             sb_recorder_paused() ? C_WARN : C_OK);
    if (!cfg->provisioned) {
        page_set(&s_menu, SB_MENU_NETWORK, "not set up", C_WARN);
    } else if (sb_net_state() == SB_NET_UP) {
        page_set(&s_menu, SB_MENU_NETWORK, cfg->ssid, C_OK);
    } else if (sb_net_state() == SB_NET_CONNECTING) {
        page_set(&s_menu, SB_MENU_NETWORK, "connecting", C_WARN);
    } else if (sb_net_reconnecting()) {
        page_set(&s_menu, SB_MENU_NETWORK, "reconnecting", C_WARN);
    } else {
        page_set(&s_menu, SB_MENU_NETWORK, "down", C_BAD);
    }
    if (cfg->cache_enabled) {
        page_set_fmt(&s_menu, SB_MENU_STORAGE, C_TEXT, "on %u MB",
                     (unsigned)cfg->cache_limit_mb);
    } else {
        page_set(&s_menu, SB_MENU_STORAGE, "off", C_MUTED);
    }
    // 菜单上的摘要只给版本号:那里空间有限,提交号在 DEVICE 页里再看。
    page_set(&s_menu, SB_MENU_DEVICE, sb_version_number(), C_TEXT);
    // DISPLAY 摘要给当前亮度:调暗过一次之后再进菜单也能一眼看出设的是多少。
    page_set_fmt(&s_menu, SB_MENU_DISPLAY, C_TEXT, "%u%%",
                 (unsigned)cfg->brightness);
    // Help 行不给右侧摘要:"Help" 本身就是完整的说明,再加一句摘要只是噪音。
    page_set(&s_menu, SB_MENU_HELP, "", C_TEXT);
}

static void refresh_net(void)
{
    const sb_config_t *cfg = sb_config_get();
    if (!cfg->provisioned) {
        page_set(&s_net, 0, "not set up", C_WARN);
    } else {
        switch (sb_net_state()) {
        case SB_NET_UP:        page_set(&s_net, 0, "connected", C_OK); break;
        case SB_NET_CONNECTING: page_set(&s_net, 0, "connecting", C_WARN); break;
        default:
            page_set(&s_net, 0, sb_net_reconnecting() ? "reconnecting" : "down",
                     sb_net_reconnecting() ? C_WARN : C_BAD);
            break;
        }
    }
    page_set(&s_net, 1, cfg->ssid[0] ? cfg->ssid : "-", C_TEXT);
    page_set(&s_net, 2, sb_net_state() == SB_NET_UP ? sb_net_ip() : "-", C_TEXT);
    page_set(&s_net, 3, cfg->srv_host, C_TEXT);   // 只给地址,不带端口
    switch (sb_uplink_state()) {
    case SB_UL_UP:         page_set(&s_net, 4, "connected", C_OK); break;
    case SB_UL_CONNECTING: page_set(&s_net, 4, "linking", C_WARN); break;
    case SB_UL_ERROR:      page_set(&s_net, 4, "error", C_BAD); break;
    default:               page_set(&s_net, 4, "idle", C_MUTED); break;
    }
}

static void refresh_rec(void)
{
    page_set(&s_rec, 0, sb_recorder_paused() ? "paused" : "active",
             sb_recorder_paused() ? C_WARN : C_OK);
    page_set_fmt(&s_rec, 1, C_TEXT, "%lu", (unsigned long)sb_recorder_segments());
    page_set_fmt(&s_rec, 2, C_TEXT, "%lu", (unsigned long)sb_uplink_frames_sent());
    page_set_fmt(&s_rec, 3,
                 sb_uplink_frames_dropped() ? C_WARN : C_TEXT,
                 "%lu", (unsigned long)sb_uplink_frames_dropped());
}

// DEVICE 页各行的下标。与 sb_nav 里的 DEV_ROW_* 一一对应,写在这边是为了让
// refresh/branch 滚动都能用名字而不是魔法数字。
#define DEV_ROW_DEV_ID  0
#define DEV_ROW_FW      1
#define DEV_ROW_SLOT    2
#define DEV_ROW_COMMIT  3
#define DEV_ROW_BRANCH  4
#define DEV_ROW_BUILT   5
#define DEV_ROW_OTA     6

// 行槽的取值宽度。值标签右边缘固定在 x=218,所以"左侧 Key 之后还剩多少像素"是个
// 常量,但每个 Key 不一样宽,分别算:
//   - Branch 未选中:取值不能盖到 "Branch" 这个名字上。它到 x=75,留 10px 空隙 →
//     值从 x=85 起,可用 218-85 = 133px。实测 "feature/always-..." 是 118px,正好。
#define DEV_VALUE_SAFE_PX 132
//   - Check for update 行的取值:该 Key 最长(到 x=149,留 6px 空隙) → 218-155 = 63px。
#define DEV_VALUE_ROW_PX  63

static int text_px(const char *s, void *ctx)
{
    (void)ctx;
    lv_point_t size;
    lv_text_get_size(&size, s, &lv_font_montserrat_14, 0, 0, LV_COORD_MAX,
                     LV_TEXT_FLAG_NONE);
    return (int)size.x;
}

// slot:运行槽直接告诉用户"我在 A 还是 B",比内部名 ota_0/ota_1 有用 —— 用户
// 关心的是"当前跑的是哪个槽",而 A/B 与 OTA 的槽切换是同一件事的两种说法。
static const char *slot_letter(void)
{
    const char *slot = sb_version_slot();
    if (slot[0] == 'o' && slot[4] == '1') {
        return "B";   // ota_1
    }
    if (slot[0] == 'o' && slot[4] == '0') {
        return "A";   // ota_0
    }
    return slot;      // factory / ? 之类原样显示
}

// Branch 的跑马灯偏移。只在光标停在 Branch 行时前进;离开时归零 —— 所以未选中
// Branch 永远只显示安全长度前缀,不会自己动起来。
static int s_branch_off;

static void refresh_dev(void)
{
    char buf[64];
    page_set(&s_dev, DEV_ROW_DEV_ID, sb_config_get()->dev_id, C_TEXT);
    // 两行分工明确:Firmware = 纯版本号(人比较"新旧"用),Commit = 8 位提交号
    // (机器/精确定位用)。合起来才是 OTA 身份串 sb_version_short()。
    page_set(&s_dev, DEV_ROW_FW, sb_version_number(), C_TEXT);
    page_set(&s_dev, DEV_ROW_SLOT, slot_letter(), C_TEXT);
    page_set(&s_dev, DEV_ROW_COMMIT, sb_version_commit(), C_TEXT);

    if (s_dev.win.cursor == DEV_ROW_BRANCH) {
        // 选中:Branch 太长时循环滚动,不截断,让用户看全。
        if (text_px(SB_GIT_BRANCH, NULL) <= DEV_VALUE_SAFE_PX) {
            s_branch_off = 0;
            page_set(&s_dev, DEV_ROW_BRANCH, SB_GIT_BRANCH, C_TEXT);
        } else {
            sb_text_marquee(buf, sizeof buf, SB_GIT_BRANCH, s_branch_off,
                            DEV_VALUE_SAFE_PX, text_px, NULL);
            s_branch_off++;
            page_set(&s_dev, DEV_ROW_BRANCH, buf, C_ACCENT);
        }
    } else {
        // 未选中:只显示安全长度,后接 ... 表示"还有内容"。安全长度保证不会盖到
        // 左边的 "Branch" 名字(见 DEV_VALUE_SAFE_PX)。
        s_branch_off = 0;
        sb_text_fit_ellipsis(buf, sizeof buf, SB_GIT_BRANCH, DEV_VALUE_SAFE_PX,
                             text_px, NULL);
        page_set(&s_dev, DEV_ROW_BRANCH, buf, C_TEXT);
    }

    page_set(&s_dev, DEV_ROW_BUILT, SB_BUILD_TIME, C_MUTED);

    // 可操作行用强调色(=超链接色),不写 "press OK":整行名字已经是蓝色,再写
    // 一次反而啰嗦。空闲时右边留空,只显示 OTA 的进度/结果。
    //
    // 失败原因可能很长("checksum mismatch after download" 有 267px),按行的取值
    // 宽度截断并加 ...,否则会盖到左边的 "Check for update" 上。
    const sb_ota_state_t st = sb_ota_state();
    switch (st) {
    case SB_OTA_CHECKING:
        page_set(&s_dev, DEV_ROW_OTA, "checking...", C_ACCENT);
        break;
    case SB_OTA_DOWNLOADING:
        sb_text_fit_ellipsis(buf, sizeof buf, sb_ota_message(), DEV_VALUE_ROW_PX,
                             text_px, NULL);
        page_set(&s_dev, DEV_ROW_OTA, buf, C_ACCENT);
        break;
    case SB_OTA_UP_TO_DATE:
        page_set(&s_dev, DEV_ROW_OTA, "up to date", C_OK);
        break;
    case SB_OTA_FAILED:
        sb_text_fit_ellipsis(buf, sizeof buf, sb_ota_message(), DEV_VALUE_ROW_PX,
                             text_px, NULL);
        page_set(&s_dev, DEV_ROW_OTA, buf, C_BAD);
        break;
    default:
        page_set(&s_dev, DEV_ROW_OTA, "", C_TEXT);
        break;
    }
}

static void refresh_stor(void)
{
    const sb_config_t *cfg = sb_config_get();
    page_set(&s_stor, 0, cfg->cache_enabled ? "on" : "off",
             cfg->cache_enabled ? C_OK : C_MUTED);
    page_set_fmt(&s_stor, 1, cfg->cache_enabled ? C_ACCENT : C_MUTED,
                 "%u MB", (unsigned)cfg->cache_limit_mb);
    if (cfg->cache_enabled) {
        page_set_fmt(&s_stor, 2, C_TEXT, "%u KB",
                     (unsigned)(sb_cache_used_bytes() / 1024u));
        page_set_fmt(&s_stor, 3, C_TEXT, "%u", (unsigned)sb_cache_pending());
        page_set_fmt(&s_stor, 4,
                     sb_cache_dropped() ? C_WARN : C_TEXT,
                     "%u", (unsigned)sb_cache_dropped());
    } else {
        page_set(&s_stor, 2, "-", C_MUTED);
        page_set(&s_stor, 3, "-", C_MUTED);
        page_set(&s_stor, 4, "-", C_MUTED);
    }
}

// 页面 -> 填值函数。滚动后立刻补一次新滑进窗口的行,所以这张表也顺带钉住了
// "每个列表页都有自己的 refresh",加页时忘了补会立刻表现为空白行。
static page_refresh_fn refresh_for(sb_screen_t screen)
{
    switch (screen) {
    case SB_SCREEN_MENU:    return refresh_menu;
    case SB_SCREEN_NET:     return refresh_net;
    case SB_SCREEN_REC:     return refresh_rec;
    case SB_SCREEN_DEV:     return refresh_dev;
    case SB_SCREEN_STORAGE: return refresh_stor;
    case SB_SCREEN_DISPLAY: return refresh_disp;
    default:                return NULL;
    }
}

// ---------------------------------------------------------------------------
// 定时刷新
// ---------------------------------------------------------------------------

static sb_screen_t s_screen = SB_SCREEN_HOME;

// ---------------------------------------------------------------------------
// 空闲调暗
//
// 屏幕是本设备上最大的耗电项(常亮 100% + Wi-Fi 常连 + 连续录音)。原来只有启动时
// 一次 bsp_display_backlight(100),之后再没有任何地方动过背光 —— 桌面放着也在满
// 亮烧电,插着充电器时尤其明显(负载把充电电流吃掉了,电量还可能倒着走)。
//
// 这里不动页面内容,只动背光:空闲够久就压到 SB_DIM_LEVEL,任何一次按键立刻恢复到
// 用户设置的亮度。调暗量取自配置,所以用户可以在 DISPLAY 页关掉(0 = 不调暗)或
// 改成更长的超时。
// ---------------------------------------------------------------------------

// 按键线程(note_input)与 LVGL 任务(dim_tick)都会读写这两个值,标 volatile 以防
// 被缓存 —— 否则按键重置的计时可能对定时器不可见,屏幕会在按键后仍然变暗。
static volatile uint32_t s_last_input_ms;
static volatile bool s_dimmed;

// 任何按键都算"用户在动",包括长按与配网页里的按键。
static void note_input(void)
{
    s_last_input_ms = lv_tick_get();
    if (s_dimmed) {
        // 立刻恢复到用户设置的亮度。恢复放在这里(按键线程)而不是定时器里,是为了
        // 让第一次按键就有反应 —— 定时器最多要等 500ms,按下去半秒才亮会显得迟钝。
        s_dimmed = false;
        bsp_display_backlight(s_bright_live);
    }
}

// 对外版本:main 的按键处理在每个事件上调用一次。它只读 lv_tick_get() 并可能改
// LEDC 背光,不碰任何 lv_* 对象,所以**不需要** LVGL 锁 —— 按键线程可以安全地直接
// 调用,从而让"按一下就亮"没有延迟。
void sb_ui_note_input(void)
{
    note_input();
}

static void dim_tick(uint32_t now_ms)
{
    const sb_config_t *cfg = sb_config_get();
    if (s_dimmed || !sb_dim_elapsed(now_ms, s_last_input_ms, cfg->dim_timeout_s)) {
        return;
    }
    s_dimmed = true;
    bsp_display_backlight(SB_DIM_LEVEL);
}

static void refresh(lv_timer_t *t)
{
    (void)t;
    // 运行时采样一次(开机 5 秒后):此时 LVGL 任务已经渲染过首帧,max_used 里已经
    // 包含了绘制任务/layer 的峰值。这正是崩溃现场那一类时刻的余量,所以"建页后"
    // 和"运行中"两组数字要一起看。
    //
    // 用 lv_tick_get() 判断而不是数调用次数:refresh() 除了 500ms 定时器,还会被
    // 滚动/激活/调亮度等路径直接调用,数次数会提前触发,量到的就不是"渲染之后"了。
    static bool s_runtime_logged;
    if (!s_runtime_logged && lv_tick_get() >= 5000) {
        s_runtime_logged = true;
        ui_mem_log("运行中");
    }
    dim_tick(lv_tick_get());
    uint32_t color = C_TEXT;
    const char *st = state_text(&color);
    if (s_home_state) {
        lv_label_set_text(s_home_state, st);
        lv_obj_set_style_text_color(s_home_state, lv_color_hex(color), 0);
    }
    battery_refresh();   // 所有页面右上角的电量
    if (s_home_sub) {
        lv_label_set_text(s_home_sub,
                          (sb_net_state() == SB_NET_UP) ? "uploading speech to cloud"
                                                        : "waiting for Wi-Fi");
    }
    if (s_home_seg) {
        lv_label_set_text_fmt(s_home_seg, "%lu", (unsigned long)sb_recorder_segments());
    }
    if (s_home_sent) {
        lv_label_set_text_fmt(s_home_sent, "%lu", (unsigned long)sb_uplink_frames_sent());
    }
    if (s_home_drop) {
        lv_label_set_text_fmt(s_home_drop, "%lu", (unsigned long)sb_uplink_frames_dropped());
    }
    // HOME 第一行的动词跟着录音状态走:暂停时写 "Start recording",在录时写
    // "Stop recording"。用户不需要知道"哪个是当前状态",只需要知道"按下去会怎样"。
    if (s_home_row_name[0] != NULL) {
        const bool paused = sb_recorder_paused();
        lv_label_set_text(s_home_row_name[0],
                          paused ? "Start recording" : "Stop recording");
        // 只有"停止"会打断会话,所以只有它用红色;开始录音用强调色即可。
        lv_obj_set_style_text_color(s_home_row_name[0],
                                    lv_color_hex(paused ? C_ACCENT : C_DANGER), 0);
    }
    refresh_menu();
    refresh_net();
    refresh_rec();
    refresh_dev();
    refresh_stor();
    refresh_disp();

    // Branch 的跑马灯只在该行被选中时前进。refresh_dev() 已经按当前光标决定画什么
    // (选中=滚动,未选中=安全前缀),所以这里不需要额外判断。
}

// ---------------------------------------------------------------------------
// 对外接口
// ---------------------------------------------------------------------------

void sb_ui_init(void)
{
    // 建页之前先记一次基线:池的总量/初始余量。之后每个阶段各记一次,这样
    // "池够不够、是哪一页吃掉的"就能从日志直接读出来,不必再靠反汇编猜。
    ui_mem_log("启动");
    build_home();
    build_pages();
    build_help();
    build_provision();
    // 全部页面建完后的余量 —— 这是判断 CONFIG_LV_MEM_SIZE 是否够用的关键数字。
    // 注意此时还没渲染过:绘制任务(layer/draw task)也从同一个池里出,所以余量
    // 必须明显大于"最大连续块"能承受的一次渲染峰值。
    ui_mem_log("建页后");
    // 背光按用户保存的亮度启动,而不是每次开机都 100%。空闲计时从"刚初始化"起算,
    // 于是开机后不会立刻把屏幕调暗。
    s_bright_live = sb_config_get()->brightness;
    s_last_input_ms = lv_tick_get();
    bsp_display_backlight(s_bright_live);
    lv_timer_create(refresh, 500, NULL);
    refresh(NULL);   // 立即填一次,避免开屏是占位符
    // HOME 也可能因池耗尽而没建出来:此时没有任何可显示的屏幕,保留状态并明确
    // 记一条错误,而不是把 NULL 交给 lv_screen_load。
    if (s_home_scr != NULL) {
        lv_screen_load(s_home_scr);
        s_screen = SB_SCREEN_HOME;
    } else {
        ESP_LOGE(TAG, "HOME 未建成:LVGL 池在启动阶段即耗尽,请调大 CONFIG_LV_MEM_SIZE");
    }
}

void sb_ui_show(sb_screen_t screen)
{
    // 离开 DISPLAY 页时退出亮度调节模式:否则从别的页面回来时 UP/DOWN 会突然变成
    // 增减亮度 —— 用户看不到头上那条"调节中"的强调色,只会觉得上下键坏了。
    if (s_screen == SB_SCREEN_DISPLAY && screen != SB_SCREEN_DISPLAY) {
        if (s_bright_adjust) {
            bright_commit();
        }
        s_bright_adjust = false;
    }
    // 目标页可能因为池耗尽而没建出来(scr == NULL)。lv_screen_load() 里
    // LV_CHECK_ARG(scr != NULL) 会直接 return,所以不会崩 —— 但**页面状态已经改了**,
    // 于是 current_page()/refresh 会去操作一个空页。这里改成:先解析出目标屏幕,
    // 只有真的存在才切换,否则退回 HOME 并保留原页面状态。
    lv_obj_t *target = NULL;
    sb_screen_t resolved = screen;
    switch (screen) {
    case SB_SCREEN_MENU:      target = s_menu.scr; break;
    case SB_SCREEN_NET:       target = s_net.scr; break;
    case SB_SCREEN_REC:       target = s_rec.scr; break;
    case SB_SCREEN_DEV:       target = s_dev.scr; break;
    case SB_SCREEN_STORAGE:   target = s_stor.scr; break;
    case SB_SCREEN_DISPLAY:   target = s_disp.scr; break;
    case SB_SCREEN_HELP:      target = s_help_scr; break;
    case SB_SCREEN_PROVISION: target = s_prov_scr; break;
    default:                  target = s_home_scr; resolved = SB_SCREEN_HOME; break;
    }
    if (target == NULL) {
        // 页面没建出来:退回 HOME。HOME 也空(池在启动时就耗尽)时保留状态,
        // 由 sb_ui_init 的日志说明原因。
        if (resolved != SB_SCREEN_HOME && s_home_scr != NULL) {
            ESP_LOGW(TAG, "页面 %d 未建成(池耗尽),退回 HOME", (int)resolved);
            lv_screen_load(s_home_scr);
            s_screen = SB_SCREEN_HOME;
        }
        return;
    }
    lv_screen_load(target);
    s_screen = resolved;
}

sb_screen_t sb_ui_current(void)
{
    return s_screen;
}

static list_page_t *current_page(void)
{
    switch (s_screen) {
    case SB_SCREEN_MENU:    return &s_menu;
    case SB_SCREEN_NET:     return &s_net;
    case SB_SCREEN_REC:     return &s_rec;
    case SB_SCREEN_DEV:     return &s_dev;
    case SB_SCREEN_STORAGE: return &s_stor;
    case SB_SCREEN_DISPLAY: return &s_disp;
    default:                return NULL;
    }
}

// 屏幕枚举 <-> 导航页枚举。两者的取值顺序一致(见各自头文件),但显式映射更
// 耐改,不依赖"枚举顺序恰好相同"这种隐形约定。
static sb_nav_page_t to_nav(sb_screen_t s)
{
    switch (s) {
    case SB_SCREEN_MENU:    return SB_NAV_MENU;
    case SB_SCREEN_NET:     return SB_NAV_NET;
    case SB_SCREEN_REC:     return SB_NAV_REC;
    case SB_SCREEN_DEV:     return SB_NAV_DEV;
    case SB_SCREEN_STORAGE: return SB_NAV_STORAGE;
    case SB_SCREEN_DISPLAY: return SB_NAV_DISPLAY;
    case SB_SCREEN_HELP:    return SB_NAV_HELP;
    case SB_SCREEN_PROVISION: return SB_NAV_PROVISION;
    default:                return SB_NAV_HOME;
    }
}

static sb_screen_t from_nav(sb_nav_page_t p)
{
    switch (p) {
    case SB_NAV_MENU:    return SB_SCREEN_MENU;
    case SB_NAV_NET:     return SB_SCREEN_NET;
    case SB_NAV_REC:     return SB_SCREEN_REC;
    case SB_NAV_DEV:     return SB_SCREEN_DEV;
    case SB_NAV_STORAGE: return SB_SCREEN_STORAGE;
    case SB_NAV_DISPLAY: return SB_SCREEN_DISPLAY;
    case SB_NAV_HELP:    return SB_SCREEN_HELP;
    case SB_NAV_PROVISION: return SB_SCREEN_PROVISION;
    default:             return SB_SCREEN_HOME;
    }
}

void sb_ui_move(int delta)
{
    if (s_screen == SB_SCREEN_HOME) {
        home_move(delta);
        return;
    }
    // DISPLAY 的调节模式:UP/DOWN 不再是"移光标",而是"增减亮度"。这条分流必须
    // 放在 page_move() 之前 —— 否则用户以为在调亮度,实际是光标上下跑。
    if (s_screen == SB_SCREEN_DISPLAY && s_bright_adjust) {
        // 只改 RAM 里的实时值并立刻作用于背光,NVS 留到退出调节模式时再写一次
        // (见 bright_commit 的说明)。所以调亮度时按键是即时响应的,不夹带 flash 擦写。
        const int next = sb_bright_step(s_bright_live, delta);
        if (next != s_bright_live) {
            s_bright_live = (uint8_t)next;
            bsp_display_backlight((uint8_t)next);
        }
        refresh(NULL);   // 让数值立刻跟上,不等下一个 500ms
        return;
    }
    list_page_t *p = current_page();
    if (p != NULL) {
        page_move(p, delta, s_screen);
    }
}

// 退出亮度调节模式。把攒下的亮度落盘并刷新显示(取消与"再按 OK"是同一个收尾动作)。
static void cancel_adjust(void)
{
    if (s_bright_adjust) {
        bright_commit();   // 取消也是"结束调节":攒下的亮度照样生效并落盘
        s_bright_adjust = false;
        refresh(NULL);
    }
}

bool sb_ui_row_actionable(void)
{
    if (s_screen == SB_SCREEN_HOME) {
        return true;   // HOME 两行都能按 OK(停录音 / 进设置)
    }
    // 可操作行的判断只有一处事实来源(sb_nav),界面不重复定义规则。页面身份在建页
    // 时已存进 list_page_t,这里直接用,不再从屏幕枚举反推一次。
    const list_page_t *p = current_page();
    if (p == NULL) {
        return false;
    }
    return sb_nav_row_actionable(p->nav, p->win.cursor);
}

void sb_ui_activate(void)
{
    switch (s_screen) {
    case SB_SCREEN_MENU:
        // 菜单行 -> 子页的映射由 sb_nav 决定,这里只负责跳转。
        sb_ui_show(from_nav(sb_nav_menu_target(s_menu.win.cursor)));
        break;
    case SB_SCREEN_HOME:
        // HOME 第一行 = 开始/停止录音(会话级,与长按 UP 同一个动作);第二行进设置。
        // 录音开关需要 main 里的 sb_uplink_end_session(),界面不直接碰会话,所以
        // HOME 的 OK 由 main 处理(见 handle_input)。这里只处理"进设置"。
        if (s_home_cursor != 0) {
            sb_ui_show(SB_SCREEN_MENU);
        }
        break;
    case SB_SCREEN_DEV:
        if (sb_nav_row_actionable(SB_NAV_DEV, s_dev.win.cursor) && !sb_ota_busy()) {
            (void)sb_ota_check_start();
        }
        break;
    case SB_SCREEN_STORAGE: {
        const sb_config_t *cfg = sb_config_get();
        if (!sb_nav_row_actionable(SB_NAV_STORAGE, s_stor.win.cursor)) {
            break;   // 统计行不响应 OK
        }
        if (s_stor.win.cursor == 0) {
            (void)sb_config_set_cache(!cfg->cache_enabled, cfg->cache_limit_mb);
        } else {
            // OK 循环切换容量,避免与"上下键移动光标"冲突。
            uint8_t next = (uint8_t)(cfg->cache_limit_mb % SB_CACHE_MAX_LIMIT_MB) + 1;
            (void)sb_config_set_cache(cfg->cache_enabled, next);
        }
        break;
    }
    case SB_SCREEN_DISPLAY: {
        const sb_config_t *cfg = sb_config_get();
        if (s_disp.win.cursor == DISPLAY_ROW_BRIGHT) {
            // OK 切换"调节模式":进入后 UP/DOWN 变成增减亮度,再按 OK 退出。
            if (s_bright_adjust) {
                bright_commit();          // 退出:把攒下的亮度落盘一次
                s_bright_adjust = false;
            } else {
                s_bright_live = cfg->brightness;   // 进入:从已保存值起步
                s_bright_adjust = true;
            }
        } else {
            // Dim after:OK 循环档位(0/30s/1min/3min/5min)。
            const int idx = sb_dim_next_index(sb_dim_preset_index(cfg->dim_timeout_s));
            (void)sb_config_set_dim_timeout(SB_DIM_PRESETS[idx]);
        }
        break;
    }
    default:
        break;
    }
    refresh(NULL);
}

void sb_ui_back(void)
{
    // 亮度调节模式里,长按 DOWN 先退出调节,而不是直接走人 —— 否则用户按住超过
    // 500ms 就会被连人带页一起带走,刚调好的亮度还在、页面却没了。
    if (s_screen == SB_SCREEN_DISPLAY && s_bright_adjust) {
        cancel_adjust();
        return;
    }
    // 上一级同样由 sb_nav 决定:子页 -> 菜单 -> 主页;主页/配网页停在原地。
    sb_ui_show(from_nav(sb_nav_parent(to_nav(s_screen))));
}

void sb_ui_set_ap_name(const char *name)
{
    if (name == NULL) {
        return;
    }
    strlcpy(s_ap_name, name, sizeof(s_ap_name));
    if (s_prov_ap) {
        lv_label_set_text(s_prov_ap, s_ap_name);
    }
}
