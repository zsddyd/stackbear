// main/sb_ui.c —— 应用界面。刻意区别于基线 demo 的"像素菜单":深色底 + 圆角卡片 +
// 单一主状态,电量固定右上角。
//
// 结构:HOME 独占聚合状态卡;STATUS 是分组菜单;四个子页各讲一层信息。
// 所有列表页共用同一个 list_page_t + 通用刷新,避免每页重复样板代码。
#include "sb_ui.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "bsp_battery.h"
#include "sb_cache.h"
#include "sb_config.h"
#include "sb_nav.h"
#include "sb_net.h"
#include "sb_ota.h"
#include "sb_recorder.h"
#include "sb_uplink.h"
#include "sb_version.h"

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

#define MAX_ROWS   7
#define ROW_X      12
#define ROW_Y0     38
#define ROW_PITCH  34
#define ROW_H      30
#define ROW_W      216
#define SEL_MS     150   // 选中高亮滑动时长;按键连击时只重启这一个动画

// ---------------------------------------------------------------------------
// 通用控件
// ---------------------------------------------------------------------------

static lv_obj_t *card(lv_obj_t *parent, int x, int y, int w, int h, uint32_t color)
{
    lv_obj_t *o = lv_obj_create(parent);
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
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    return l;
}

static lv_obj_t *base_screen(const char *title)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(scr, lv_color_hex(C_BG), 0);
    lv_obj_set_style_border_width(scr, 0, 0);
    lv_obj_set_style_pad_all(scr, 0, 0);
    lv_obj_t *t = label(scr, title, &lv_font_montserrat_14, C_MUTED);
    lv_obj_set_pos(t, 12, 10);
    return scr;
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

typedef struct {
    lv_obj_t *scr;
    lv_obj_t *sel;               // 选中高亮矩形(C_SEL)
    lv_obj_t *vals[MAX_ROWS];
    int count;
    int cursor;
} list_page_t;

static list_page_t s_menu, s_net, s_rec, s_dev, s_stor;

// 一行的底色矩形。只负责画那一行的卡片轮廓,不承载子对象。
static lv_obj_t *row_bg(lv_obj_t *parent, int y)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(o, ROW_X, y);
    lv_obj_set_size(o, ROW_W, ROW_H);
    lv_obj_set_style_radius(o, 10, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    lv_obj_set_style_bg_color(o, lv_color_hex(C_CARD), 0);
    return o;
}

static void page_init(list_page_t *p, const char *title, const char *const *names, int count)
{
    p->count = count;
    p->cursor = 0;
    p->scr = base_screen(title);

    // 第 1 层:所有行的卡片底色。
    for (int i = 0; i < count && i < MAX_ROWS; i++) {
        // 只需要对象存在,后续不再引用:标签按行索引算坐标,不依赖句柄。
        (void)row_bg(p->scr, ROW_Y0 + i * ROW_PITCH);
    }

    // 第 2 层:选中高亮。它在行底色之后创建,所以盖在选中行的底色上;几何与
    // 行底色完全一致(同样的 x/y/w/h 与圆角),视觉上等同于把那一行的底色换成
    // C_SEL,但它是一个可以整体缓动的独立对象。
    p->sel = lv_obj_create(p->scr);
    lv_obj_remove_flag(p->sel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(p->sel, ROW_X, ROW_Y0);
    lv_obj_set_size(p->sel, ROW_W, ROW_H);
    lv_obj_set_style_radius(p->sel, 10, 0);
    lv_obj_set_style_border_width(p->sel, 0, 0);
    lv_obj_set_style_pad_all(p->sel, 0, 0);
    lv_obj_set_style_bg_color(p->sel, lv_color_hex(C_SEL), 0);

    // 第 3 层:标签。挂在屏幕上(而非行底色里)才能绘制在高亮之上。
    for (int i = 0; i < count && i < MAX_ROWS; i++) {
        // 行垂直中心。字号固定为 14,用它的行高把文字居中。
        const int y_c = ROW_Y0 + i * ROW_PITCH + ROW_H / 2;
        lv_obj_t *n = label(p->scr, names[i], &lv_font_montserrat_14, C_MUTED);
        lv_obj_align(n, LV_ALIGN_TOP_LEFT, ROW_X + 10,
                     y_c - lv_font_get_line_height(&lv_font_montserrat_14) / 2);
        p->vals[i] = label(p->scr, "-", &lv_font_montserrat_14, C_TEXT);
        // 右边缘对齐到行右侧内缩 10px 处。偏移量从屏幕右边缘往前算,所以不写死
        // 屏幕宽度:屏幕是 240 宽时 240-(12+216-10)=22。
        lv_obj_align(p->vals[i], LV_ALIGN_TOP_RIGHT,
                     -(lv_obj_get_width(p->scr) - (ROW_X + ROW_W - 10)),
                     y_c - lv_font_get_line_height(&lv_font_montserrat_14) / 2);
    }
}

static void page_set(list_page_t *p, int row, const char *value, uint32_t color)
{
    if (p->vals[row] == NULL) {
        return;
    }
    lv_label_set_text(p->vals[row], value);
    lv_obj_set_style_text_color(p->vals[row], lv_color_hex(color), 0);
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
    const int32_t target = ROW_Y0 + p->cursor * ROW_PITCH;
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

static void page_move(list_page_t *p, int delta)
{
    const int before = p->cursor;
    p->cursor += delta;
    if (p->cursor < 0) {
        p->cursor = 0;
    }
    if (p->cursor >= p->count) {
        p->cursor = p->count - 1;
    }
    // 光标没动(已到边界)就不要白白重启动画。
    if (p->cursor != before) {
        page_slide_sel(p);
    }
}

// ---------------------------------------------------------------------------
// HOME:聚合状态 + 三个计数
// ---------------------------------------------------------------------------

static lv_obj_t *s_home_scr;
static lv_obj_t *s_home_state;
static lv_obj_t *s_home_sub;
static lv_obj_t *s_home_batt;
static lv_obj_t *s_home_seg;
static lv_obj_t *s_home_sent;
static lv_obj_t *s_home_drop;

static void build_home(void)
{
    s_home_scr = base_screen("STACKBEAR RECORDER");
    s_home_batt = label(s_home_scr, "--%", &lv_font_montserrat_14, C_TEXT);
    lv_obj_align(s_home_batt, LV_ALIGN_TOP_RIGHT, -12, 10);

    lv_obj_t *c = card(s_home_scr, 12, 44, 216, 132, C_CARD);
    s_home_state = label(c, "STARTING", &lv_font_montserrat_20, C_TEXT);
    lv_obj_align(s_home_state, LV_ALIGN_TOP_MID, 0, 42);
    s_home_sub = label(c, "", &lv_font_montserrat_14, C_MUTED);
    lv_label_set_long_mode(s_home_sub, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_home_sub, 196);
    lv_obj_set_style_text_align(s_home_sub, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_home_sub, LV_ALIGN_BOTTOM_MID, 0, -14);

    static const char *caps[3] = {"SEG", "SENT", "DROP"};
    lv_obj_t **vals[3] = {&s_home_seg, &s_home_sent, &s_home_drop};
    for (int i = 0; i < 3; i++) {
        lv_obj_t *k = card(s_home_scr, 12 + i * 74, 194, 68, 62, C_CARD);
        label(k, caps[i], &lv_font_montserrat_14, C_MUTED);
        lv_obj_align(lv_obj_get_child(k, 0), LV_ALIGN_TOP_MID, 0, 8);
        *vals[i] = label(k, "0", &lv_font_montserrat_14, C_TEXT);
        lv_obj_align(*vals[i], LV_ALIGN_BOTTOM_MID, 0, -10);
    }
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
    lv_obj_t *c = card(s_prov_scr, 12, 44, 216, 176, C_CARD);
    lv_obj_t *l1 = label(c, "Join this Wi-Fi hotspot:", &lv_font_montserrat_14, C_MUTED);
    lv_obj_align(l1, LV_ALIGN_TOP_LEFT, 14, 14);
    s_prov_ap = label(c, "Passport-XXXX", &lv_font_montserrat_20, C_OK);
    lv_obj_align(s_prov_ap, LV_ALIGN_TOP_LEFT, 14, 40);
    lv_obj_t *l2 = label(c, "Password: passport1234", &lv_font_montserrat_14, C_TEXT);
    lv_obj_align(l2, LV_ALIGN_TOP_LEFT, 14, 74);
    lv_obj_t *l3 = label(c, "Then open in a browser:", &lv_font_montserrat_14, C_MUTED);
    lv_obj_align(l3, LV_ALIGN_TOP_LEFT, 14, 104);
    lv_obj_t *l4 = label(c, "http://192.168.4.1/", &lv_font_montserrat_14, C_ACCENT);
    lv_obj_align(l4, LV_ALIGN_TOP_LEFT, 14, 128);
    lv_obj_t *hint = label(s_prov_scr, "hold OK: cancel", &lv_font_montserrat_14, C_MUTED);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -8);
}

// ---------------------------------------------------------------------------
// 页面构建
// ---------------------------------------------------------------------------

static void build_pages(void)
{
    static const char *menu_names[] = {"Network", "Recorder", "Device", "Storage"};
    page_init(&s_menu, "STATUS", menu_names, 4);

    // 页面行数必须与 sb_nav(层级与光标边界的唯一事实来源)一致,否则光标会
    // 停到不存在的行、或漏掉最后一行。放在这里用编译期断言钉住。
    _Static_assert(sizeof(menu_names) / sizeof(menu_names[0]) == SB_MENU_ROWS,
                   "menu row count must match sb_nav");

    // NETWORK:Wi-Fi 与 Uplink 分列 —— "WiFi 通"不等于"能上传"。
    static const char *net_names[] = {"Wi-Fi", "SSID", "Device IP", "Server IP", "Uplink"};
    page_init(&s_net, "NETWORK", net_names, 5);
    _Static_assert(sizeof(net_names) / sizeof(net_names[0]) ==
                       SB_NAV_NET_ROWS, "net row count must match sb_nav");

    // RECORDER:只讲录音本身,不重复 WiFi 状态。
    static const char *rec_names[] = {"Recording", "Segments", "Sent", "Dropped"};
    page_init(&s_rec, "RECORDER", rec_names, 4);
    _Static_assert(sizeof(rec_names) / sizeof(rec_names[0]) ==
                       SB_NAV_REC_ROWS, "rec row count must match sb_nav");

    static const char *dev_names[] = {"Device ID", "Firmware", "Slot", "Commit",
                                      "Branch", "Built", "Check for update"};
    page_init(&s_dev, "DEVICE", dev_names, 7);
    _Static_assert(sizeof(dev_names) / sizeof(dev_names[0]) ==
                       SB_NAV_DEV_ROWS, "dev row count must match sb_nav");

    static const char *stor_names[] = {"Cache", "Size", "Used", "Pending", "Dropped"};
    page_init(&s_stor, "STORAGE", stor_names, 5);
    _Static_assert(sizeof(stor_names) / sizeof(stor_names[0]) ==
                       SB_NAV_STORAGE_ROWS, "storage row count must match sb_nav");
}

// ---------------------------------------------------------------------------
// 刷新:每个列表页只填自己的值
// ---------------------------------------------------------------------------

static void refresh_menu(void)
{
    const sb_config_t *cfg = sb_config_get();
    // 菜单右侧摘要:不进子页也能看出关键状态。
    if (!cfg->provisioned) {
        page_set(&s_menu, 0, "not set up", C_WARN);
    } else if (sb_net_state() == SB_NET_UP) {
        page_set(&s_menu, 0, cfg->ssid, C_OK);
    } else if (sb_net_state() == SB_NET_CONNECTING) {
        page_set(&s_menu, 0, "connecting", C_WARN);
    } else if (sb_net_reconnecting()) {
        page_set(&s_menu, 0, "reconnecting", C_WARN);
    } else {
        page_set(&s_menu, 0, "down", C_BAD);
    }
    page_set(&s_menu, 1, sb_recorder_paused() ? "paused" : "active",
             sb_recorder_paused() ? C_WARN : C_OK);
    page_set(&s_menu, 2, sb_version_short(), C_TEXT);
    if (cfg->cache_enabled) {
        page_set_fmt(&s_menu, 3, C_TEXT, "on %u MB", (unsigned)cfg->cache_limit_mb);
    } else {
        page_set(&s_menu, 3, "off", C_MUTED);
    }
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

static void refresh_dev(void)
{
    page_set(&s_dev, 0, sb_config_get()->dev_id, C_TEXT);
    page_set(&s_dev, 1, sb_version_short(), C_TEXT);
    page_set(&s_dev, 2, sb_version_slot(), C_TEXT);
    page_set(&s_dev, 3, SB_GIT_COMMIT, C_TEXT);
    page_set(&s_dev, 4, SB_GIT_BRANCH, C_TEXT);
    page_set(&s_dev, 5, SB_BUILD_TIME, C_MUTED);
    // 可操作行用强调色,并显示 OTA 进度/结果。
    const sb_ota_state_t st = sb_ota_state();
    switch (st) {
    case SB_OTA_CHECKING:    page_set(&s_dev, 6, "checking...", C_ACCENT); break;
    case SB_OTA_DOWNLOADING: page_set(&s_dev, 6, sb_ota_message(), C_ACCENT); break;
    case SB_OTA_UP_TO_DATE:  page_set(&s_dev, 6, "up to date", C_OK); break;
    case SB_OTA_FAILED:      page_set(&s_dev, 6, sb_ota_message(), C_BAD); break;
    default:                 page_set(&s_dev, 6, "press OK", C_ACCENT); break;
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

// ---------------------------------------------------------------------------
// 定时刷新
// ---------------------------------------------------------------------------

static sb_screen_t s_screen = SB_SCREEN_HOME;

static void refresh(lv_timer_t *t)
{
    (void)t;
    uint32_t color = C_TEXT;
    const char *st = state_text(&color);
    if (s_home_state) {
        lv_label_set_text(s_home_state, st);
        lv_obj_set_style_text_color(s_home_state, lv_color_hex(color), 0);
    }
    const int soc = bsp_battery_soc();
    if (s_home_batt) {
        if (soc < 0) {
            lv_label_set_text(s_home_batt, "--%");
        } else {
            lv_label_set_text_fmt(s_home_batt, "%d%%", soc);
        }
    }
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
    refresh_menu();
    refresh_net();
    refresh_rec();
    refresh_dev();
    refresh_stor();
}

// ---------------------------------------------------------------------------
// 对外接口
// ---------------------------------------------------------------------------

void sb_ui_init(void)
{
    build_home();
    build_pages();
    build_provision();
    lv_timer_create(refresh, 500, NULL);
    refresh(NULL);   // 立即填一次,避免开屏是占位符
    lv_screen_load(s_home_scr);
    s_screen = SB_SCREEN_HOME;
}

void sb_ui_show(sb_screen_t screen)
{
    s_screen = screen;
    switch (screen) {
    case SB_SCREEN_MENU:    lv_screen_load(s_menu.scr); break;
    case SB_SCREEN_NET:     lv_screen_load(s_net.scr); break;
    case SB_SCREEN_REC:     lv_screen_load(s_rec.scr); break;
    case SB_SCREEN_DEV:     lv_screen_load(s_dev.scr); break;
    case SB_SCREEN_STORAGE: lv_screen_load(s_stor.scr); break;
    case SB_SCREEN_PROVISION: lv_screen_load(s_prov_scr); break;
    default:                lv_screen_load(s_home_scr); s_screen = SB_SCREEN_HOME; break;
    }
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
    case SB_NAV_PROVISION: return SB_SCREEN_PROVISION;
    default:             return SB_SCREEN_HOME;
    }
}

void sb_ui_move(int delta)
{
    list_page_t *p = current_page();
    if (p != NULL) {
        page_move(p, delta);
    }
}

bool sb_ui_row_actionable(void)
{
    // 可操作行的判断只有一处事实来源(sb_nav),界面不重复定义规则。
    const list_page_t *p = current_page();
    if (p == NULL) {
        return false;
    }
    return sb_nav_row_actionable(to_nav(s_screen), p->cursor);
}

void sb_ui_activate(void)
{
    switch (s_screen) {
    case SB_SCREEN_MENU:
        // 菜单行 -> 子页的映射由 sb_nav 决定,这里只负责跳转。
        sb_ui_show(from_nav(sb_nav_menu_target(s_menu.cursor)));
        break;
    case SB_SCREEN_DEV:
        if (sb_nav_row_actionable(SB_NAV_DEV, s_dev.cursor) && !sb_ota_busy()) {
            (void)sb_ota_check_start();
        }
        break;
    case SB_SCREEN_STORAGE: {
        const sb_config_t *cfg = sb_config_get();
        if (!sb_nav_row_actionable(SB_NAV_STORAGE, s_stor.cursor)) {
            break;   // 统计行不响应 OK
        }
        if (s_stor.cursor == 0) {
            (void)sb_config_set_cache(!cfg->cache_enabled, cfg->cache_limit_mb);
        } else {
            // OK 循环切换容量,避免与"上下键移动光标"冲突。
            uint8_t next = (uint8_t)(cfg->cache_limit_mb % SB_CACHE_MAX_LIMIT_MB) + 1;
            (void)sb_config_set_cache(cfg->cache_enabled, next);
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
