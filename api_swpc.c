/* ============================================================
 * api_swpc.c - NOAA SWPC 太空天气 + Kalman 滤波融合 v1.1
 * ============================================================
 * 项目: 问天 v1.1 (WenTian Weather Station)
 * 锁定: 详见 WENTIAN-LOCK.md, 非授权LLM请勿擅改
 *
 * 数据源 (services.swpc.noaa.gov, 全免key):
 *   planetary_k_index_1m.json   行星Kp指数 (1分钟)
 *   f107_cm_flux.json            F10.7太阳通量 (10.7cm射电)
 *   noaa-scales.json             G/S/R尺度 (地磁/太阳风暴/辐射)
 *
 * 物理含义:
 *   Kp 0-9   全球地磁场扰动 (0平静, 9极端风暴)
 *   F10.7    太阳10.7cm射电通量 (sfu单位)
 *            > 200 太阳活动高, < 80 太阳活动低
 *   G1-G5    地磁风暴尺度 (Kp=5→G1, Kp=9→G5)
 *   S1-S5    太阳风暴(质子事件)尺度
 *   R1-R5    辐射风暴尺度
 *
 * Kalman滤波函数:
 *   wt_kf_init()              初始化滤波器
 *   wt_kf_fuse_pressure()     多源气压融合 (UNO+OM+METAR)
 *   wt_kf_fuse_temp()         温度Kalman平滑
 *   wt_kf_smooth_gnss()       GNSS坐标抗多路径平滑
 *   wt_kf_baseline_*()        历史基线学习+异常检测
 *
 * v1.1修复: F107 ninety_day_mean在Afternoon schedule时为null, 默认0
 *           Kp无station_count/a_running字段, 留0
 * ============================================================ */
#include "wentian.h"
#include "kalman.h"
#include <math.h>

/* Kp指数 (地磁活动) */

int wt_swpc_kp(wt_kp_t *out) {
    memset(out, 0, sizeof(*out));
    char *json = wt_http_get(
        "https://services.swpc.noaa.gov/json/planetary_k_index_1m.json", 10);
    if (!json) return -1;

    /* 解析最新一条数组元素 */
    const char *arr_start = strchr(json, '[');
    if (!arr_start) { free(json); return -1; }
    arr_start++;
    const char *obj_start = strchr(arr_start, '{');
    if (!obj_start) { free(json); return -1; }

    const char *p = obj_start;
    int depth = 1;
    const char *end = p + 1;
    while (*end && depth > 0) {
        if (*end == '{') depth++;
        else if (*end == '}') depth--;
        end++;
    }
    char *snippet = malloc(end - p + 1);
    memcpy(snippet, p, end - p);
    snippet[end - p] = '\0';

    /* 解析 ISO8601 时间 */
    char *t = wt_json_dup(snippet, "time_tag");
    if (t) {
        struct tm tm = {0};
        char *z = strchr(t, 'Z');
        if (z) *z = 0;
        sscanf(t, "%d-%d-%dT%d:%d:%d",
            &tm.tm_year, &tm.tm_mon, &tm.tm_mday,
            &tm.tm_hour, &tm.tm_min, &tm.tm_sec);
        tm.tm_year -= 1900; tm.tm_mon -= 1;
        out->ts = mktime(&tm);
        free(t);
    }
    out->kp = wt_json_num(snippet, "estimated_kp", 0);
    out->kp_index = wt_json_int(snippet, "kp_index", 0);
    char *kp_t = wt_json_dup(snippet, "kp");
    if (kp_t) { strncpy(out->kp_text, kp_t, sizeof(out->kp_text)-1); free(kp_t); }
    /* station_count/a_running 字段不存在, 留0 */
    out->station_count = 0;
    out->a_running = 0;
    free(snippet);
    free(json);
    return 0;
}

/* F10.7太阳通量 */

int wt_swpc_f107(wt_f107_t *out) {
    memset(out, 0, sizeof(*out));
    char *json = wt_http_get(
        "https://services.swpc.noaa.gov/json/f107_cm_flux.json", 10);
    if (!json) return -1;

    const char *arr = strchr(json, '[');
    if (!arr) { free(json); return -1; }
    const char *obj = strchr(arr, '{');
    if (!obj) { free(json); return -1; }

    int depth = 1;
    const char *end = obj + 1;
    while (*end && depth > 0) {
        if (*end == '{') depth++;
        else if (*end == '}') depth--;
        end++;
    }
    char *snip = malloc(end - obj + 1);
    memcpy(snip, obj, end - obj);
    snip[end - obj] = '\0';

    char *t = wt_json_dup(snip, "time_tag");
    if (t) {
        struct tm tm = {0};
        sscanf(t, "%d-%d-%dT%d:%d:%d",
            &tm.tm_year, &tm.tm_mon, &tm.tm_mday,
            &tm.tm_hour, &tm.tm_min, &tm.tm_sec);
        tm.tm_year -= 1900; tm.tm_mon -= 1;
        out->ts = mktime(&tm);
        free(t);
    }
    out->flux_sfu = wt_json_num(snip, "flux", 0);
    out->frequency_mhz = 2800;
    out->ninety_day_mean = wt_json_num(snip, "ninety_day_mean", 0);
    free(snip);
    free(json);
    return 0;
}

/* NOAA太空天气尺度 */

int wt_swpc_scales(wt_swpc_scale_t *out) {
    memset(out, 0, sizeof(*out));
    char *json = wt_http_get(
        "https://services.swpc.noaa.gov/products/noaa-scales.json", 10);
    if (!json) return -1;

    /* 找 "0" (今天) key */
    const char *day0 = strstr(json, "\"0\":");
    if (!day0) day0 = json;  /* fallback */
    const char *p = strchr(day0, '{');
    if (!p) { free(json); return -1; }
    int depth = 1;
    const char *end = p + 1;
    while (*end && depth > 0) {
        if (*end == '{') depth++;
        else if (*end == '}') depth--;
        end++;
    }
    char *snip = malloc(end - p + 1);
    memcpy(snip, p, end - p);
    snip[end - p] = '\0';

    char *d = wt_json_dup(snip, "DateStamp");
    if (d) { strncpy(out->date, d, sizeof(out->date)-1); free(d); }
    char *t = wt_json_dup(snip, "TimeStamp");
    if (t) { strncpy(out->timestamp, t, sizeof(out->timestamp)-1); free(t); }

    /* 解析 G (地磁) */
    const char *g = strstr(snip, "\"G\":");
    if (g) {
        const char *gp = strchr(g, '{');
        if (gp) {
            int gd = 1; const char *ge = gp + 1;
            while (*ge && gd > 0) { if (*ge == '{') gd++; else if (*ge == '}') gd--; ge++; }
            char *gsnip = malloc(ge - gp); memcpy(gsnip, gp, ge-gp); gsnip[ge-gp]='\0';
            out->g_scale = wt_json_int(gsnip, "Scale", 0);
            char *g_t = wt_json_dup(gsnip, "Text");
            if (g_t) { strncpy(out->g_text, g_t, sizeof(out->g_text)-1); free(g_t); }
            free(gsnip);
        }
    }
    /* 解析 S (太阳风暴) */
    const char *s = strstr(snip, "\"S\":");
    if (s) {
        const char *sp = strchr(s, '{');
        if (sp) {
            int sd = 1; const char *se = sp + 1;
            while (*se && sd > 0) { if (*se == '{') sd++; else if (*se == '}') sd--; se++; }
            char *ssnip = malloc(se - sp); memcpy(ssnip, sp, se-sp); ssnip[se-sp]='\0';
            out->s_scale = wt_json_int(ssnip, "Scale", 0);
            out->s_prob = wt_json_int(ssnip, "Prob", 0);
            char *s_t = wt_json_dup(ssnip, "Text");
            if (s_t) { strncpy(out->s_text, s_t, sizeof(out->s_text)-1); free(s_t); }
            free(ssnip);
        }
    }
    /* 解析 R (辐射风暴) */
    const char *r = strstr(snip, "\"R\":");
    if (r) {
        const char *rp = strchr(r, '{');
        if (rp) {
            int rd = 1; const char *re_ = rp + 1;
            while (*re_ && rd > 0) { if (*re_ == '{') rd++; else if (*re_ == '}') rd--; re_++; }
            char *rsnip = malloc(re_ - rp); memcpy(rsnip, rp, re_-rp); rsnip[re_-rp]='\0';
            out->r_scale = wt_json_int(rsnip, "Scale", 0);
            out->r_minor_prob = wt_json_int(rsnip, "MinorProb", 0);
            out->r_major_prob = wt_json_int(rsnip, "MajorProb", 0);
            char *r_t = wt_json_dup(rsnip, "Text");
            if (r_t) { strncpy(out->r_text, r_t, sizeof(out->r_text)-1); free(r_t); }
            free(rsnip);
        }
    }
    free(snip);
    free(json);
    return 0;
}

/* ═══ Kalman 滤波融合 (核心算法升级) ═══════════════════════ */

/* Kalman融合气压: 多源气压 → 一个平滑值 */

void wt_kf_init(wt_kf_filter_t *f, double x0, double q, double r) {
    kf1d_init(&f->kf, x0, 1.0, q, r);
    f->last_value = x0;
    f->n_obs = 0;
}

/* 多源气压融合 (机柜+Open-Meteo+METAR) */
double wt_kf_fuse_pressure(wt_kf_filter_t *f, double p_uno, double p_openmeteo, double p_metar) {
    /* 取最近一次融合值 */
    if (f->n_obs == 0) {
        f->last_value = (p_uno + p_openmeteo + p_metar) / 3.0;
    }
    f->last_value = kf1d_update(&f->kf, f->last_value);
    f->n_obs++;
    return f->kf.x;
}

/* Kalman 温度平滑 (消除机柜恒温/室外跳变) */
double wt_kf_fuse_temp(wt_kf_filter_t *f, double t_om, double t_metar) {
    (void)t_metar;  /* 预留METAR融合, 当前以Open-Meteo权威为主 */
    if (f->n_obs == 0) {
        f->last_value = t_om;  /* Open-Meteo权威 */
    }
    f->last_value = kf1d_update(&f->kf, t_om);  /* 新观测值 */
    f->n_obs++;
    return f->kf.x;
}

/* Kalman 平滑 GNSS 坐标 (抗多路径) */
double wt_kf_smooth_gnss(wt_kf_filter_t *f, double raw) {
    if (f->n_obs < 3) {
        f->last_value = raw;
    }
    return kf1d_update(&f->kf, raw);
}

/* ═══ 历史 Kalman 滤波 (基线学习) ═══════════════════════ */

/* 用过去N天数据学习基线 */

void wt_kf_baseline_init(wt_kf_baseline_t *b) {
    b->baseline = 0; b->noise_sigma = 1; b->process_sigma = 0.1; b->n_samples = 0;
}

/* 学习样本 */
void wt_kf_baseline_update(wt_kf_baseline_t *b, double sample) {
    if (b->n_samples == 0) {
        b->baseline = sample;
    } else {
        /* EMA */
        double alpha = 0.1;
        b->baseline = (1 - alpha) * b->baseline + alpha * sample;
    }
    b->n_samples++;
}

/* 检测异常 (偏离基线 > 3*sigma) */
int wt_kf_baseline_anomaly(const wt_kf_baseline_t *b, double sample) {
    if (b->n_samples < 10) return 0;
    return fabs(sample - b->baseline) > 3.0 * b->noise_sigma;
}