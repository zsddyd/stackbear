// main/sb_version.h —— 固件版本/构建信息。由 CMake 在编译时注入(见 CMakeLists.txt)。
#pragma once

// 构建时由 CMake 定义的字符串。若未定义(例如主机测试),给出安全的占位值。
// SB_GIT_COMMIT 已经是**恰好由 git 保证唯一的短号(8 位)**,这里不再做任何
// 手工截断 —— 手工截断会绕开 git 的唯一性保证,见 CMakeLists.txt 里的说明。
#ifndef SB_GIT_COMMIT
#define SB_GIT_COMMIT "unknown"
#endif
#ifndef SB_GIT_BRANCH
#define SB_GIT_BRANCH "unknown"
#endif
#ifndef SB_BUILD_TIME
#define SB_BUILD_TIME "unknown"
#endif
#ifndef SB_APP_VERSION
#define SB_APP_VERSION "0.0.0"
#endif

// 纯版本号,界面 "Firmware" 行显示的就是它,如 "0.7.8"。
const char *sb_version_number(void);

// 8 位提交短号(与 sb_version_number() 拼成 OTA 身份串),如 "df4e58d1"。
const char *sb_version_commit(void);

// OTA 身份串 = "v<版本号>+<8 位提交短号>",如 "v0.7.8+df4e58d1"。
// 这是设备与 OTA manifest **唯一**用来判断"是否已是最新"的字符串,所以它必须
// 与 tools/make_ota_manifest.py 生成的完全一致(两边都用 git 的 --short=8)。
const char *sb_version_short(void);

// 运行槽标识:"ota_0" / "ota_1" / "factory";未运行在合法分区时返回 "?"。
const char *sb_version_slot(void);
