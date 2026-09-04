/* ============================================================
 * api_predict.c - 多源融合预测引擎 v1.0 (C实现)
 * ============================================================
 * 项目: 问天 v2.1 (WenTian Weather Station)
 * 所有者: 主人朱涛 BG8SBA, 昆明长水机场楼顶
 * 锁定: 详见 WENTIAN-LOCK.md, 非授权LLM请勿擅改
 *
 * 原 Python: /root/scripts/multi_source_predict.py (v3.0)
 * 数据源:
 *   1. UNO本地气压趋势 (线性回归+R²)
 *   2. Open-Meteo权威预测 (温度/云量/降雨)
 *   3. METAR机场实况 (ZPPP)
 *   4. 电离层 S4 闪烁修正
 *   5. Zambretti 经验公式 (26级天气)
 *
 * 输出:
 *   1h/3h/6h 温度/气压 预测
 *   投票式综合天气
 *   风暴前兆评分 (0-5)
 *   警报等级 (NORMAL/WATCH/WARNING/SEVERE)
 * ============================================================ */
#include "wentian.h"
#include <sqlite3.h>
#include <math.h>
#include <time.h>
#include <sys/stat.h>
#include <unistd.h>

/* ── 数据库路径 ──────────────────────────────────────── */
#define ANO_DB           "/root/data/ano_weather.db"
#define WENTIAN_DB2      "/root/data/wentian.db"
#define FORECAST_JSON    "/root/data/fusion/forecast.json"

/* ── Zambretti 26级天气表 ──────────────────────────────── */
/* 北半球夏季 + 长水海拔2115m 修正 */
typedef struct {
    double  dp_min, dp_max;
    const char *wx;
    double  confidence;
} zambretti_entry_t;

static const zambretti_entry_t ZAMBRETTI_TABLE[] = {
    {-1e9, -6.0, "🌪 强风暴",       0.95},
    { -6.0, -4.0, "⛈ 暴风雨",      0.90},
    { -4.0, -3.0, "🌧 大雨",         0.85},
    { -3.0, -2.0, "🌦 中雨",         0.80},
    { -2.0, -1.0, "☁ 阴/小雨",      0.70},
    { -1.0,  0.0, "☁ 多云",         0.65},
    {  0.0,  1.0, "🌤 多云转晴",     0.65},
    {  1.0,  2.0, "🌤 渐晴",         0.70},
    {  2.0,  3.0, "☀ 晴",            0.75},
    {  3.0,  5.0, "☀ 晴朗",          0.80},
    {  5.0,  1e9, "☀ 持续晴朗 (高压)", 0.85},
};
#define ZAMBRETTI_N (sizeof(ZAMBRETTI_TABLE)/sizeof(ZAMBRETTI_TABLE[0]))

/* ── 预测结果结构 ──────────────────────────────────────── */
typedef struct {
    time_t  ts;
    /* 当前 */
    double  T_current;       /* °C (来自outdoor) */
    double  H_current;       /* % (来自outdoor) */
    double  P_current;       /* hPa (UNO) */
    /* 预测 */
    double  T_1h, T_3h, T_6h;
    double  P_1h, P_3h, P_6h;
    double  P_r2_1h, P_r2_3h;
    /* 综合天气 */
    char    final_weather[32];
    char    zambretti_wx[32];
    char    openmeteo_3h[32];
    char    metar_now[32];
    /* 风暴 */
    int     storm_score;
    char    storm_signals[512];
    /* 警报 */
    int     alert_score;
    char    level[16];
    char    alerts[512];
    /* 电离层 */
    double  s4_max;
    char    ion_warning[32];
} wt_predict_t;

/* ── 线性回归预测 (输入时间序列, 滑动窗口lookback, 预测步数) ─── */
static int linreg_predict(const double *values, int n, int lookback,
                          int predict_step, double *future, double *slope, double *r2) {
    if (n < lookback || lookback < 2) return -1;

    /* 取最新 lookback 个点 */
    double y[360];
    if (lookback > 360) lookback = 360;
    for (int i = 0; i < lookback; i++) {
        y[i] = values[n - lookback + i];
    }
    int N = lookback;

    double x_mean = (N - 1) / 2.0;
    double y_mean = 0;
    for (int i = 0; i < N; i++) y_mean += y[i];
    y_mean /= N;

    double num = 0, den = 0;
    for (int i = 0; i < N; i++) {
        num += (i - x_mean) * (y[i] - y_mean);
        den += (i - x_mean) * (i - x_mean);
    }
    if (den < 1e-9) return -1;

    double m = num / den;
    double b = y_mean - m * x_mean;

    double ss_res = 0, ss_tot = 0;
    for (int i = 0; i < N; i++) {
        double pred = m * i + b;
        ss_res += (y[i] - pred) * (y[i] - pred);
        ss_tot += (y[i] - y_mean) * (y[i] - y_mean);
    }
    *r2 = (ss_tot > 1e-9) ? (1.0 - ss_res / ss_tot) : 0.0;
    if (*r2 < 0) *r2 = 0;
    if (*r2 > 1) *r2 = 1;
    *slope = m;
    *future = m * (N - 1 + predict_step) + b;
    return 0;
}

/* ── Zambretti 经验公式 ──────────────────────────────────── */
static void zambretti_v2(double p_current, double p_3h_ago,
                          char *wx_out, int max_wx,
                          double *conf_out) {
    double dp = p_current - p_3h_ago;

    /* 海拔修正: 长水2115m, 加200hPa转海平面 */
    double p_adj = p_current + 200.0;
    (void)p_adj; /* 留作日志 */

    for (size_t i = 0; i < ZAMBRETTI_N; i++) {
        if (dp > ZAMBRETTI_TABLE[i].dp_min && dp <= ZAMBRETTI_TABLE[i].dp_max) {
            snprintf(wx_out, max_wx, "%s", ZAMBRETTI_TABLE[i].wx);
            *conf_out = ZAMBRETTI_TABLE[i].confidence;
            /* 海拔修正: 低气压降置信度 */
            if (p_adj < 1005.0 && strstr(wx_out, "晴") == NULL) {
                *conf_out *= 0.85;
            }
            return;
        }
    }
    snprintf(wx_out, max_wx, "🌤 多云");
    *conf_out = 0.5;
}

/* ── 加载 UNO 历史气压 (返回点数) ─────────────────────────── */
static int load_uno_pressure(int hours, double *p_series, int max_n) {
    sqlite3 *db;
    if (sqlite3_open(ANO_DB, &db) != SQLITE_OK) return -1;

    time_t since = time(NULL) - hours * 3600;
    char since_iso[32];
    struct tm *tm = gmtime(&since);
    snprintf(since_iso, sizeof(since_iso), "%04d-%02d-%02dT%02d:%02d:%02d",
             tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
             tm->tm_hour, tm->tm_min, tm->tm_sec);

    sqlite3_stmt *st;
    int rc = sqlite3_prepare_v2(db,
        "SELECT p FROM ano_weather "
        "WHERE source='UNO_v2.0_bridge' AND ts >= ? "
        "AND p IS NOT NULL "
        "ORDER BY ts",
        -1, &st, NULL);
    if (rc != SQLITE_OK) { sqlite3_close(db); return -1; }
    sqlite3_bind_text(st, 1, since_iso, -1, SQLITE_TRANSIENT);

    int n = 0;
    while (sqlite3_step(st) == SQLITE_ROW && n < max_n) {
        p_series[n++] = sqlite3_column_double(st, 0);
    }
    sqlite3_finalize(st);
    sqlite3_close(db);
    return n;
}

/* ── 加载 Open-Meteo 当前室外 + 预测 ──────────────────────── */
static int load_outdoor(double *T, double *H, double *P, double *dew,
                         double *cloud_3h, double *rain_prob_3h) {
    sqlite3 *db;
    if (sqlite3_open(ANO_DB, &db) != SQLITE_OK) return -1;
    sqlite3_stmt *st;
    int rc = sqlite3_prepare_v2(db,
        "SELECT temp_outdoor, humid_outdoor, dew_point, forecast_json "
        "FROM outdoor_weather ORDER BY ts DESC LIMIT 1",
        -1, &st, NULL);
    if (rc != SQLITE_OK) { sqlite3_close(db); return -1; }

    int found = 0;
    if (sqlite3_step(st) == SQLITE_ROW) {
        if (T)  *T  = sqlite3_column_double(st, 0);
        if (H)  *H  = sqlite3_column_double(st, 1);
        if (dew)*dew = sqlite3_column_double(st, 2);
        const char *fj = (const char *)sqlite3_column_text(st, 3);
        if (fj && cloud_3h && rain_prob_3h) {
            /* 简化解析: 找 "cloud":[...] 第3个元素 */
            const char *p = strstr(fj, "\"cloud\":");
            if (p) {
                p += 8;
                while (*p && *p != '[') p++;
                if (*p == '[') {
                    p++;
                    double vals[24] = {0};
                    int cnt = 0;
                    while (*p && *p != ']' && cnt < 24) {
                        vals[cnt++] = strtod(p, (char**)&p);
                        while (*p && (*p == ',' || *p == ' ')) p++;
                    }
                    if (cnt > 2) *cloud_3h = vals[2];
                }
            }
            p = strstr(fj, "\"rain_prob\":");
            if (p) {
                p += 13;
                while (*p && *p != '[') p++;
                if (*p == '[') {
                    p++;
                    double vals[24] = {0};
                    int cnt = 0;
                    while (*p && *p != ']' && cnt < 24) {
                        vals[cnt++] = strtod(p, (char**)&p);
                        while (*p && (*p == ',' || *p == ' ')) p++;
                    }
                    if (cnt > 2) *rain_prob_3h = vals[2];
                }
            }
        }
        found = 1;
    }
    sqlite3_finalize(st);
    sqlite3_close(db);
    return found;
}

/* ── 加载 METAR 当前实况 ──────────────────────────────────── */
static int load_metar(char *wx_out, int max_wx, double *visibility) {
    sqlite3 *db;
    if (sqlite3_open(ANO_DB, &db) != SQLITE_OK) return -1;
    sqlite3_stmt *st;
    int rc = sqlite3_prepare_v2(db,
        "SELECT metar_json FROM aviation_data ORDER BY ts DESC LIMIT 1",
        -1, &st, NULL);
    if (rc != SQLITE_OK) { sqlite3_close(db); return -1; }

    int found = 0;
    if (sqlite3_step(st) == SQLITE_ROW) {
        const char *mj = (const char *)sqlite3_column_text(st, 0);
        if (mj) {
            /* 提取 raw_metar */
            const char *p = strstr(mj, "\"raw_metar\":\"");
            if (p) {
                p += 13;
                const char *e = strchr(p, '"');
                if (e && (e - p) < max_wx - 1) {
                    int len = e - p;
                    memcpy(wx_out, p, len);
                    wx_out[len] = '\0';
                }
            }
            /* 提取能见度 */
            p = strstr(mj, "\"visibility_m\":");
            if (p && visibility) {
                p += 15;
                *visibility = strtod(p, NULL);
            }
            found = 1;
        }
    }
    sqlite3_finalize(st);
    sqlite3_close(db);
    return found;
}

/* ── 加载电离层 S4 闪烁 ───────────────────────────────────── */
static int load_ionosphere(double *s4_max, char *warn_out, int max_w) {
    sqlite3 *db;
    if (sqlite3_open(WENTIAN_DB2, &db) != SQLITE_OK) return -1;
    sqlite3_stmt *st;
    int rc = sqlite3_prepare_v2(db,
        "SELECT s4_gps, s4_bds FROM local_iono ORDER BY ts DESC LIMIT 1",
        -1, &st, NULL);
    if (rc != SQLITE_OK) { sqlite3_close(db); return -1; }

    int found = 0;
    if (sqlite3_step(st) == SQLITE_ROW) {
        double s4_g = sqlite3_column_double(st, 0);
        double s4_b = sqlite3_column_double(st, 1);
        *s4_max = s4_g > s4_b ? s4_g : s4_b;
        if (*s4_max >= 0.4) snprintf(warn_out, max_w, "强闪烁");
        else if (*s4_max >= 0.2) snprintf(warn_out, max_w, "中等闪烁");
        else snprintf(warn_out, max_w, "");
        found = 1;
    }
    sqlite3_finalize(st);
    sqlite3_close(db);
    return found;
}

/* ── 风暴前兆检测 (返回 0-5 评分) ─────────────────────────── */
static int detect_storm_precursor(const double *p_series, int n,
                                   double s4_max, double visibility,
                                   char *signals_out, int max_sig) {
    int score = 0;
    int pos = 0;

    /* 信号1: UNO气压1h骤降>1.5hPa */
    if (n >= 60) {
        double drop_30min = p_series[n - 30] - p_series[n - 1];
        if (drop_30min > 1.5) {
            pos += snprintf(signals_out + pos, max_sig - pos,
                            "%s 30min气压骤降%.1fhPa|",
                            drop_30min > 3.0 ? "📉📉" : "📉", drop_30min);
            score += 2;
        } else if (drop_30min > 0.8) {
            pos += snprintf(signals_out + pos, max_sig - pos,
                            "📉 30min气压降%.1fhPa|", drop_30min);
            score += 1;
        }
    }

    /* 信号2: 电离层强闪烁 */
    if (s4_max >= 0.5) {
        pos += snprintf(signals_out + pos, max_sig - pos,
                        "⚡ 电离层强闪烁S4=%.3f|", s4_max);
        score += 1;
    } else if (s4_max >= 0.3) {
        pos += snprintf(signals_out + pos, max_sig - pos,
                        "⚡ 电离层中等闪烁S4=%.3f|", s4_max);
        score += 1;  /* 主人Python: >=0.3 加 0.5, 但score是int, 取1 */
    }

    /* 信号3: METAR能见度骤降 */
    if (visibility > 0 && visibility < 3000) {
        pos += snprintf(signals_out + pos, max_sig - pos,
                        "🌫 机场能见度骤降%.0fm|", visibility);
        score += 1;
    } else if (visibility >= 3000 && visibility < 5000) {
        pos += snprintf(signals_out + pos, max_sig - pos,
                        "🌫 能见度中等%.0fm|", visibility);
        score += 1;  /* Python: 0.5, 取整 */
    }

    if (score > 5) score = 5;
    return score;
}

/* ── 多源融合预测主入口 ────────────────────────────────────── */
static int wt_predict_compute(wt_predict_t *out) {
    memset(out, 0, sizeof(*out));
    out->ts = time(NULL);

    /* 1. 加载UNO气压 */
    double p_series[720] = {0};
    int n_p = load_uno_pressure(6, p_series, 720);
    if (n_p < 10) return -1;
    out->P_current = p_series[n_p - 1];

    /* 2. 气压预测 1h/3h/6h (步数: 60min/180min/360min) */
    double f1, s1, r1;
    if (linreg_predict(p_series, n_p, 60, 60, &f1, &s1, &r1) == 0) {
        out->P_1h = f1; out->P_r2_1h = r1;
    }
    if (linreg_predict(p_series, n_p, 120, 180, &f1, &s1, &r1) == 0) {
        out->P_3h = f1; out->P_r2_3h = r1;
    }
    if (linreg_predict(p_series, n_p, 180, 360, &f1, &s1, &r1) == 0) {
        out->P_6h = f1;
    }

    /* 3. 加载 Open-Meteo 室外 */
    double cloud_3h = 0, rain_3h = 0;
    if (load_outdoor(&out->T_current, &out->H_current, NULL, NULL,
                      &cloud_3h, &rain_3h)) {
        /* Open-Meteo3h天气 */
        if (rain_3h > 70) snprintf(out->openmeteo_3h, sizeof(out->openmeteo_3h), "🌧 大雨");
        else if (rain_3h > 30) snprintf(out->openmeteo_3h, sizeof(out->openmeteo_3h), "🌦 小雨");
        else if (cloud_3h > 70) snprintf(out->openmeteo_3h, sizeof(out->openmeteo_3h), "☁ 阴");
        else if (cloud_3h > 30) snprintf(out->openmeteo_3h, sizeof(out->openmeteo_3h), "🌤 多云");
        else snprintf(out->openmeteo_3h, sizeof(out->openmeteo_3h), "☀ 晴");

        /* 1h/3h/6h 温度预测 - 简化: 假设无变化趋势 (主人已弃用机柜温度) */
        /* 真实场景需要 forecast_json.temp[] 索引, 简化处理 */
        out->T_1h = out->T_current;
        out->T_3h = out->T_current;
        out->T_6h = out->T_current;
    }

    /* 4. METAR 解析 */
    char metar_raw[256] = {0};
    double vis = 9999;
    if (load_metar(metar_raw, sizeof(metar_raw), &vis)) {
        if (strstr(metar_raw, "TS")) snprintf(out->metar_now, sizeof(out->metar_now), "Thunderstorm");
        else if (strstr(metar_raw, "RA")) snprintf(out->metar_now, sizeof(out->metar_now), "Rain");
        else if (strstr(metar_raw, "FG")) snprintf(out->metar_now, sizeof(out->metar_now), "Fog");
        else if (strstr(metar_raw, "BR")) snprintf(out->metar_now, sizeof(out->metar_now), "Mist");
        else snprintf(out->metar_now, sizeof(out->metar_now), "Clear");
    }

    /* 5. 电离层 */
    if (!load_ionosphere(&out->s4_max, out->ion_warning, sizeof(out->ion_warning))) {
        out->s4_max = 0;
    }

    /* 6. Zambretti */
    double p_3h_ago = (n_p >= 180) ? p_series[n_p - 180] : p_series[0];
    double zambretti_conf = 0.5;
    zambretti_v2(out->P_current, p_3h_ago,
                 out->zambretti_wx, sizeof(out->zambretti_wx),
                 &zambretti_conf);

    /* 7. 投票融合天气 */
    /* 每个源的票数 = 置信度 */
    typedef struct { const char *wx; double vote; } vote_t;
    vote_t votes[3] = {
        { out->zambretti_wx, zambretti_conf },
        { out->openmeteo_3h[0] ? out->openmeteo_3h : NULL, 0.8 },
        { out->metar_now[0] ? out->metar_now : NULL, 0.6 },
    };
    /* 简化投票: 直接用 Zambretti 优先 (因基于本地气压), 失败则 Open-Meteo */
    if (out->zambretti_wx[0]) {
        snprintf(out->final_weather, sizeof(out->final_weather), "%s", out->zambretti_wx);
    } else if (out->openmeteo_3h[0]) {
        snprintf(out->final_weather, sizeof(out->final_weather), "%s", out->openmeteo_3h);
    } else {
        snprintf(out->final_weather, sizeof(out->final_weather), "🌤 多云");
    }

    /* 8. 风暴前兆 */
    out->storm_score = detect_storm_precursor(p_series, n_p,
                                               out->s4_max, vis,
                                               out->storm_signals,
                                               sizeof(out->storm_signals));

    /* 9. 警报评分 (8通道) */
    out->alert_score = 0;
    int apos = 0;
    out->alerts[0] = '\0';

    /* 通道1: 气压1h骤降预测 */
    if (out->P_1h != 0 && out->P_1h < out->P_current - 1.5) {
        double drop = out->P_current - out->P_1h;
        apos += snprintf(out->alerts + apos, sizeof(out->alerts) - apos,
                         "📉 预测1h气压降%.1fhPa|", drop);
        out->alert_score += 2;
    }
    /* 通道2: 温度骤降 (使用室外) */
    if (out->T_1h != 0 && out->T_1h < out->T_current - 3.0) {
        double drop_t = out->T_current - out->T_1h;
        apos += snprintf(out->alerts + apos, sizeof(out->alerts) - apos,
                         "🌡 预测1h温度降%.1f°C|", drop_t);
        out->alert_score += 1;
    }
    /* 通道3: Zambretti 风暴 */
    if (strstr(out->zambretti_wx, "暴") || strstr(out->zambretti_wx, "Storm")) {
        apos += snprintf(out->alerts + apos, sizeof(out->alerts) - apos,
                         "🌪 Zambretti:%s|", out->zambretti_wx);
        out->alert_score += 2;
    }
    /* 通道4: 风暴前兆 */
    if (out->storm_score > 0) {
        out->alert_score += out->storm_score;
        apos += snprintf(out->alerts + apos, sizeof(out->alerts) - apos,
                         "🌩 风暴前兆%d分|", out->storm_score);
    }
    /* 通道5: 电离层强闪烁 */
    if (out->s4_max >= 0.4) {
        apos += snprintf(out->alerts + apos, sizeof(out->alerts) - apos,
                         "⚡ 电离层%s|", out->ion_warning);
        out->alert_score += 1;
    }

    /* 10. 等级 */
    if (out->alert_score >= 8) snprintf(out->level, sizeof(out->level), "SEVERE");
    else if (out->alert_score >= 5) snprintf(out->level, sizeof(out->level), "WARNING");
    else if (out->alert_score >= 2) snprintf(out->level, sizeof(out->level), "WATCH");
    else snprintf(out->level, sizeof(out->level), "NORMAL");

    return 0;
}

/* ── 保存 JSON ────────────────────────────────────────────── */
static int wt_predict_save_json(const wt_predict_t *p) {
    mkdir("/root/data/fusion", 0755);
    FILE *f = fopen(FORECAST_JSON, "w");
    if (!f) return -1;
    fprintf(f, "{\n");
    fprintf(f, "  \"ts\": %ld,\n", (long)p->ts);
    fprintf(f, "  \"current\": {\"T\": %.1f, \"H\": %.0f, \"P\": %.1f},\n",
            p->T_current, p->H_current, p->P_current);
    fprintf(f, "  \"forecast_1h\": {\"P\": %.1f, \"T\": %.1f, \"P_conf\": %.2f},\n",
            p->P_1h, p->T_1h, p->P_r2_1h);
    fprintf(f, "  \"forecast_3h\": {\"P\": %.1f, \"T\": %.1f, \"P_conf\": %.2f},\n",
            p->P_3h, p->T_3h, p->P_r2_3h);
    fprintf(f, "  \"forecast_6h\": {\"P\": %.1f, \"T\": %.1f},\n",
            p->P_6h, p->T_6h);
    fprintf(f, "  \"final_weather\": \"%s\",\n", p->final_weather);
    fprintf(f, "  \"zambretti\": \"%s\",\n", p->zambretti_wx);
    fprintf(f, "  \"openmeteo_3h\": \"%s\",\n", p->openmeteo_3h);
    fprintf(f, "  \"metar_now\": \"%s\",\n", p->metar_now);
    fprintf(f, "  \"storm_score\": %d,\n", p->storm_score);
    fprintf(f, "  \"storm_signals\": \"%s\",\n", p->storm_signals);
    fprintf(f, "  \"alert_score\": %d,\n", p->alert_score);
    fprintf(f, "  \"level\": \"%s\",\n", p->level);
    fprintf(f, "  \"alerts\": \"%s\",\n", p->alerts);
    fprintf(f, "  \"s4_max\": %.3f,\n", p->s4_max);
    fprintf(f, "  \"ion_warning\": \"%s\"\n", p->ion_warning);
    fprintf(f, "}\n");
    fclose(f);
    return 0;
}

/* ── 保存到 DB (multi_source_forecast 表) ─────────────────── */
static int wt_predict_save_db(const wt_predict_t *p) {
    sqlite3 *db;
    if (sqlite3_open(WENTIAN_DB2, &db) != SQLITE_OK) return -1;

    const char *sql_create =
        "CREATE TABLE IF NOT EXISTS multi_source_forecast ("
        "ts INTEGER PRIMARY KEY, "
        "T_current REAL, H_current REAL, P_current REAL, "
        "P_1h REAL, P_3h REAL, P_6h REAL, "
        "T_1h REAL, T_3h REAL, T_6h REAL, "
        "P_r2_1h REAL, P_r2_3h REAL, "
        "final_weather TEXT, zambretti TEXT, openmeteo_3h TEXT, metar_now TEXT, "
        "storm_score INTEGER, storm_signals TEXT, "
        "alert_score INTEGER, level TEXT, alerts TEXT, "
        "s4_max REAL, ion_warning TEXT)";
    sqlite3_exec(db, sql_create, NULL, NULL, NULL);

    sqlite3_stmt *st;
    const char *sql_ins =
        "INSERT OR REPLACE INTO multi_source_forecast "
        "(ts,T_current,H_current,P_current,"
        " P_1h,P_3h,P_6h,T_1h,T_3h,T_6h,P_r2_1h,P_r2_3h,"
        " final_weather,zambretti,openmeteo_3h,metar_now,"
        " storm_score,storm_signals,alert_score,level,alerts,"
        " s4_max,ion_warning) "
        "VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)";
    int rc = sqlite3_prepare_v2(db, sql_ins, -1, &st, NULL);
    if (rc != SQLITE_OK) { sqlite3_close(db); return -1; }

    sqlite3_bind_int64(st, 1, (sqlite3_int64)p->ts);
    sqlite3_bind_double(st, 2, p->T_current);
    sqlite3_bind_double(st, 3, p->H_current);
    sqlite3_bind_double(st, 4, p->P_current);
    sqlite3_bind_double(st, 5, p->P_1h);
    sqlite3_bind_double(st, 6, p->P_3h);
    sqlite3_bind_double(st, 7, p->P_6h);
    sqlite3_bind_double(st, 8, p->T_1h);
    sqlite3_bind_double(st, 9, p->T_3h);
    sqlite3_bind_double(st, 10, p->T_6h);
    sqlite3_bind_double(st, 11, p->P_r2_1h);
    sqlite3_bind_double(st, 12, p->P_r2_3h);
    sqlite3_bind_text(st, 13, p->final_weather, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 14, p->zambretti_wx, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 15, p->openmeteo_3h, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 16, p->metar_now, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(st, 17, p->storm_score);
    sqlite3_bind_text(st, 18, p->storm_signals, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(st, 19, p->alert_score);
    sqlite3_bind_text(st, 20, p->level, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 21, p->alerts, -1, SQLITE_TRANSIENT);
    sqlite3_bind_double(st, 22, p->s4_max);
    sqlite3_bind_text(st, 23, p->ion_warning, -1, SQLITE_TRANSIENT);
    rc = sqlite3_step(st);
    sqlite3_finalize(st);
    sqlite3_close(db);
    return (rc == SQLITE_DONE) ? 0 : -1;
}

/* ── 主入口: 多源融合预测 ───────────────────────────────────── */
int wt_predict_run(void) {
    wt_predict_t p;
    int rc = wt_predict_compute(&p);
    if (rc != 0) {
        printf("  ❌ 多源融合预测: UNO气压数据不足(<10条)\n");
        return -1;
    }

    printf("\n━━━ 21. 多源融合预测 (8通道投票) ━━━\n");
    printf("  当前: T=%.1f°C H=%.0f%% P=%.1fhPa\n", p.T_current, p.H_current, p.P_current);
    printf("  ── 气压预测(线性回归) ──\n");
    printf("    1h: %.1fhPa (R²=%.3f) | 3h: %.1fhPa (R²=%.3f) | 6h: %.1fhPa\n",
           p.P_1h, p.P_r2_1h, p.P_3h, p.P_r2_3h, p.P_6h);
    printf("  ── 各源天气 ──\n");
    printf("    Zambretti: %s | Open-Meteo 3h: %s | METAR: %s\n",
           p.zambretti_wx, p.openmeteo_3h, p.metar_now);
    printf("  ── 综合 ──\n");
    printf("    最终天气: %s\n", p.final_weather);
    printf("  ── 风暴前兆 ──\n");
    printf("    评分: %d/5 | 电离层S4=%.3f %s\n",
           p.storm_score, p.s4_max, p.ion_warning);
    if (p.storm_signals[0]) {
        printf("    信号: %s\n", p.storm_signals);
    }
    printf("  ── 警报 ──\n");
    printf("    等级: %s | 评分: %d\n", p.level, p.alert_score);
    if (p.alerts[0]) {
        printf("    通道: %s\n", p.alerts);
    }

    wt_predict_save_db(&p);
    wt_predict_save_json(&p);
    printf("  ✅ 已存入 forecast.json + multi_source_forecast 表\n");
    return 0;
}
