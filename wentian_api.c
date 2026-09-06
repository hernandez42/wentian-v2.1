/* ============================================================
 * wentian_api.c - 问天气象站 HTTP/JSON 工具实现 v1.1
 * ============================================================
 * 项目: 问天 v1.1 (WenTian Weather Station)
 * 锁定: 详见 WENTIAN-LOCK.md, 非授权LLM请勿擅改
 *
 * 模块职责:
 *   1. wt_buf_t 动态字符串缓冲 (无json-c依赖)
 *   2. wt_http_get/post libcurl封装 (含重试2次 + TCP优化)
 *   3. wt_json_dup/n "dup_arr"/num/int 手写JSON字段提取
 *
 * 关键约束:
 *   - 内存: 返回的字符串必须由调用者free
 *   - HTTP: 连接8秒超时, 总超时由调用者指定, 失败重试1次
 *   - JSON: 兼容字符串数字 (wttr.in是 "22" 不是 22)
 *
 * 不在此模块:
 *   - API URL常量 (在各 api_*.c 中)
 *   - SQLite封装 (在 wentian_db.c)
 *   - Kalman滤波 (在 kalman.h)
 * ============================================================ */
#include "wentian.h"
#include <curl/curl.h>
#include <ctype.h>
#include <unistd.h>

/* ── 缓冲 ────────────────────────────────────────────────── */
void wt_buf_init(wt_buf_t *b) { b->data = NULL; b->size = b->cap = 0; }
void wt_buf_free(wt_buf_t *b) { free(b->data); b->data = NULL; b->size = b->cap = 0; }

int wt_buf_append(wt_buf_t *b, const char *p, size_t n) {
    if (b->size + n + 1 > b->cap) {
        size_t new_cap = b->cap ? b->cap * 2 : 4096;
        while (new_cap < b->size + n + 1) new_cap *= 2;
        char *p2 = realloc(b->data, new_cap);
        if (!p2) return -1;
        b->data = p2;
        b->cap = new_cap;
    }
    memcpy(b->data + b->size, p, n);
    b->size += n;
    b->data[b->size] = '\0';
    return 0;
}

/* ── libcurl 写回调 ────────────────────────────────────── */
static size_t curl_write_cb(char *ptr, size_t size, size_t nmemb, void *userp) {
    return wt_buf_append((wt_buf_t *)userp, ptr, size * nmemb) == 0 ? size * nmemb : 0;
}

/* ── HTTP GET/POST ─────────────────────────────────────── */
static char *do_http(const char *method, const char *url, const char *body, int timeout) {
    CURL *curl;
    CURLcode res;
    wt_buf_t buf;
    wt_buf_init(&buf);

    curl_global_init(CURL_GLOBAL_DEFAULT);
    curl = curl_easy_init();
    if (!curl) { wt_buf_free(&buf); return NULL; }

    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "User-Agent: " WENTIAN_USER_AGENT);
    if (body) {
        headers = curl_slist_append(headers, "Content-Type: application/json");
    }

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&buf);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, (long)timeout);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 8L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(curl, CURLOPT_IPRESOLVE, CURL_IPRESOLVE_V4);
    curl_easy_setopt(curl, CURLOPT_TCP_FASTOPEN, 1L);
    curl_easy_setopt(curl, CURLOPT_TCP_NODELAY, 1L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

    if (strcmp(method, "POST") == 0) {
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        if (body) curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
    }

    /* 重试2次 */
    int attempt;
    for (attempt = 0; attempt < 2; attempt++) {
        res = curl_easy_perform(curl);
        if (res == CURLE_OK) break;
        fprintf(stderr, "[HTTP] %s %s 失败(尝试%d/%d): %s\n",
            method, url, attempt+1, 2, curl_easy_strerror(res));
        wt_buf_free(&buf);  /* 清空重试, free 防泄漏 */
        wt_buf_init(&buf);
        if (attempt == 0) sleep(1);
    }
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    curl_global_cleanup();

    if (res != CURLE_OK) {
        wt_buf_free(&buf);
        return NULL;
    }
    return buf.data ? buf.data : strdup("");
}

char *wt_http_get(const char *url, int timeout_sec) {
    return do_http("GET", url, NULL, timeout_sec);
}

char *wt_http_post(const char *url, const char *body, int timeout_sec) {
    return do_http("POST", url, body, timeout_sec);
}

/* ── JSON 字段提取 ────────────────────────────────────── */

/* 找 "key":"..." 字段值 (调用者free返回的字符串) */
char *wt_json_dup(const char *json, const char *key) {
    if (!json || !key) return NULL;
    char pat[128];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(json, pat);
    if (!p) return NULL;
    p += strlen(pat);
    while (*p == ' ' || *p == '\t' || *p == '\n') p++;
    if (*p != ':') return NULL;
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\n') p++;
    if (*p != '"') return NULL;
    p++;
    /* 找下一个未转义的" */
    const char *start = p;
    while (*p && *p != '"') {
        if (*p == '\\' && *(p+1)) p++;
        p++;
    }
    size_t len = p - start;
    char *out = malloc(len + 1);
    if (!out) return NULL;
    /* 简单反转义 */
    size_t j = 0;
    for (size_t i = 0; i < len; i++) {
        if (start[i] == '\\' && i+1 < len) {
            char c = start[++i];
            switch (c) {
                case 'n': out[j++] = '\n'; break;
                case 'r': out[j++] = '\r'; break;
                case 't': out[j++] = '\t'; break;
                case '"': out[j++] = '"'; break;
                case '\\': out[j++] = '\\'; break;
                default: out[j++] = c;
            }
        } else {
            out[j++] = start[i];
        }
    }
    out[j] = '\0';
    return out;
}

/* "key":[v1,v2,...] 取第idx个元素 (strdup) */
char *wt_json_dup_arr(const char *json, const char *key, int idx) {
    if (!json || !key) return NULL;
    char pat[128];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(json, pat);
    if (!p) return NULL;
    p += strlen(pat);
    while (*p == ' ' || *p == '\t' || *p == '\n') p++;
    if (*p != ':') return NULL;
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\n') p++;
    if (*p != '[') return NULL;
    p++;
    /* 跳过 idx 个逗号 */
    for (int i = 0; i < idx; i++) {
        int depth = 0;
        while (*p) {
            if (*p == '[' || *p == '{') depth++;
            else if (*p == ']' || *p == '}') {
                if (depth == 0) return NULL;
                depth--;
            }
            else if (*p == ',' && depth == 0) { p++; break; }
            p++;
        }
    }
    /* 读一个值 */
    const char *start = p;
    int depth = 0;
    while (*p && !(depth == 0 && (*p == ',' || *p == ']'))) {
        if (*p == '[' || *p == '{') depth++;
        else if (*p == ']' || *p == '}') {
            if (depth == 0) break;
            depth--;
        }
        p++;
    }
    size_t len = p - start;
    char *out = malloc(len + 1);
    if (!out) return NULL;
    memcpy(out, start, len);
    out[len] = '\0';
    /* 修剪首尾空白 */
    char *end = out + len - 1;
    while (end > out && isspace((unsigned char)*end)) { *end-- = '\0'; len--; }
    char *s = out;
    while (*s && isspace((unsigned char)*s)) { s++; len--; }
    if (s != out) memmove(out, s, len + 1);
    return out;
}

/* 找数字字段 - 兼容字符串形式 "22" 和裸数字 22 */
static const char *find_num(const char *json, const char *key) {
    char pat[128];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(json, pat);
    if (!p) return NULL;
    p += strlen(pat);
    while (*p == ' ' || *p == '\t' || *p == '\n') p++;
    if (*p != ':') return NULL;
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\n') p++;
    /* 兼容字符串值 "22" - 跳过首引号 */
    if (*p == '"') p++;
    return p;
}

/* ── 带 NaN 默认值的 JSON 数值提取 (替代硬编码0默认值) ───── */
double wt_json_num_nan(const char *json, const char *key) {
    const char *p = find_num(json, key);
    if (!p) return NAN;
    char *endp;
    double v = strtod(p, &endp);
    return (endp == p) ? NAN : v;
}

/* ── 带 -1 默认值的 JSON 整数提取 (替代硬编码0默认值) ────── */
int wt_json_int_neg1(const char *json, const char *key) {
    const char *p = find_num(json, key);
    if (!p) return -1;
    char *endp;
    long v = strtol(p, &endp, 10);
    return (endp == p) ? -1 : (int)v;
}

/* 声明在 wentian_api.h */

double wt_json_num(const char *json, const char *key, double defval) {
    const char *p = find_num(json, key);
    if (!p) return defval;
    char *endp;
    double v = strtod(p, &endp);
    return (endp == p) ? defval : v;
}

int wt_json_int(const char *json, const char *key, int defval) {
    const char *p = find_num(json, key);
    if (!p) return defval;
    char *endp;
    long v = strtol(p, &endp, 10);
    return (endp == p) ? defval : (int)v;
}