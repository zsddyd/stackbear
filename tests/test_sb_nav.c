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
    CHECK(sb_nav_parent(SB_NAV_MENU) == SB_NAV_HOME, "");
    // 主页与配网页无处可退(长按返回应停在原页,不能把用户甩到空屏)。
    CHECK(sb_nav_parent(SB_NAV_HOME) == SB_NAV_HOME, "");
    CHECK(sb_nav_parent(SB_NAV_PROVISION) == SB_NAV_PROVISION, "");

    // 从任意子页连按返回,必须能走到主页且不越界。
    for (sb_nav_page_t p = SB_NAV_MENU; p < SB_NAV_PROVISION; p++) {
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
    CHECK(sb_nav_menu_target(SB_MENU_NETWORK) == SB_NAV_NET, "");
    CHECK(sb_nav_menu_target(SB_MENU_RECORDER) == SB_NAV_REC, "");
    CHECK(sb_nav_menu_target(SB_MENU_DEVICE) == SB_NAV_DEV, "");
    CHECK(sb_nav_menu_target(SB_MENU_STORAGE) == SB_NAV_STORAGE, "");
    // 越界不能崩,也不能跳到意外的页。
    CHECK(sb_nav_menu_target(-1) == SB_NAV_MENU, "");
    CHECK(sb_nav_menu_target(SB_MENU_ROWS) == SB_NAV_MENU, "");
}

static void test_rows_and_clamp(void)
{
    CHECK(sb_nav_rows(SB_NAV_MENU) == 4, "");
    CHECK(sb_nav_rows(SB_NAV_NET) == 5, "");
    CHECK(sb_nav_rows(SB_NAV_REC) == 4, "");
    CHECK(sb_nav_rows(SB_NAV_DEV) == 7, "");
    CHECK(sb_nav_rows(SB_NAV_STORAGE) == 5, "");
    CHECK(sb_nav_rows(SB_NAV_HOME) == 0, "");        // 非列表页
    CHECK(sb_nav_rows(SB_NAV_PROVISION) == 0, "");

    // 光标在两端都不能溢出(多按几次上/下要停住,而不是绕回或越界)。
    for (sb_nav_page_t p = SB_NAV_MENU; p <= SB_NAV_STORAGE; p++) {
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
    // DEVICE 只有最后一行(Check for update)可操作。
    CHECK(!sb_nav_row_actionable(SB_NAV_DEV, 0), "");
    CHECK(!sb_nav_row_actionable(SB_NAV_DEV, 5), "");
    CHECK(sb_nav_row_actionable(SB_NAV_DEV, 6), "");
    // STORAGE 只有 Cache 与 Size 可操作,统计行不可。
    CHECK(sb_nav_row_actionable(SB_NAV_STORAGE, 0), "");
    CHECK(sb_nav_row_actionable(SB_NAV_STORAGE, 1), "");
    CHECK(!sb_nav_row_actionable(SB_NAV_STORAGE, 2), "");
    CHECK(!sb_nav_row_actionable(SB_NAV_STORAGE, 3), "");
    CHECK(!sb_nav_row_actionable(SB_NAV_STORAGE, 4), "");
    // 越界一律不可操作。
    CHECK(!sb_nav_row_actionable(SB_NAV_DEV, 7), "");
    CHECK(!sb_nav_row_actionable(SB_NAV_STORAGE, -1), "");
    CHECK(!sb_nav_row_actionable(SB_NAV_HOME, 0), "");
}

int main(void)
{
    test_parent_chain();
    test_menu_targets();
    test_rows_and_clamp();
    test_actionable_rows();
    if (g_failures == 0) {
        printf("test_sb_nav: PASS\n");
        return 0;
    }
    printf("test_sb_nav: %d failure(s)\n", g_failures);
    return 1;
}
