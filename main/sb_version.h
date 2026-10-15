// main/sb_version.h —— 固件版本/构建信息。由 CMake 在编译时注入(见 CMakeLists.txt)。
#pragma once

// 构建时由 CMake 定义的字符串。若未定义(例如主机测试),给出安全的占位值。
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

// 一行简短版本(界面标题/日志用),如 "v0.4.0+df4e58d"。
const char *sb_version_short(void);

// 运行槽标识:"ota_0" / "ota_1" / "factory";未运行在合法分区时返回 "?"。
const char *sb_version_slot(void);
