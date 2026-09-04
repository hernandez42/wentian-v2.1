/* ============================================================
 * api_pwv.c - GNSS PWV实时反演引擎 v1.0 (C实现)
 * ============================================================
 * 项目: 问天 v1.1 (WenTian Weather Station)
 * 所有者: 主人朱涛 BG8SBA, 昆明长水机场楼顶
 * 锁定: 详见 WENTIAN-LOCK.md, 非授权LLM请勿擅改
 *
 * 功能: 把 gps_uno_fusion.py 的Saastamoinen PWV反演用C重写,
 *       嵌入wentian daemon, 从离线Python变实时C.
 *
 * 物理模型:
 *   Saastamoinen(1973)对流层延迟模型:
 *     ZTD = (0.002277/cos(z)) * [P + (1255/T+0.05)*e - tan²(z)*1.16]
 *     ZHD ≈ 0.002277 * P / cos(z)  (干延迟)
 *     ZWD = ZTD - ZHD              (湿延迟)
 *     PWV = ZWD * 1000 / 6.5       (Bevis常数, mm)
 *
 *   水汽压(Magnus公式):
 *     e = (rh/100) * 6.105 * exp(17.27*T/(T+237.7))
 *
 * 数据源:
 *   - UNO: 温度/湿度/海平面气压 (SQLite ano_weather表)
 *   - GNSS: 定位坐标/海拔 (SQLite gps_log表)
 *   - 输出: pwv_history.csv + wentian.db local_pwv表
 *
 * 与Python版对比:
 *   Python: 离线批处理, 每次重算全部历史, 慢
 *   C: 实时增量反演, 每次只算最新点, 快100倍+
 *
 * 编译: 纳入 wentian 主编译
 * ============================================================ */
#include "wentian.h"
#include <sqlite3.h>
#include <math.h>
#include <unistd.h>

#define PWV_HISTORY_CSV    "/root/data/fusion/pwv_history.csv"
#define PWV_MAX_ROWS       1440   /* 24小时*60min, 环形缓冲 */
#define PWV_BEVIS_CONST    6.5    /* Bevis转换常数(无量纲) */
#define PWV_SAA_CONST      0.002277 /* Saastamoinen常数 */
#define PWV_MAGNUS_A       6.105
#define PWV_MAGNUS_B       17.27
#define PWV_MAGNUS_C       237.7
#define PWV_PRESSURE_GRAD  0.12   /* hPa/m, 气压垂直递减率 */

/* ── PWV反演结果 ────────────────────────────────────────── */
/* wt_pwv_t 已在 wentian.h 中定义 */

/* ── Saastamoinen模型 ──────────────────────────────────── */
/* 输入: 测站气压(hPa), 温度(°C), 湿度(%), 天顶角(度)
 * 输出: ZTD/ZHD/ZWD(m)
 * 参考: Saastamoinen J. (1973) "Contributions to the theory of
 *       mapping the atmosphere" */
static void saastamoinen(double P_hPa, double T_c, double rh,
                         double zenith_deg,
                         double *ztd, double *zhd, double *zwd) {
    double T_k = T_c + 273.15;
    /* Magnus公式: 饱和水汽压(hPa) */
    double e_sat = PWV_MAGNUS_A * exp(PWV_MAGNUS_B * T_c / (T_c + PWV_MAGNUS_C));
    /* 实际水汽压 */
    double e = (rh / 100.0) * e_sat;
    /* 天顶角余弦 */
    double cos_z = cos(zenith_deg * M_PI / 180.0);
    if (cos_z < 0.1) cos_z = 0.1;  /* 避免除零(天顶角>84°不可靠) */
    double tan_z = tan(zenith_deg * M_PI / 180.0);

    /* 总天顶延迟(m) */
    double ztd_val = (PWV_SAA_CONST / cos_z) *
        (P_hPa + (1255.0 / T_k + 0.05) * e - tan_z * tan_z * 1.16);
    /* 干延迟 */
    double zhd_val = PWV_SAA_CONST * P_hPa / cos_z;
    /* 湿延迟 */
    double zwd_val = ztd_val - zhd_val;

    *ztd = ztd_val;
    *zhd = zhd_val;
    *zwd = zwd_val;
}

/* ── PWV反演 ───────────────────────────────────────────── */
static double pwv_from_zwd(double zwd_m) {
    return zwd_m * 1000.0 / PWV_BEVIS_CONST;
}

/* ── 暴风雨指标 ────────────────────────────────────────── */
static int storm_score(double pwv, double pwv_delta, double rh, double press) {
    int score = 0;
    if (pwv_delta > 1.0) score += 30;   /* PWV急升 */
    else if (pwv_delta > 0.5) score += 15;
    if (rh > 85) score += 30;            /* 高湿 */
    else if (rh > 75) score += 15;
    if (press < 1008) score += 20;       /* 低气压 */
    if (pwv > 45) score += 20;           /* 高PWV */
    return (score > 100) ? 100 : score;
}

/* ── 从UNO取最新数据 ───────────────────────────────────── */
static int load_uno_latest(double *t, double *rh, double *p_sea, time_t *ts) {
    sqlite3 *db;
    if (sqlite3_open("/root/data/ano_weather.db", &db) != SQLITE_OK) return -1;

    sqlite3_stmt *st;
    int rc = sqlite3_prepare_v2(db,
        "SELECT ts,t,h,p FROM ano_weather "
        "WHERE source='UNO_v2.0_bridge' ORDER BY ts DESC LIMIT 1", -1, &st, NULL);
    if (rc != SQLITE_OK) { sqlite3_close(db); return -1; }
    if (sqlite3_step(st) != SQLITE_ROW) { sqlite3_finalize(st); sqlite3_close(db); return -1; }

    /* ts是字符串格式 */
    const char *ts_str = (const char *)sqlite3_column_text(st, 0);
    *ts = time(NULL);
    if (ts_str) {
        struct tm tm = {0};
        if (sscanf(ts_str, "%d-%d-%dT%d:%d:%d",
            &tm.tm_year, &tm.tm_mon, &tm.tm_mday,
            &tm.tm_hour, &tm.tm_min, &tm.tm_sec) == 6) {
            tm.tm_year -= 1900;
            tm.tm_mon -= 1;
            tm.tm_isdst = -1;
            *ts = mktime(&tm);
        }
    }
    *t = sqlite3_column_double(st, 1);
    *rh = sqlite3_column_double(st, 2);
    *p_sea = sqlite3_column_double(st, 3);
    sqlite3_finalize(st);
    sqlite3_close(db);
    return 0;
}

/* ── 从GNSS取最新坐标 ──────────────────────────────────── */
static int load_gnss_pos(double *lat, double *lon, double *alt) {
    *lat = 25.0808;
    *lon = 102.9129;
    *alt = 2103.0;  /* 默认初始化, 防止未使用警告 */

    sqlite3 *db;
    if (sqlite3_open("/root/data/ano_weather.db", &db) != SQLITE_OK) return -1;

    sqlite3_stmt *st;
    int rc = sqlite3_prepare_v2(db,
        "SELECT lat,lon,alt FROM gps_log "
        "WHERE fix IN ('3D','2D','1D') ORDER BY ts DESC LIMIT 1", -1, &st, NULL);
    if (rc != SQLITE_OK) { sqlite3_close(db); return -1; }
    if (sqlite3_step(st) == SQLITE_ROW) {
        *lat = sqlite3_column_double(st, 0);
        *lon = sqlite3_column_double(st, 1);
        *alt = sqlite3_column_double(st, 2);
        sqlite3_finalize(st);
        sqlite3_close(db);
        return 0;
    }
    sqlite3_finalize(st);
    sqlite3_close(db);
    /* 回退: 主人家固定坐标 */
    *lat = 25.0808;
    *lon = 102.9129;
    *alt = 2103.0;
    return 0;
}

/* ── 单次PWV反演 ───────────────────────────────────────── */
int wt_pwv_compute(wt_pwv_t *out, double last_pwv) {
    memset(out, 0, sizeof(*out));
    out->ts = time(NULL);

    double T_c, rh, p_sea, lat, lon, alt;
    time_t ts;

    /* 1. 取UNO数据 */
    if (load_uno_latest(&T_c, &rh, &p_sea, &ts) != 0) return -1;
    out->ts = ts;
    out->temp_c = T_c;
    out->humid_pct = rh;

    /* 2. 取GNSS坐标 */
    load_gnss_pos(&lat, &lon, &alt);

    /* 3. 海平面气压 → 测站气压(用GPS海拔) */
    double p_station = p_sea - (alt * PWV_PRESSURE_GRAD);
    if (p_station < 600 || p_station > 1100) p_station = p_sea;
    out->press_hpa = p_station;

    /* 4. Saastamoinen模型(天顶角60°=典型值) */
    saastamoinen(p_station, T_c, rh, 60.0, &out->ztd_m, &out->zhd_m, &out->zwd_m);

    /* 5. PWV反演 */
    out->pwv_mm = pwv_from_zwd(out->zwd_m);
    out->delta_pwv = out->pwv_mm - last_pwv;

    /* 6. 暴风雨指标 */
    out->storm_score = storm_score(out->pwv_mm, out->delta_pwv, rh, p_sea);

    return 0;
}

/* ── 保存到DB ──────────────────────────────────────────── */
int wt_db_save_pwv(const wt_pwv_t *p) {
    sqlite3 *db;
    if (sqlite3_open(WENTIAN_DB, &db) != SQLITE_OK) return -1;

    char sql[512];
    snprintf(sql, sizeof(sql),
        "INSERT OR REPLACE INTO local_pwv "
        "(ts,pwv_mm,ztd_m,zhd_m,zwd_m,delta_pwv,temp_c,humid_pct,press_hpa,storm_score) "
        "VALUES (%ld,%.4f,%.6f,%.6f,%.6f,%.4f,%.2f,%.2f,%.2f,%d)",
        (long)p->ts, p->pwv_mm, p->ztd_m, p->zhd_m, p->zwd_m,
        p->delta_pwv, p->temp_c, p->humid_pct, p->press_hpa, p->storm_score);

    int rc = sqlite3_exec(db, sql, NULL, NULL, NULL);
    sqlite3_close(db);
    return (rc == SQLITE_OK) ? 0 : -1;
}

/* ── 写CSV历史 ─────────────────────────────────────────── */
/* 增量追加: 用unix时间戳格式, 与旧Python版(秒-日)区分 */
static int pwv_csv_append(const wt_pwv_t *p) {
    int file_exists = access(PWV_HISTORY_CSV, F_OK) == 0;
    FILE *f = fopen(PWV_HISTORY_CSV, file_exists ? "a" : "w");
    if (!f) return -1;

    if (!file_exists) {
        fprintf(f, "ts,T_c,rh,p_sea,p_station,ztd_m,zhd_m,zwd_m,pwv_mm,pwv_delta,storm_score\n");
    }

    /* 海平面气压(近似) */
    double p_sea = p->press_hpa + (2103.0 * PWV_PRESSURE_GRAD);
    /* 用unix时间戳(整数), 这样load_pwv_recent能自动识别为新格式 */
    fprintf(f, "%ld,%.2f,%.1f,%.1f,%.2f,%.6f,%.6f,%.6f,%.4f,%.2f,%d\n",
        (long)p->ts, p->temp_c, p->humid_pct, p_sea, p->press_hpa,
        p->ztd_m, p->zhd_m, p->zwd_m, p->pwv_mm, p->delta_pwv, p->storm_score);

    fclose(f);
    return 0;
}

/* ── 取最新PWV值(用于计算delta) ───────────────────────── */
static double pwv_get_last(void) {
    sqlite3 *db;
    if (sqlite3_open(WENTIAN_DB, &db) != SQLITE_OK) return 0.0;

    sqlite3_stmt *st;
    int rc = sqlite3_prepare_v2(db,
        "SELECT pwv_mm FROM local_pwv ORDER BY ts DESC LIMIT 1", -1, &st, NULL);
    double last = 0.0;
    if (rc == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW) {
        last = sqlite3_column_double(st, 0);
    }
    sqlite3_finalize(st);
    sqlite3_close(db);
    return last;
}

/* ── Daemon入口 ─────────────────────────────────────────── */
int wt_pwv_run(void) {
    double last_pwv = pwv_get_last();
    wt_pwv_t p;

    if (wt_pwv_compute(&p, last_pwv) != 0) {
        printf("  ❌ PWV反演失败\n");
        return -1;
    }

    /* 保存到DB */
    wt_db_save_pwv(&p);

    /* 追加CSV */
    pwv_csv_append(&p);

    /* 打印 */
    const char *level = p.storm_score >= 60 ? "暴雨" :
                        p.storm_score >= 30 ? "注意" : "正常";
    const char *icon = p.storm_score >= 60 ? "⚠" :
                       p.storm_score >= 30 ? "⚠" : "✓";
    printf("  %s PWV=%.2fmm(Δ%.2fmm) ZWD=%.2fmm 温=%.1fC 湿=%.0f%% 气压=%.1f hPa 指标:%d(%s)\n",
        icon, p.pwv_mm, p.delta_pwv, p.zwd_m*1000, p.temp_c, p.humid_pct,
        p.press_hpa, p.storm_score, level);

    return 0;
}

/* ── DB表初始化 ─────────────────────────────────────────── */
int wt_pwv_db_init(const char *path) {
    sqlite3 *db;
    if (sqlite3_open(path, &db) != SQLITE_OK) return -1;

    const char *sql =
        "CREATE TABLE IF NOT EXISTS local_pwv ("
        "ts INTEGER PRIMARY KEY, "
        "pwv_mm REAL, "
        "ztd_m REAL, "
        "zhd_m REAL, "
        "zwd_m REAL, "
        "delta_pwv REAL, "
        "temp_c REAL, "
        "humid_pct REAL, "
        "press_hpa REAL, "
        "storm_score INTEGER);";
    int rc = sqlite3_exec(db, sql, NULL, NULL, NULL);
    sqlite3_close(db);
    return (rc == SQLITE_OK) ? 0 : -1;
}