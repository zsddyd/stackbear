// tests/test_sb_nav.c —— 页面层级与可操作行的主机测试。
//
// 层级关系是纯数据,却最容易写错:返回路径、光标边界、"这行按 OK 有没有反应"。
// 这里穷举验证,而不是靠刷机点按键。
#include <stdio.h>

#include "sb_nav.h"

static int g_failures;

#define CHECK(cond, ...)                                                   \
    do {                                                                   \
        if (!(cond)) {                                                     \
            printf("FAIL %s:%d: %s", __FILE__, __LINE__, #cond);;           \
            printf(" " __VA_ARGS__);                                       \
            printf("\n");                                                  \
            g_failures++;                                                  \
        }                                                                  \
    } while (0)

static void test_parent_chain(void)
{
    // 子页 -> 菜单 -> 主页,两级封顶。
    CHECK(sb_nav_parent(SB_NAV_NET) == SB_NAV_MENU, "");
    CHECK(sb_nav_parent(SB_NAV_REC) == SB_NAV_MENU, "");
    CHECK(sb_nav_parent(SB_NAV_DEV) == SB_NAV_MENU, "");
    CHECK(sb_nav_parent(SB_NAV_STORAGE) == SB_NAV_MENU, "");
    CHECK(sb_nav_parent(SB_NAV_DISPLAY) == SB_NAV_MENU, "");
    CHECK(sb_nav_parent(SB_NAV_HELP) == SB_NAV_MENU, "");
    CHECK(sb_nav_parent(SB_NAV_MENU) == SB_NAV_HOME, "");
    // OTA 挂在 DEVICE 下(Upgrade 行进入),返回时回到 DEVICE 而不是菜单 ——
    // 否则用户看完升级结果会被甩回菜单,还得重新进 DEVICE。
    CHECK(sb_nav_parent(SB_NAV_OTA) == SB_NAV_DEV, "");
    CHECK(sb_nav_parent(SB_NAV_DEV) == SB_NAV_MENU, "");
    // 主页与配网页无处可退(长按返回应停在原页,不能把用户甩到空屏)。
    CHECK(sb_nav_parent(SB_NAV_HOME) == SB_NAV_HOME, "");
    CHECK(sb_nav_parent(SB_NAV_PROVISION) == SB_NAV_PROVISION, "");

    // 从任意子页连按返回,必须能走到主页且不越界。
    for (sb_nav_page_t p = SB_NAV_MENU; p < SB_NAV_PAGE_COUNT; p++) {
        if (p == SB_NAV_PROVISION) {
            continue;   // 配网是模态页,长按 OK 退出而不是走父链
        }
        int steps = 0;
        sb_nav_page_t cur = p;
        while (cur != SB_NAV_HOME && steps < 8) {
            cur = sb_nav_parent(cur);
            steps++;
        }
        CHECK(cur == SB_NAV_HOME, "every page must reach HOME");;
    }
}

static void test_menu_targets(void)
{
    // 每行各自映射到它的子页,顺序由 sb_nav.h 的枚举决定(SETTINGS 页自上而下
    // 是 Recorder / Network / Storage / Device / Display / Help)。
    CHECK(sb_nav_menu_target(SB_MENU_RECORDER) == SB_NAV_REC, "");
    CHECK(sb_nav_menu_target(SB_MENU_NETWORK) == SB_NAV_NET, "");
    CHECK(sb_nav_menu_target(SB_MENU_STORAGE) == SB_NAV_STORAGE, "");
    CHECK(sb_nav_menu_target(SB_MENU_DEVICE) == SB_NAV_DEV, "");
    CHECK(sb_nav_menu_target(SB_MENU_DISPLAY) == SB_NAV_DISPLAY, "");
    CHECK(sb_nav_menu_target(SB_MENU_HELP) == SB_NAV_HELP, "");
    // 枚举顺序本身就是"页面上从上到下"的顺序,顺序错位会让 OK 进错页,所以把
    // 数值也钉住。
    CHECK(SB_MENU_RECORDER == 0 && SB_MENU_NETWORK == 1 && SB_MENU_STORAGE == 2 &&
          SB_MENU_DEVICE == 3 && SB_MENU_DISPLAY == 4 && SB_MENU_HELP == 5,
          "menu order is the screen order");
    // 越界不能崩,也不能跳到意外的页。
    CHECK(sb_nav_menu_target(-1) == SB_NAV_MENU, "");
    CHECK(sb_nav_menu_target(SB_MENU_ROWS) == SB_NAV_MENU, "");
}

static void test_rows_and_clamp(void)
{
    CHECK(sb_nav_rows(SB_NAV_MENU) == 6, "");
    CHECK(sb_nav_rows(SB_NAV_NET) == 5, "");
    CHECK(sb_nav_rows(SB_NAV_REC) == 4, "");
    CHECK(sb_nav_rows(SB_NAV_DEV) == 7, "");
    CHECK(sb_nav_rows(SB_NAV_STORAGE) == 5, "");
    CHECK(sb_nav_rows(SB_NAV_DISPLAY) == 2, "");
    CHECK(sb_nav_rows(SB_NAV_HELP) == 0, "");        // 整页滚动文字,没有光标
    CHECK(sb_nav_rows(SB_NAV_OTA) == 0, "");         // 纯状态页,没有可停的行
    CHECK(sb_nav_rows(SB_NAV_HOME) == 0, "");        // 非列表页
    CHECK(sb_nav_rows(SB_NAV_PROVISION) == 0, "");

    // 光标在两端都不能溢出(多按几次上/下要停住,而不是绕回或越界)。
    for (sb_nav_page_t p = SB_NAV_MENU; p <= SB_NAV_DISPLAY; p++) {
        const int rows = sb_nav_rows(p);
        CHECK(sb_nav_clamp(p, -5) == 0, "clamp low");;
        CHECK(sb_nav_clamp(p, rows + 9) == rows - 1, "clamp high");;
        CHECK(sb_nav_clamp(p, 0) == 0, "clamp keeps 0");;
    }
}

static void test_actionable_rows(void)
{
    // 菜单每行都能进子页。
    for (int r = 0; r < sb_nav_rows(SB_NAV_MENU); r++) {
        CHECK(sb_nav_row_actionable(SB_NAV_MENU, r), "menu rows enter a page");;
    }
    // NETWORK / RECORDER 是纯展示,按 OK 不该有任何副作用。
    for (int r = 0; r < sb_nav_rows(SB_NAV_NET); r++) {
        CHECK(!sb_nav_row_actionable(SB_NAV_NET, r), "network is read-only");;
    }
    for (int r = 0; r < sb_nav_rows(SB_NAV_REC); r++) {
        CHECK(!sb_nav_row_actionable(SB_NAV_REC, r), "recorder is read-only");;
    }
    // DEVICE 只有最后一行(Upgrade)可操作。
    CHECK(!sb_nav_row_actionable(SB_NAV_DEV, 0), "");
    CHECK(!sb_nav_row_actionable(SB_NAV_DEV, 5), "");
    CHECK(sb_nav_row_actionable(SB_NAV_DEV, 6), "");
    // STORAGE 只有 Cache 与 Size 可操作,统计行不可。
    CHECK(sb_nav_row_actionable(SB_NAV_STORAGE, 0), "");
    CHECK(sb_nav_row_actionable(SB_NAV_STORAGE, 1), "");
    CHECK(!sb_nav_row_actionable(SB_NAV_STORAGE, 2), "");
    CHECK(!sb_nav_row_actionable(SB_NAV_STORAGE, 3), "");
    CHECK(!sb_nav_row_actionable(SB_NAV_STORAGE, 4), "");
    // DISPLAY 两行都可操作(亮度行切换调节模式,调暗行循环档位)。
    CHECK(sb_nav_row_actionable(SB_NAV_DISPLAY, 0), "");
    CHECK(sb_nav_row_actionable(SB_NAV_DISPLAY, 1), "");
    CHECK(!sb_nav_row_actionable(SB_NAV_DISPLAY, 2), "out of range");
    // 越界一律不可操作。
    CHECK(!sb_nav_row_actionable(SB_NAV_DEV, 7), "");
    CHECK(!sb_nav_row_actionable(SB_NAV_STORAGE, -1), "");
    CHECK(!sb_nav_row_actionable(SB_NAV_HOME, 0), "");
    CHECK(!sb_nav_row_actionable(SB_NAV_HELP, 0), "");   // Help 没有行
}

// 界面用 sb_nav_row_actionable() 给可操作行的名称上强调色(用户否则看不出
// 哪些行按 OK 有反应)。这里钉住"确实存在可操作行"以及"菜单每行都可操作",
// 免得规则被改成全部不可操作后界面变成一片灰、也没人发现。
static void test_actionable_marker_contract(void)
{
    int actionable_pages = 0;
    for (sb_nav_page_t p = SB_NAV_MENU; p <= SB_NAV_DISPLAY; p++) {
        const int rows = sb_nav_rows(p);
        int marked = 0;
        for (int r = 0; r < rows; r++) {
            if (sb_nav_row_actionable(p, r)) marked++;
        }
        if (marked > 0) actionable_pages++;
        if (p == SB_NAV_MENU) {
            CHECK(marked == rows, "every menu row is actionable");
        }
    }
    // 五个子页里 DEVICE / STORAGE / DISPLAY 各有可操作行,菜单也有;
    // NETWORK/RECORDER 没有。
    CHECK(actionable_pages == 4, "exactly four pages carry actionable rows");
}

int main(void)
{
    test_parent_chain();
    test_menu_targets();
    test_rows_and_clamp();
    test_actionable_rows();
    test_actionable_marker_contract();
    if (g_failures == 0) {
        printf("test_sb_nav: PASS\n");
        return 0;
    }
    printf("test_sb_nav: %d failure(s)\n", g_failures);
    return 1;
}
