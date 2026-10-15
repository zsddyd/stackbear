// main/sb_json.c —— 极简 JSON 字符串字段提取。
#include "sb_json.h"

#include <string.h>

bool sb_json_str(const char *json, const char *key, char *out, size_t cap)
{
    if (json == NULL || key == NULL || out == NULL || cap == 0) {
        return false;
    }
    const char *p = strstr(json, key);
    if (p == NULL) {
        return false;
    }
    p += strlen(key);
    // 跳过冒号与空白,直到值的起始引号。
    while (*p == ':' || *p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') {
        p++;
    }
    if (*p != '"') {
        return false;
    }
    p++;
    size_t i = 0;
    while (*p != '\0' && *p != '"') {
        if (i + 1 >= cap) {
            return false;   // 值放不下:宁可失败,也不返回被截断的字符串
        }
        out[i++] = *p++;
    }
    if (*p != '"') {
        return false;       // 引号没闭合
    }
    out[i] = '\0';
    return true;
}
