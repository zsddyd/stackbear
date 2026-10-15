// main/sb_json.h —— 极简 JSON 取字符串字段(纯逻辑,可主机测试)。
//
// 固件只需要从 OTA 清单里读几个字符串(version/sha256/url),为这点需求引入完整
// JSON 库不划算;但这个"够用"的解析曾经写错过(要求 key 后紧跟引号,而实际上中间
// 是冒号),导致 OTA 一直 check failed,所以单独成文件并配主机测试。
#pragma once

#include <stdbool.h>
#include <stddef.h>

// 在 json 中查找 "key"(调用时 key 需含两侧引号,如 "\"url\"")并取其后的字符串值。
// 成功返回 true 并把值写入 out(以 '\0' 结尾);未找到、值不是字符串或放不下返回 false。
bool sb_json_str(const char *json, const char *key, char *out, size_t cap);
