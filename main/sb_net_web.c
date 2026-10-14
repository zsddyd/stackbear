// main/sb_net_web.c —— 配网用的本地配置网页(esp_http_server)。
//
// 设计取舍:为了让 C 侧代码尽量小,C 只提供两个 JSON 接口(/scan 扫热点、/save 保存
// 并验证),页面渲染与交互交给网页里的一小段 JS。所有 HTTP 输入都做长度上限,
// 表单体先收完整再按边界解析,避免小表单也能溢出。
#include <stdlib.h>
#include <string.h>

#include "esp_http_server.h"
#include "esp_log.h"
#include "sb_config.h"
#include "sb_net.h"

static const char *TAG = "sb_prov";
static httpd_handle_t s_httpd;

int sb_net_scan_items(void *out, int max);   // 由 sb_net.c 提供的弱封装
void sb_net_provision_mark(int result);

// sb_net.c 里的扫描结构在本文件重复声明以免暴露内部类型。
#define SB_SCAN_MAX 20
typedef struct {
    char ssid[33];
    int8_t rssi;
} sb_scan_item_t;
int sb_net_scan(sb_scan_item_t *out, int max);

static const char PAGE_HTML[] =
    "<!doctype html><html><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>AI Passport Setup</title><style>"
    "body{background:#0E1216;color:#EDF2F7;font-family:system-ui,sans-serif;margin:0;padding:24px}"
    "h1{font-size:20px;font-weight:600}label{display:block;margin:16px 0 6px;color:#8A97A6;font-size:14px}"
    "select,input{width:100%;box-sizing:border-box;padding:12px;border-radius:10px;border:0;"
    "background:#1B222B;color:#EDF2F7;font-size:16px}"
    "button{margin-top:20px;width:100%;padding:14px;border:0;border-radius:10px;background:#3B82F6;"
    "color:#fff;font-size:16px;font-weight:600}#msg{margin-top:16px;font-size:14px;color:#8A97A6}"
    "</style></head><body><h1>AI Passport Setup</h1>"
    "<label>Network</label><select id='ssid'></select>"
    "<label>Password</label><input id='pass' type='password' autocomplete='off'>"
    "<button id='go'>Connect</button><div id='msg'></div><script>"
    "async function scan(){const r=await fetch('/scan');const j=await r.json();"
    "const s=document.getElementById('ssid');s.innerHTML='';"
    "j.networks.forEach(n=>{const o=document.createElement('option');o.value=n.ssid;"
    "o.textContent=n.ssid+'  ('+n.rssi+'dBm)';s.appendChild(o);});"
    "if(!j.networks.length){s.innerHTML='<option value=\"\">(no networks found)</option>';}}"
    "document.getElementById('go').onclick=async()=>{const m=document.getElementById('msg');"
    "m.textContent='Connecting...';const ssid=document.getElementById('ssid').value;"
    "const pass=document.getElementById('pass').value;"
    "const body=new URLSearchParams({ssid:ssid,password:pass});"
    "const r=await fetch('/save',{method:'POST',body});const j=await r.json();"
    "m.textContent=j.ok?('Connected! Saving '+ssid+'. Device will restart setup shortly.'):"
    "('Failed: '+(j.error||'unknown'));};"
    "scan();setInterval(scan,15000);"
    "</script></body></html>";

static esp_err_t page_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, PAGE_HTML, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t scan_get(httpd_req_t *req)
{
    static sb_scan_item_t items[SB_SCAN_MAX];
    const int n = sb_net_scan(items, SB_SCAN_MAX);
    char *buf = malloc(2048);
    if (buf == NULL) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
    }
    int off = snprintf(buf, 2048, "{\"networks\":[");
    for (int i = 0; i < n && off < 1900; i++) {
        // SSID 可能含引号/反斜杠,做最小转义。
        char esc[80] = {0};
        size_t e = 0;
        for (const char *p = items[i].ssid; *p && e < sizeof(esc) - 2; p++) {
            if (*p == '"' || *p == '\\') {
                esc[e++] = '\\';
            }
            esc[e++] = *p;
        }
        off += snprintf(buf + off, 2048 - off, "%s{\"ssid\":\"%s\",\"rssi\":%d}",
                        i ? "," : "", esc, items[i].rssi);
    }
    snprintf(buf + off, 2048 - off, "]}");
    httpd_resp_set_type(req, "application/json");
    const esp_err_t err = httpd_resp_sendstr(req, buf);
    free(buf);
    return err;
}

static void url_decode(char *s)
{
    char *w = s;
    for (char *r = s; *r; r++) {
        if (*r == '+') {
            *w++ = ' ';
        } else if (*r == '%' && r[1] && r[2]) {
            char hex[3] = {r[1], r[2], 0};
            *w++ = (char)strtol(hex, NULL, 16);
            r += 2;
        } else {
            *w++ = *r;
        }
    }
    *w = '\0';
}

static void form_field(const char *body, const char *key, char *out, size_t cap)
{
    out[0] = '\0';
    char pattern[24];
    snprintf(pattern, sizeof(pattern), "%s=", key);
    const char *p = strstr(body, pattern);
    if (p == NULL) {
        return;
    }
    p += strlen(pattern);
    const char *end = strchr(p, '&');
    size_t len = end ? (size_t)(end - p) : strlen(p);
    if (len >= cap) {
        len = cap - 1;
    }
    memcpy(out, p, len);
    out[len] = '\0';
    url_decode(out);
}

static esp_err_t save_post(httpd_req_t *req)
{
    static char body[512];
    if (req->content_len <= 0 || req->content_len >= (int)sizeof(body)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad length");
    }
    int received = 0;
    while (received < req->content_len) {
        const int r = httpd_req_recv(req, body + received, req->content_len - received);
        if (r <= 0) {
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "recv");
        }
        received += r;
    }
    body[received] = '\0';

    char ssid[SB_SSID_MAX];
    char pass[SB_PASS_MAX];
    form_field(body, "ssid", ssid, sizeof(ssid));
    form_field(body, "password", pass, sizeof(pass));
    if (ssid[0] == '\0') {
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"empty ssid\"}");
    }

    ESP_LOGI(TAG, "尝试连接 '%s' ...", ssid);
    const esp_err_t err = sb_net_sta_connect(ssid, pass);
    if (err == ESP_OK) {
        sb_config_set_wifi(ssid, pass);
        sb_net_provision_mark(1);
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "{\"ok\":true}");
    }
    // 连接失败:保留旧凭据,只回错误。
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"cannot connect\"}");
}

esp_err_t sb_net_provision_web_start(void)
{
    if (s_httpd != NULL) {
        return ESP_OK;
    }
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers = 4;
    cfg.lru_purge_enable = true;
    cfg.stack_size = 6144;
    if (httpd_start(&s_httpd, &cfg) != ESP_OK) {
        s_httpd = NULL;
        return ESP_FAIL;
    }
    const httpd_uri_t page = {.uri = "/", .method = HTTP_GET, .handler = page_get};
    const httpd_uri_t scan = {.uri = "/scan", .method = HTTP_GET, .handler = scan_get};
    const httpd_uri_t save = {.uri = "/save", .method = HTTP_POST, .handler = save_post};
    httpd_register_uri_handler(s_httpd, &page);
    httpd_register_uri_handler(s_httpd, &scan);
    httpd_register_uri_handler(s_httpd, &save);
    ESP_LOGI(TAG, "配置网页已就绪 http://192.168.4.1/");
    return ESP_OK;
}

esp_err_t sb_net_provision_web_stop(void)
{
    if (s_httpd != NULL) {
        httpd_stop(s_httpd);
        s_httpd = NULL;
    }
    return ESP_OK;
}
