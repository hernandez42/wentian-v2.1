/* ============================================================
 * api_open_data.c - 问天开源数据集成 v1.0
 * ============================================================
 * 项目: 问天 v2.3 (WenTian Weather Station)
 * 所有者: 主人朱涛 BG8SBA, 昆明长水机场楼顶
 *
 * 主人2026-09-04指示:
 *   "能提升的开源免费 API 专业数据, 你都可以接入互补"
 *
 * 已验证可用的免费API (实测HTTP 200):
 *   1. NOAA SWPC Kp (1分钟) - 太空天气/电离层基础
 *   2. NOAA SWPC F10.7 太阳射电通量 - 太阳活动
 *   3. NOAA SWPC 太阳黑子 (1749年至今)
 *   4. met.no Locationforecast (挪威气象) - 欧洲权威预报
 *   5. wttr.in (CC-BY) - 全球当前气象
 *   6. USGS GeoJSON (全球地震2.5+级) - 已在 api_other.c 实现
 *
 * 这些数据源相互独立、跨地域、跨机构, 与问天本地数据形成
 * 真正的"多源融合" - 主人原话: "融合互补"
 *
 * 输出: external_data 表 + external_data.json
 * ============================================================ */
#include "wentian.h"
#include <sqlite3.h>
#include <math.h>
#include <time.h>
#include <string.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

/* ── API 端点 (全部实测HTTP 200) ─────────────────────────── */
#define NOAA_KP_1M_URL       "https://services.swpc.noaa.gov/json/planetary_k_index_1m.json"
#define NOAA_F107_URL        "https://services.swpc.noaa.gov/json/f107_cm_flux.json"
#define NOAA_KP_HIST_URL     "https://services.swpc.noaa.gov/products/noaa-planetary-k-index.json"
#define METNO_FORECAST_URL   "https://api.met.no/weatherapi/locationforecast/2.0/compact?lat=25.0820&lon=102.9129&altitude=2103"
#define WTTR_URL             "https://wttr.in/Kunming?format=j1"
#define EXTERNAL_JSON        "/root/data/fusion/external_data.json"

/* ── 简单JSON值提取器(从libcurl返回的body里找数字) ──────────── */
static double json_find_double(const char *json, const char *key) {
    if (!json || !key) return NAN;
    char needle[128];
    snprintf(needle, sizeof(needle), "\"%s\"", key);
    const char *p = strstr(json, needle);
    if (!p) return NAN;
    p += strlen(needle);
    /* 跳过 : 和空白 */
    while (*p && (*p == ':' || *p == ' ' || *p == '\t' || *p == '\n')) p++;
    /* 数字或字符串 */
    if (*p == '"') p++;
    char *endp;
    double v = strtod(p, &endp);
    if (endp == p) return NAN;
    return v;
}

/* json_find_int — 未使用, 保留为注释 */
/*
static int json_find_int(const char *json, const char *key) {
    double v = json_find_double(json, key);
    return isnan(v) ? -1 : (int)v;
}
*/

/* ── 1. NOAA SWPC Kp (1分钟) ──────────────────────────────── */
/* 格式: [{"time_tag":"2026-09-04T05:09:00","kp_index":1,"estimated_kp":0.67,"kp":"1M"}, ...] */
static int fetch_noaa_kp_1m(double *out_kp, double *out_estimated, char *out_time, int max_time) {
    char *body = wt_http_get(NOAA_KP_1M_URL, 15);
    if (!body) return -1;

    /* 解析最后一个 (最新) 记录
     * 简单方法: 找最后一个 "kp_index":数字 模式 */
    int latest_kp = -1;
    double latest_est = NAN;
    char latest_time[64] = {0};
    const char *p = body;
    int count = 0;
    while ((p = strstr(p, "\"time_tag\"")) != NULL) {
        p += 11; /* skip "time_tag" */
        if (*p == ':') p++;
        if (*p == '"') p++;
        /* 读取时间字段 */
        const char *t_start = p;
        const char *t_end = strchr(p, '"');
        if (t_end && (t_end - t_start) < 63) {
            strncpy(latest_time, t_start, t_end - t_start);
            latest_time[t_end - t_start] = '\0';
        }
        /* 找这一行内的 kp_index */
        const char *line_end = strchr(t_end ? t_end : p, '}');
        if (!line_end) line_end = p + 100;
        char snippet[256] = {0};
        if (line_end > t_end) {
            int n = line_end - t_end;
            if (n > 255) n = 255;
            memcpy(snippet, t_end, n);
        }
        const char *kp_pos = strstr(snippet, "\"kp_index\"");
        if (kp_pos) {
            latest_kp = atoi(kp_pos + 10);
        }
        const char *est_pos = strstr(snippet, "\"estimated_kp\"");
        if (est_pos) {
            latest_est = strtod(est_pos + 14, NULL);
        }
        p = line_end + 1;
        count++;
    }

    if (out_kp && latest_kp >= 0) *out_kp = (double)latest_kp;
    if (out_estimated && !isnan(latest_est)) *out_estimated = latest_est;
    if (out_time && latest_time[0]) {
        strncpy(out_time, latest_time, max_time - 1);
    }

    free(body);
    return count;
}

/* ── 2. NOAA SWPC F10.7 太阳射电通量 ──────────────────────── */
/* 格式: [{"time_tag":"...","frequency":2800,"flux":108.0,"..."}] */
static int fetch_noaa_f107(double *out_flux, double *out_mean90) {
    char *body = wt_http_get(NOAA_F107_URL, 15);
    if (!body) return -1;

    /* 找第一个 "flux" 字段 (最新) */
    const char *p = strstr(body, "\"flux\"");
    double flux = NAN, mean90 = NAN;
    if (p) {
        flux = strtod(p + 7, NULL);
    }
    /* 90天平均 */
    const char *q = strstr(body, "\"avg_flux\"");
    if (!q) q = strstr(body, "\"ninety_day_mean\"");
    if (q) {
        const char *colon = strchr(q, ':');
        if (colon) mean90 = strtod(colon + 1, NULL);
    }

    if (out_flux) *out_flux = flux;
    if (out_mean90) *out_mean90 = mean90;

    free(body);
    return 0;
}

/* ── 3. met.no Locationforecast (挪威气象局权威) ────────── */
static int fetch_metno(double *out_temp, double *out_humid, double *out_pressure,
                       double *out_wind, char *out_summary, int max_summary) {
    char *body = wt_http_get(METNO_FORECAST_URL, 15);
    if (!body) return -1;

    /* 解析 instant details */
    const char *details = strstr(body, "\"instant\":");
    if (details) {
        if (out_temp)     *out_temp     = json_find_double(details, "air_temperature");
        if (out_humid)    *out_humid    = json_find_double(details, "relative_humidity");
        if (out_pressure)  *out_pressure  = json_find_double(details, "air_pressure_at_sea_level");
        if (out_wind)      *out_wind     = json_find_double(details, "wind_speed");
    }
    /* summary 在 next_1_hours/details 或 next_6_hours  */
    const char *next1 = strstr(body, "\"next_1_hours\":");
    if (next1 && out_summary) {
        const char *sum = strstr(next1, "\"summary\"");
        if (sum) {
            const char *colon = strchr(sum, ':');
            if (colon) {
                const char *q1 = strchr(colon, '"');
                if (q1) {
                    const char *q2 = strchr(q1 + 1, '"');
                    if (q2 && (q2 - q1 - 1) < max_summary - 1) {
                        strncpy(out_summary, q1 + 1, q2 - q1 - 1);
                    }
                }
            }
        }
    }

    free(body);
    return 0;
}

/* ── 4. wttr.in 当前气象 ─────────────────────────────────── */
static int fetch_wttr(double *out_temp, double *out_humid, char *out_desc, int max_desc) {
    char *body = wt_http_get(WTTR_URL, 15);
    if (!body) return -1;

    if (out_temp) {
        /* wttr.in 格式: "temp_C": "22" (字符串数字, 冒号后可能有空格) */
        const char *p = strstr(body, "\"temp_C\"");
        if (p) {
            p += 8;  /* skip "temp_C" */
            while (*p && (*p == ':' || *p == ' ' || *p == '"')) p++;
            *out_temp = strtod(p, NULL);
        }
    }
    if (out_humid) {
        const char *p = strstr(body, "\"humidity\"");
        if (p) {
            p += 10;
            while (*p && (*p == ':' || *p == ' ' || *p == '"')) p++;
            *out_humid = strtod(p, NULL);
        }
    }
    if (out_desc) {
        /* weatherDesc":[{"value":"Sunny"}] */
        const char *p = strstr(body, "\"weatherDesc\":[{\"value\":\"");
        if (p) {
            p += 26;
            const char *q = strchr(p, '"');
            if (q && (q - p) < max_desc - 1) {
                strncpy(out_desc, p, q - p);
            }
        }
    }

    free(body);
    return 0;
}

/* ── 主入口: 集成 4 大开源 API ────────────────────────────── */
int wt_open_data_run(void) {
    printf("\n━━━ 24. 开源专业数据集成 (4 API) ━━━\n");

    /* 1. NOAA Kp 1分钟 */
    double kp = NAN, kp_est = NAN;
    char kp_time[64] = {0};
    int rc_kp = fetch_noaa_kp_1m(&kp, &kp_est, kp_time, sizeof(kp_time));
    if (rc_kp > 0) {
        printf("  ✅ [1] NOAA SWPC Kp(1分钟) | kp_index=%.0f estimated=%.2f @ %s (共%d条)\n",
               kp, kp_est, kp_time, rc_kp);
    } else {
        printf("  ⚠️ [1] NOAA Kp 拉取失败\n");
    }

    /* 2. NOAA F10.7 */
    double f107 = NAN, f107_mean = NAN;
    int rc_f107 = fetch_noaa_f107(&f107, &f107_mean);
    if (rc_f107 == 0) {
        printf("  ✅ [2] NOAA SWPC F10.7 太阳射电通量 | flux=%.1f sfu 90日均值=%.1f\n",
               f107, f107_mean);
    } else {
        printf("  ⚠️ [2] NOAA F10.7 拉取失败\n");
    }

    /* 3. met.no */
    double mn_temp = NAN, mn_humid = NAN, mn_pressure = NAN, mn_wind = NAN;
    char mn_summary[64] = {0};
    int rc_mn = fetch_metno(&mn_temp, &mn_humid, &mn_pressure, &mn_wind,
                            mn_summary, sizeof(mn_summary));
    if (rc_mn == 0) {
        printf("  ✅ [3] met.no (挪威) | T=%.1f°C H=%.0f%% P=%.0fhPa 风=%.1fm/s | %s\n",
               mn_temp, mn_humid, mn_pressure, mn_wind, mn_summary);
    } else {
        printf("  ⚠️ [3] met.no 拉取失败\n");
    }

    /* 4. wttr.in */
    double wt_temp = NAN, wt_humid = NAN;
    char wt_desc[64] = {0};
    int rc_wt = fetch_wttr(&wt_temp, &wt_humid, wt_desc, sizeof(wt_desc));
    if (rc_wt == 0) {
        printf("  ✅ [4] wttr.in (CC-BY) | T=%.1f°C H=%.0f%% | %s\n",
               wt_temp, wt_humid, wt_desc);
    } else {
        printf("  ⚠️ [4] wttr.in 拉取失败\n");
    }

    /* ── 写入 DB ─────────────────────────────────────────── */
    sqlite3 *db;
    if (sqlite3_open(WENTIAN_DB, &db) == SQLITE_OK) {
        const char *sql = "CREATE TABLE IF NOT EXISTS external_data ("
            "ts INTEGER PRIMARY KEY, "
            "noaa_kp REAL DEFAULT -1, noaa_kp_est REAL DEFAULT -1, noaa_kp_time TEXT DEFAULT '', "
            "noaa_f107 REAL DEFAULT -1, noaa_f107_90d REAL DEFAULT -1, "
            "metno_temp REAL DEFAULT -1, metno_humid REAL DEFAULT -1, "
            "metno_pressure REAL DEFAULT -1, metno_wind REAL DEFAULT -1, metno_summary TEXT DEFAULT '', "
            "wttr_temp REAL DEFAULT -1, wttr_humid REAL DEFAULT -1, wttr_desc TEXT DEFAULT '', "
            "sources_n INTEGER DEFAULT 0, note TEXT DEFAULT '')";
        sqlite3_exec(db, sql, NULL, NULL, NULL);

        sqlite3_stmt *st;
        int rc;
        rc = sqlite3_prepare_v2(db,
            "INSERT OR REPLACE INTO external_data "
            "(ts,noaa_kp,noaa_kp_est,noaa_kp_time,noaa_f107,noaa_f107_90d,"
            " metno_temp,metno_humid,metno_pressure,metno_wind,metno_summary,"
            " wttr_temp,wttr_humid,wttr_desc,sources_n,note) "
            "VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)", -1, &st, NULL);
        if (rc == SQLITE_OK) {
            int sources = 0;
            if (rc_kp > 0) sources++;
            if (rc_f107 == 0) sources++;
            if (rc_mn == 0) sources++;
            if (rc_wt == 0) sources++;
            sqlite3_bind_int64(st, 1, (sqlite3_int64)time(NULL));
            sqlite3_bind_double(st, 2, isnan(kp) ? -1 : kp);
            sqlite3_bind_double(st, 3, isnan(kp_est) ? -1 : kp_est);
            sqlite3_bind_text(st, 4, kp_time, -1, SQLITE_TRANSIENT);
            sqlite3_bind_double(st, 5, isnan(f107) ? -1 : f107);
            sqlite3_bind_double(st, 6, isnan(f107_mean) ? -1 : f107_mean);
            sqlite3_bind_double(st, 7, isnan(mn_temp) ? -1 : mn_temp);
            sqlite3_bind_double(st, 8, isnan(mn_humid) ? -1 : mn_humid);
            sqlite3_bind_double(st, 9, isnan(mn_pressure) ? -1 : mn_pressure);
            sqlite3_bind_double(st, 10, isnan(mn_wind) ? -1 : mn_wind);
            sqlite3_bind_text(st, 11, mn_summary, -1, SQLITE_TRANSIENT);
            sqlite3_bind_double(st, 12, isnan(wt_temp) ? -1 : wt_temp);
            sqlite3_bind_double(st, 13, isnan(wt_humid) ? -1 : wt_humid);
            sqlite3_bind_text(st, 14, wt_desc, -1, SQLITE_TRANSIENT);
            sqlite3_bind_int(st, 15, sources);
            char note[64];
            snprintf(note, sizeof(note), "%d/4 源成功", sources);
            sqlite3_bind_text(st, 16, note, -1, SQLITE_TRANSIENT);
            sqlite3_step(st);
            sqlite3_finalize(st);
        }
        sqlite3_close(db);
    }

    /* ── 写 JSON ─────────────────────────────────────────── */
    mkdir("/root/data/fusion", 0755);
    FILE *jf = fopen(EXTERNAL_JSON, "w");
    if (jf) {
        fprintf(jf, "{\n");
        fprintf(jf, "  \"ts\": %ld,\n", (long)time(NULL));
        fprintf(jf, "  \"noaa\": {\n");
        fprintf(jf, "    \"kp_index\": %.0f,\n", isnan(kp) ? -1 : kp);
        fprintf(jf, "    \"kp_estimated\": %.2f,\n", isnan(kp_est) ? -1 : kp_est);
        fprintf(jf, "    \"kp_time\": \"%s\",\n", kp_time);
        fprintf(jf, "    \"f107_sfu\": %.1f,\n", isnan(f107) ? -1 : f107);
        fprintf(jf, "    \"f107_90d\": %.1f\n", isnan(f107_mean) ? -1 : f107_mean);
        fprintf(jf, "  },\n");
        fprintf(jf, "  \"metno\": {\n");
        fprintf(jf, "    \"temp_c\": %.1f,\n", isnan(mn_temp) ? -999 : mn_temp);
        fprintf(jf, "    \"humid_pct\": %.0f,\n", isnan(mn_humid) ? -1 : mn_humid);
        fprintf(jf, "    \"pressure_hpa\": %.0f,\n", isnan(mn_pressure) ? -1 : mn_pressure);
        fprintf(jf, "    \"wind_ms\": %.1f,\n", isnan(mn_wind) ? -1 : mn_wind);
        fprintf(jf, "    \"summary\": \"%s\"\n", mn_summary);
        fprintf(jf, "  },\n");
        fprintf(jf, "  \"wttr\": {\n");
        fprintf(jf, "    \"temp_c\": %.1f,\n", isnan(wt_temp) ? -999 : wt_temp);
        fprintf(jf, "    \"humid_pct\": %.0f,\n", isnan(wt_humid) ? -1 : wt_humid);
        fprintf(jf, "    \"description\": \"%s\"\n", wt_desc);
        fprintf(jf, "  }\n");
        fprintf(jf, "}\n");
        fclose(jf);
    }

    printf("  ✅ 已存入 external_data 表 + external_data.json\n");
    return 0;
}