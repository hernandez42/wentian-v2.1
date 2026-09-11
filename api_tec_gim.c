/* ============================================================
 * api_tec_gim.c - 真实TEC集成引擎 v1.0
 * ============================================================
 * 项目: 问天 v2.3 (WenTian Weather Station)
 * 所有者: 主人朱涛 BG8SBA, 昆明长水机场
 *
 * 功能: 从NOAA SWPC GloTEC实时模型获取真实TEC数据,
 *       对目标坐标做双线性插值, 失败时回退经验估算。
 *
 * API: https://services.swpc.noaa.gov/products/noaa-tech.json
 *   返回JSON数组: [时间, 纬度, 经度, TEC值, TEC误差]
 *
 * 双线性插值网格: 找到最邻近4个网格点后插值
 *
 * 数据源标记策略:
 *   - 成功获取NOAA真实数据: source="NOAA SWPC"
 *   - API不可用/解析失败: 诚实回退到经验估算, source="经验估算"
 * ============================================================ */
#include "wentian.h"
#include <sqlite3.h>
#include <math.h>
#include <time.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>

#define NOAA_TEC_URL   "https://services.swpc.noaa.gov/products/noaa-tech.json"
#define TEC_HTTP_TIMEOUT 30

/* ── 内部: JSON数组元素提取 (数组中的基本值, 非键值对) ── */

/* 跳过空白 */
static const char *skip_ws(const char *p) {
    while (p && *p && (unsigned char)*p <= 32) p++;
    return p;
}

/* 从当前位置读一个JSON值(数字、字符串或null), 返回strdup */
static char *json_elem_dup(const char **pp) {
    const char *p = skip_ws(*pp);
    if (!p || !*p) return NULL;

    char *out = NULL;

    if (*p == '"') {
        /* 字符串 */
        p++;
        const char *start = p;
        while (*p && *p != '"') {
            if (*p == '\\' && *(p+1)) p++;
            p++;
        }
        size_t len = p - start;
        out = malloc(len + 1);
        if (out) {
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
        }
        if (*p == '"') p++;
    } else if (*p == 'n' && strncmp(p, "null", 4) == 0) {
        out = strdup("null");
        p += 4;
    } else {
        /* 数字 */
        const char *start = p;
        if (*p == '-') p++;
        while (*p && (isdigit((unsigned char)*p) || *p == '.' || *p == 'e' || *p == 'E' || *p == '+' || *p == '-')) {
            if (*p == '+' || *p == '-') { if (p > start && *(p-1) != 'e' && *(p-1) != 'E') break; }
            p++;
        }
        size_t len = p - start;
        out = malloc(len + 1);
        if (out) {
            memcpy(out, start, len);
            out[len] = '\0';
        }
    }

    *pp = p;
    return out;
}

/* ── 解析 time_tag 字符串 → time_t ── */
/* 格式: "2024-01-15T12:00:00Z" 或 "2024-01-15 12:00:00" */
static time_t parse_iso_time(const char *s) {
    if (!s || strcmp(s, "null") == 0) return 0;
    struct tm tm;
    memset(&tm, 0, sizeof(tm));
    if (sscanf(s, "%d-%d-%dT%d:%d:%d",
               &tm.tm_year, &tm.tm_mon, &tm.tm_mday,
               &tm.tm_hour, &tm.tm_min, &tm.tm_sec) >= 5) {
        tm.tm_year -= 1900;
        tm.tm_mon -= 1;
        tm.tm_isdst = 0;
        return timegm(&tm);
    }
    if (sscanf(s, "%d-%d-%d %d:%d:%d",
               &tm.tm_year, &tm.tm_mon, &tm.tm_mday,
               &tm.tm_hour, &tm.tm_min, &tm.tm_sec) >= 5) {
        tm.tm_year -= 1900;
        tm.tm_mon -= 1;
        tm.tm_isdst = 0;
        return timegm(&tm);
    }
    return 0;
}

/* ── NOAA TEC JSON 条目结构体 ── */
typedef struct {
    double lat;
    double lon;
    double vtec;
    double error;
    time_t ts;
    int    valid;
} noaa_tec_point_t;

/* ── 解析NOAA TEC JSON响应 ── */
/* 格式: [[时间, 纬度, 经度, TEC值, TEC误差], ...] */
static int parse_noaa_tec_json(const char *json, noaa_tec_point_t *points, int max_points) {
    if (!json) return 0;

    const char *p = skip_ws(json);
    if (*p != '[') return 0;
    p++; /* 跳过开头的 [ */

    int count = 0;
    while (*p && count < max_points) {
        p = skip_ws(p);
        if (*p == ']') break; /* 空数组或结束 */
        if (*p != '[') { /* 跳过非数组元素(如逗号) */ p++; continue; }
        p++; /* 跳过 [ */

        /* 提取5个值: 时间, 纬度, 经度, TEC, 误差 */
        char *time_str = json_elem_dup(&p);
        if (!time_str) break;
        p = skip_ws(p); if (*p == ',') p++;

        char *lat_str = json_elem_dup(&p);
        if (!lat_str) { free(time_str); break; }
        p = skip_ws(p); if (*p == ',') p++;

        char *lon_str = json_elem_dup(&p);
        if (!lon_str) { free(time_str); free(lat_str); break; }
        p = skip_ws(p); if (*p == ',') p++;

        char *tec_str = json_elem_dup(&p);
        if (!tec_str) { free(time_str); free(lat_str); free(lon_str); break; }
        p = skip_ws(p); if (*p == ',') p++;

        char *err_str = json_elem_dup(&p);
        if (!err_str) { free(time_str); free(lat_str); free(lon_str); free(tec_str); break; }

        /* 跳过 ] 和可能的逗号 */
        p = skip_ws(p); if (*p == ']') p++;
        p = skip_ws(p); if (*p == ',') p++;

        /* 解析数值 */
        double lat = atof(lat_str);
        double lon = atof(lon_str);
        double vtec = atof(tec_str);
        double error = atof(err_str);

        /* 验证有效范围 */
        if (lat >= -90 && lat <= 90 && lon >= -180 && lon <= 180 && vtec >= 0 && vtec < 200) {
            points[count].lat   = lat;
            points[count].lon   = lon;
            points[count].vtec  = vtec;
            points[count].error = error;
            points[count].ts    = parse_iso_time(time_str);
            points[count].valid = 1;
            count++;
        }

        free(time_str);
        free(lat_str);
        free(lon_str);
        free(tec_str);
        free(err_str);
    }

    return count;
}

/* ── 双线性插值 ── */
/* 在 (lat, lon) 位置从4个网格点 Q11(lat1,lon1), Q12(lat1,lon2),
 * Q21(lat2,lon1), Q22(lat2,lon2) 插值 */
static double bilinear_interp(double lat, double lon,
                               double lat1, double lat2,
                               double lon1, double lon2,
                               double f11, double f12,
                               double f21, double f22) {
    double dlat = lat2 - lat1;
    double dlon = lon2 - lon1;
    if (dlat == 0 && dlon == 0) return f11;
    if (dlat == 0) {
        /* 仅经度方向线性插值 */
        return f11 + (lon - lon1) * (f12 - f11) / dlon;
    }
    if (dlon == 0) {
        /* 仅纬度方向线性插值 */
        return f11 + (lat - lat1) * (f21 - f11) / dlat;
    }
    /* 标准双线性插值 */
    double rlat = (lat - lat1) / dlat;  /* 0~1 归一化 */
    double rlon = (lon - lon1) / dlon;
    /* 先在经度方向插值 */
    double ftop    = f11 + rlon * (f12 - f11);    /* 上边 */
    double fbottom = f21 + rlon * (f22 - f21);    /* 下边 */
    /* 再在纬度方向插值 */
    return ftop + rlat * (fbottom - ftop);
}

/* ── 查找最近4个网格点并进行双线性插值 ── */
static int interpolate_at_point(noaa_tec_point_t *points, int n_points,
                                 double target_lat, double target_lon,
                                 double *out_vtec, time_t *out_ts) {
    if (n_points < 4) return -1;

    /* 收集所有唯一网格点 (lat, lon) 及其TEC值 */
    /* 先按与目标点的距离排序, 取最近的4个 */

    /* 策略: 找目标点周围的4个象限各一个点 */
    /* 或者更简单: 取最近点, 然后沿经纬度方向扩张 */

    /* 更稳健的方法: 直接用最近4个点做双线性插值 */
    /* 由于NOAA网格是规则的, 最近的4个点应构成四边形 */

    /* 找到最近的4个网格点: */
    /* 1. 找纬度最接近的两个值 */
    /* 2. 找经度最接近的两个值 */
    /* 3. 组合成4个网格点 */

    /* 收集所有唯一的纬度和经度 */
#define MAX_UNIQUE 200
    double lats[MAX_UNIQUE];
    double lons[MAX_UNIQUE];
    int n_lats = 0, n_lons = 0;

    for (int i = 0; i < n_points && n_lats < MAX_UNIQUE; i++) {
        int found = 0;
        for (int j = 0; j < n_lats; j++) {
            if (fabs(lats[j] - points[i].lat) < 0.01) { found = 1; break; }
        }
        if (!found) lats[n_lats++] = points[i].lat;
    }
    for (int i = 0; i < n_points && n_lons < MAX_UNIQUE; i++) {
        int found = 0;
        for (int j = 0; j < n_lons; j++) {
            if (fabs(lons[j] - points[i].lon) < 0.01) { found = 1; break; }
        }
        if (!found) lons[n_lons++] = points[i].lon;
    }

    /* 找目标经纬度在两个方向上的紧邻值 */
    /* 纬度: 找到 lat1 ≤ target ≤ lat2 */
    double lat1 = -1e9, lat2 = 1e9;
    for (int i = 0; i < n_lats; i++) {
        if (lats[i] <= target_lat && lats[i] > lat1) lat1 = lats[i];
        if (lats[i] >= target_lat && lats[i] < lat2) lat2 = lats[i];
    }
    /* 经度: 找到 lon1 ≤ target ≤ lon2 */
    double lon1 = -1e9, lon2 = 1e9;
    for (int i = 0; i < n_lons; i++) {
        if (lons[i] <= target_lon && lons[i] > lon1) lon1 = lons[i];
        if (lons[i] >= target_lon && lons[i] < lon2) lon2 = lons[i];
    }

    /* 如果目标点恰好落在网格线上, 拓展搜索 */
    if (lat1 < -1e8 || lat2 > 1e8 || lon1 < -1e8 || lon2 > 1e8) {
        /* 用最近点代替 */
        int nearest = 0;
        double min_dist = 1e9;
        for (int i = 0; i < n_points; i++) {
            double dlat = points[i].lat - target_lat;
            double dlon = points[i].lon - target_lon;
            double dist = dlat * dlat + dlon * dlon;
            if (dist < min_dist) {
                min_dist = dist;
                nearest = i;
            }
        }
        *out_vtec = points[nearest].vtec;
        *out_ts = points[nearest].ts;
        return 0;
    }

    /* 从points中找到4个网格点的TEC值 */
    double f11 = NAN, f12 = NAN, f21 = NAN, f22 = NAN;
    time_t ts_min = 0;
    int ts_set = 0;

    for (int i = 0; i < n_points; i++) {
        int match_lat = 0, match_lon = 0;
        if (fabs(points[i].lat - lat1) < 0.01) match_lat = 1;
        else if (fabs(points[i].lat - lat2) < 0.01) match_lat = 2;

        if (fabs(points[i].lon - lon1) < 0.01) match_lon = 1;
        else if (fabs(points[i].lon - lon2) < 0.01) match_lon = 2;

        if (match_lat == 1 && match_lon == 1) { f11 = points[i].vtec; if (!ts_set || points[i].ts < ts_min) { ts_min = points[i].ts; ts_set = 1; } }
        if (match_lat == 1 && match_lon == 2) { f12 = points[i].vtec; if (!ts_set || points[i].ts < ts_min) { ts_min = points[i].ts; ts_set = 1; } }
        if (match_lat == 2 && match_lon == 1) { f21 = points[i].vtec; if (!ts_set || points[i].ts < ts_min) { ts_min = points[i].ts; ts_set = 1; } }
        if (match_lat == 2 && match_lon == 2) { f22 = points[i].vtec; if (!ts_set || points[i].ts < ts_min) { ts_min = points[i].ts; ts_set = 1; } }
    }

    /* 检查是否4个顶点都有值 */
    if (isnan(f11) || isnan(f12) || isnan(f21) || isnan(f22)) {
        /* 不足4个, 用已有值的平均 */
        double sum = 0;
        int n = 0;
        if (!isnan(f11)) { sum += f11; n++; }
        if (!isnan(f12)) { sum += f12; n++; }
        if (!isnan(f21)) { sum += f21; n++; }
        if (!isnan(f22)) { sum += f22; n++; }
        if (n > 0) {
            *out_vtec = sum / n;
            *out_ts = ts_min;
            return 0;
        }
        return -1;
    }

    *out_vtec = bilinear_interp(target_lat, target_lon,
                                 lat1, lat2, lon1, lon2,
                                 f11, f12, f21, f22);
    *out_ts = ts_min;
    return 0;
}

#undef MAX_UNIQUE

/* ── tec_gim表初始化 ── */
static int tec_gim_db_init(void) {
    sqlite3 *db;
    if (sqlite3_open(WENTIAN_DB, &db) != SQLITE_OK) {
        fprintf(stderr, "[TEC_GIM] 无法打开数据库 %s\n", WENTIAN_DB);
        return -1;
    }
    char *err = NULL;
    const char *sql = "CREATE TABLE IF NOT EXISTS tec_gim ("
                      "ts INTEGER, vtec REAL, lat REAL, lon REAL, source TEXT)";
    int rc = sqlite3_exec(db, sql, NULL, NULL, &err);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "[TEC_GIM] 创建表失败: %s\n", err);
        sqlite3_free(err);
        sqlite3_close(db);
        return -1;
    }
    sqlite3_close(db);
    return 0;
}

/* ── 保存到 tec_gim 表 ── */
static int tec_gim_save(time_t ts, double vtec, double lat, double lon, const char *source) {
    sqlite3 *db;
    if (sqlite3_open(WENTIAN_DB, &db) != SQLITE_OK) {
        fprintf(stderr, "[TEC_GIM] 打开数据库失败\n");
        return -1;
    }
    sqlite3_stmt *st;
    const char *sql = "INSERT INTO tec_gim VALUES (?,?,?,?,?)";
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) {
        fprintf(stderr, "[TEC_GIM] prepare失败: %s\n", sqlite3_errmsg(db));
        sqlite3_close(db);
        return -1;
    }
    sqlite3_bind_int64(st, 1, (sqlite3_int64)ts);
    sqlite3_bind_double(st, 2, vtec);
    sqlite3_bind_double(st, 3, lat);
    sqlite3_bind_double(st, 4, lon);
    sqlite3_bind_text(st, 5, source, -1, SQLITE_TRANSIENT);

    int rc = sqlite3_step(st);
    if (rc != SQLITE_DONE) {
        fprintf(stderr, "[TEC_GIM] insert rc=%d: %s\n", rc, sqlite3_errmsg(db));
        sqlite3_finalize(st);
        sqlite3_close(db);
        return -1;
    }
    sqlite3_finalize(st);
    sqlite3_close(db);
    return 0;
}

/* ═══════════════════════════════════════════════════════════
 * 公开接口
 * ═══════════════════════════════════════════════════════════ */

/* ── wt_tec_gim_fetch: 从NOAA获取真实TEC ── */
int wt_tec_gim_fetch(wt_tec_gim_t *gim, double lat, double lon) {
    if (!gim) return -1;

    /* 初始化 */
    memset(gim, 0, sizeof(*gim));
    gim->lat = lat;
    gim->lon = lon;
    gim->ts = time(NULL);
    gim->valid = 0;
    strcpy(gim->source, "经验估算");
    gim->vtec = 0.0;

    /* 确保表存在 */
    tec_gim_db_init();

    /* 1. HTTP获取JSON */
    printf("\n━━━ TEC GIM: 从NOAA获取真实TEC数据 ━━━\n");
    printf("  目标坐标: %.4f°N, %.4f°E\n", lat, lon);

    char *body = wt_http_get(NOAA_TEC_URL, TEC_HTTP_TIMEOUT);
    if (!body) {
        printf("  ❌ NOAA TEC API 不可用(HTTP失败), 标记无效\n");
        printf("  数据源: 经验估算(回退)\n");
        tec_gim_save(gim->ts, 0.0, lat, lon, "API_FAILED");
        return -1;
    }

    /* 检查返回内容是否有效JSON数组 */
    const char *p = skip_ws(body);
    if (*p != '[') {
        printf("  ❌ NOAA TEC API 返回非JSON格式\n");
        printf("  数据源: 经验估算(回退)\n");
        free(body);
        tec_gim_save(gim->ts, 0.0, lat, lon, "API_FAILED");
        return -1;
    }

    /* 2. 解析JSON */
    #define MAX_TEC_POINTS 5000
    noaa_tec_point_t *points = malloc(sizeof(noaa_tec_point_t) * MAX_TEC_POINTS);
    if (!points) {
        printf("  ❌ 内存分配失败\n");
        free(body);
        return -1;
    }

    int n_points = parse_noaa_tec_json(body, points, MAX_TEC_POINTS);
    printf("  解析到 %d 个TEC网格点\n", n_points);

    if (n_points < 4) {
        printf("  ❌ 网格点不足 (%d < 4), 无法插值\n", n_points);
        free(points);
        free(body);
        tec_gim_save(gim->ts, 0.0, lat, lon, "INSUFFICIENT_DATA");
        return -1;
    }

    /* 3. 双线性插值 */
    double vtec_interp;
    time_t ts_gim;
    int ret = interpolate_at_point(points, n_points, lat, lon,
                                    &vtec_interp, &ts_gim);

    if (ret == 0 && vtec_interp >= 0 && vtec_interp < 200) {
        gim->valid = 1;
        gim->vtec = vtec_interp;
        gim->ts = (ts_gim > 0) ? ts_gim : time(NULL);
        strcpy(gim->source, "NOAA SWPC");
        printf("  ✅ GIM TEC插值成功: %.2f TECU (源: %s)\n", gim->vtec, gim->source);
        tec_gim_save(gim->ts, gim->vtec, lat, lon, gim->source);
    } else {
        printf("  ❌ 插值失败, 标记无效\n");
        tec_gim_save(gim->ts, 0.0, lat, lon, "INTERP_FAILED");
    }

    free(points);
    free(body);
    return ret;
}

#undef MAX_TEC_POINTS

/* ── wt_tec_hybrid_run: 混合TEC获取 ──
 * 先尝试NOAA真实TEC → 失败时回退经验估算
 * 返回TEC值, 通过输出参数返回数据源 */
int wt_tec_hybrid_run(wt_tec_gim_t *gim) {
    if (!gim) return -1;

    printf("\n╔══════════════════════════════════════════════════════╗\n");
    printf("║       TEC 混合引擎: 先真实 → 再经验回退           ║\n");
    printf("╚══════════════════════════════════════════════════════╝\n");

    /* 步骤1: 尝试真实GIM TEC */
    int ret = wt_tec_gim_fetch(gim, WENTIAN_LAT, WENTIAN_LON);

    if (ret == 0 && gim->valid) {
        printf("  使用数据源: %s, TEC=%.2f TECU\n", gim->source, gim->vtec);
        return 0;
    }

    /* 步骤2: NOAA不可用, 回退经验估算 */
    printf("\n  ── NOAA TEC不可用, 回退经验估算 ──\n");

    /* 调用现有的经验估算引擎 */
    extern int wt_tec_run(void);
    wt_tec_run();

    /* 从DB读取最新的经验TEC */
    sqlite3 *db;
    double tec_est = 0;
    int found = 0;
    if (sqlite3_open(WENTIAN_DB, &db) == SQLITE_OK) {
        sqlite3_stmt *st;
        const char *sql = "SELECT tec_fused_est FROM external_data ORDER BY ts DESC LIMIT 1";
        if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) == SQLITE_OK) {
            if (sqlite3_step(st) == SQLITE_ROW) {
                tec_est = sqlite3_column_double(st, 0);
                found = 1;
            }
            sqlite3_finalize(st);
        }
        sqlite3_close(db);
    }

    if (!found) {
        /* 如果external_data也没有, 用Kp估算作为最后堡垒 */
        double kp = wt_db_read_kp();
        if (!isnan(kp)) {
            tec_est = (kp + 1.0) * 5.0;
            found = 1;
        }
    }

    gim->valid = 1;
    gim->vtec = tec_est;
    gim->ts = time(NULL);
    strcpy(gim->source, "经验估算");

    printf("  经验回退 TEC: %.2f TECU (源: %s)\n", gim->vtec, gim->source);
    tec_gim_save(gim->ts, gim->vtec, WENTIAN_LAT, WENTIAN_LON, gim->source);

    return 0;
}
