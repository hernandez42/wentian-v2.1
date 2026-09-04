/* ============================================================
 * api_gnss_ion.c - GNSS电离层闪烁指数实时计算 v1.0 (C实现)
 * ============================================================
 * 项目: 问天 v2.1 (WenTian Weather Station)
 * 所有者: 主人朱涛 BG8SBA, 昆明长水机场楼顶
 *
 * 功能: 从GNSS SNR数据计算S4闪烁指数 + Klobuchar电离层延迟模型
 *       替代原Python版 gnss_ionosphere.py, 嵌入daemon 60秒周期
 *
 * S4 = stddev(SNR) / mean(SNR)  (1分钟窗口)
 *   S4 < 0.05: 平静
 *   0.05-0.15: 弱扰动
 *   0.15-0.30: 中等闪烁
 *   >0.30: 强闪烁(雷暴关联前兆)
 *
 * Klobuchar模型: 单频GPS用户标准电离层延迟修正
 * ============================================================
 */
#include "wentian.h"
#include <sqlite3.h>
#include <math.h>
#include <time.h>
#include <unistd.h>

#define OWNER_DB    "/root/data/ano_weather.db"

/* ── 字符串化宏 ────────────────────────────────────────── */
#define XSTRING(s)  XSTRING_(s)
#define XSTRING_(s) #s

/* ── Klobuchar模型参数(广播星历典型值) ─────────────────── */
static const double KLOB_ALPHA[4] = {1.1e-8, -7.6e-9, -5.6e-7, 5.7e-8};
static const double KLOB_BETA[4]  = {91136.0, 65536.0, -393216.0, 393216.0};

/* ── 电离层活动评估 ────────────────────────────────────── */
static const char *ion_activity_from_s4(double s4_max) {
    if (s4_max < 0.05) return "QUIET";
    if (s4_max < 0.15) return "WEAK";
    if (s4_max < 0.30) return "MODERATE";
    return "STRONG";
}

/* ── S4指数计算 ────────────────────────────────────────── */
static double calc_s4(const double *vals, int n) {
    if (n < 5) return -1.0;
    double mean = 0.0;
    for (int i = 0; i < n; i++) mean += vals[i];
    mean /= n;
    if (mean <= 0) return -1.0;
    double var = 0.0;
    for (int i = 0; i < n; i++) {
        double d = vals[i] - mean;
        var += d * d;
    }
    double sd = sqrt(var / (n > 1 ? n - 1 : 1));
    return sd / mean;
}

/* ── 从DB加载最近N分钟GNSS SNR ─────────────────────────── */
/* 直接从主人原始DB(gps_log表)读取, 避免依赖local_gnss表的同步延迟
 * 返回: >0=实际加载的GPS行数, 0=无数据 */
static int load_gnss_snr(double *gps_snrs, int gps_max,
                         double *bds_snrs, int bds_max,
                         double *pdops, int pdop_max,
                         int lookback_min) {
    sqlite3 *db;
    /* 从原始gps_log表读SNR, 该表由api_local.c持续写入
     * gps_log的ts是ISO格式(如2026-09-04T09:22:08), 用strftime统一比较 */
    if (sqlite3_open(OWNER_DB, &db) != SQLITE_OK) return 0;

    sqlite3_stmt *st;
    /* 用strftime('%Y-%m-%dT%H:%M', ts)提取分钟精度, 与datetime('now')比较 */
    int rc = sqlite3_prepare_v2(db,
        "SELECT gps_snr_avg,bds_snr_avg,pdop FROM gps_log "
        "WHERE strftime('%s', ts) >= strftime('-%d seconds', 'now') "
        "AND gps_snr_avg > 0 ORDER BY ts DESC LIMIT 64",
        -1, &st, NULL);
    if (rc != SQLITE_OK) { fprintf(stderr, "SQL prepare fail rc=%d\n", rc); sqlite3_close(db); return 0; }

    int ng = 0, nb = 0, np = 0;
    int step_rc;
    while ((step_rc = sqlite3_step(st)) == SQLITE_ROW) {
        if (ng < gps_max) gps_snrs[ng++] = sqlite3_column_double(st, 0);
        if (nb < bds_max) bds_snrs[nb++] = sqlite3_column_double(st, 1);
        if (np < pdop_max) pdops[np++] = sqlite3_column_double(st, 2);
    }
    fprintf(stderr, "step final rc=%d ng=%d\n", step_rc, ng);
    sqlite3_finalize(st);
    sqlite3_close(db);
    return ng;
}

/* ── Klobuchar电离层延迟模型 ────────────────────────────── */
/* 输入: 纬度(度), 经度(度), 海拔(m), UTC秒数(当天)
 * 输出: 垂直延迟(s), 斜向延迟(s), 斜向因子, 周期(s)
 */
static void klobuchar_model(double lat_deg, double lon_deg, double alt_m,
                            double utc_sec,
                            double *vert_delay, double *slant_delay,
                            double *slant_factor, double *period_s) {
    double lat = lat_deg / 180.0;  /* 半圆 */
    double lon = lon_deg / 180.0;
    double alt_km = alt_m / 1000.0;

    /* 地心角(秒) */
    double psi = 0.0137 / (alt_km + 0.11) - 0.022;

    /* 测站地心纬度(半圆) */
    double phi_i = lat + psi * cos(M_PI * lat);
    if (phi_i > 0.416) phi_i = 0.416;
    if (phi_i < -0.416) phi_i = -0.416;

    /* 地方时(秒) */
    double t = 4.32e4 * lon + utc_sec;
    while (t < 0) t += 86400;
    while (t >= 86400) t -= 86400;

    /* 倾斜因子 */
    double f = 1.0 + 16.0 * pow(0.53 - alt_km / 57.3, 3);

    /* 周期(秒) */
    double p = KLOB_BETA[0] + KLOB_BETA[1] * (phi_i / M_PI)
             + KLOB_BETA[2] * (phi_i / M_PI) * (phi_i / M_PI)
             + KLOB_BETA[3] * (phi_i / M_PI) * (phi_i / M_PI) * (phi_i / M_PI);
    if (p < 72000) p = 72000;

    /* 相位(秒) */
    double x = 2 * M_PI * (t - 50400) / p;

    /* 振幅(秒) */
    double amp;
    if (fabs(x) > M_PI / 2) {
        amp = f * 5e-9;
    } else {
        amp = f * (KLOB_ALPHA[0] + KLOB_ALPHA[1] * (phi_i / M_PI)
                   + KLOB_ALPHA[2] * (phi_i / M_PI) * (phi_i / M_PI)
                   + KLOB_ALPHA[3] * (phi_i / M_PI) * (phi_i / M_PI) * (phi_i / M_PI));
        if (amp < 0) amp = 0;
    }

    /* 垂直延迟(秒) */
    double tv;
    if (fabs(x) > M_PI / 2) {
        tv = f * 5e-9;
    } else {
        double x2 = x * x;
        tv = amp * (1.0 - x2 / 2.0 + x2 * x2 / 24.0);
    }

    *vert_delay = tv;
    *slant_delay = f * tv;
    *slant_factor = f;
    *period_s = p;
}

/* ── 电离层反演主函数 ──────────────────────────────────── */
int wt_gnss_ionosphere_revert(wt_gnss_ion_t *out, time_t ts) {
    if (!out) return -1;
    memset(out, 0, sizeof(wt_gnss_ion_t));
    out->ts = ts;

    /* 位置: 昆明长水 */
    const double LAT = 25.0808, LON = 102.9129, ALT = 2103.0;

    /* 加载SNR数据 */
    double gps_snrs[64] = {0}, bds_snrs[64] = {0}, pdops[32] = {0};
    int ng = load_gnss_snr(gps_snrs, 64, bds_snrs, 64, pdops, 32, 10);
    if (ng < 5) {
        /* SNR数据不足5个样本, 无法计算S4 */
        out->s4_gps = -1.0;
        out->s4_bds = -1.0;
        return -1;
    }
    /* 计算S4 — 用实际行数 */
    out->s4_gps = calc_s4(gps_snrs, ng);
    out->s4_bds = calc_s4(bds_snrs, ng);

    /* 平均SNR */
    double gps_sum = 0, bds_sum = 0;
    for (int i = 0; i < ng; i++) {
        if (gps_snrs[i] > 0) gps_sum += gps_snrs[i];
        if (bds_snrs[i] > 0) bds_sum += bds_snrs[i];
    }
    out->avg_gps_snr = gps_sum / ng;
    out->avg_bds_snr = bds_sum / ng;

    /* 平均PDOP */
    double pdop_sum = 0;
    int np = 0;
    for (int i = 0; i < 32 && pdops[i] > 0 && pdops[i] < 10; i++) {
        pdop_sum += pdops[i]; np++;
    }
    out->avg_pdop = np > 0 ? pdop_sum / np : 0;

    /* Klobuchar模型 */
    struct tm tm_utc;
    gmtime_r(&ts, &tm_utc);
    double utc_sec = tm_utc.tm_hour * 3600 + tm_utc.tm_min * 60 + tm_utc.tm_sec;
    klobuchar_model(LAT, LON, ALT, utc_sec,
                    &out->klob_vert_delay, &out->klob_slant_delay,
                    &out->klob_slant_factor, &out->klob_period);

    /* 电离层活动 */
    double s4_max = (out->s4_gps > out->s4_bds) ? out->s4_gps : out->s4_bds;
    out->activity = ion_activity_from_s4(s4_max >= 0 ? s4_max : 0);

    return 0;
}

/* ── 保存电离层数据到DB ────────────────────────────────── */
static int ion_db_save(const wt_gnss_ion_t *p) {
    sqlite3 *db;
    if (sqlite3_open(WENTIAN_DB, &db) != SQLITE_OK) return -1;

    const char *sql = "INSERT OR REPLACE INTO local_ionosphere "
        "(ts,s4_gps,s4_bds,samples_gps,samples_bds,"
        "gps_snr_avg,bds_snr_avg,pdop_avg,"
        "klob_vert_delay,klob_slant_delay,klob_slant_factor,klob_period,"
        "activity) VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?)";
    sqlite3_stmt *st;
    int rc = sqlite3_prepare_v2(db, sql, -1, &st, NULL);
    if (rc != SQLITE_OK) { sqlite3_errmsg(db); sqlite3_close(db); return -1; }

    sqlite3_bind_int64(st, 1, (sqlite3_int64)p->ts);
    sqlite3_bind_double(st, 2, p->s4_gps);
    sqlite3_bind_double(st, 3, p->s4_bds);
    sqlite3_bind_int(st, 4, 64);
    sqlite3_bind_int(st, 5, 64);
    sqlite3_bind_double(st, 6, p->avg_gps_snr);
    sqlite3_bind_double(st, 7, p->avg_bds_snr);
    sqlite3_bind_double(st, 8, p->avg_pdop);
    sqlite3_bind_double(st, 9, p->klob_vert_delay);
    sqlite3_bind_double(st, 10, p->klob_slant_delay);
    sqlite3_bind_double(st, 11, p->klob_slant_factor);
    sqlite3_bind_double(st, 12, p->klob_period);
    sqlite3_bind_text(st, 13, p->activity, -1, SQLITE_STATIC);

    rc = sqlite3_step(st);
    sqlite3_finalize(st);
    sqlite3_close(db);
    return (rc == SQLITE_OK) ? 0 : -1;
}

/* ── Daemon入口 ────────────────────────────────────────── */
int wt_gnss_ion_run(void) {
    time_t now = time(NULL);
    wt_gnss_ion_t ion;
    int rc = wt_gnss_ionosphere_revert(&ion, now);
    if (rc == 0) {
        ion_db_save(&ion);
        printf("━━━ 20. GNSS电离层闪烁监测(C实现) ━━━\n");
        printf("  S4: GPS=%.3f BDS=%.3f | SNR: GPS=%.1fdB BDS=%.1fdB | PDOP=%.2f | %s\n",
               ion.s4_gps >= 0 ? ion.s4_gps : 0.0,
               ion.s4_bds >= 0 ? ion.s4_bds : 0.0,
               ion.avg_gps_snr, ion.avg_bds_snr, ion.avg_pdop, ion.activity);
        printf("  Klobuchar垂直延迟=%.2fns 斜向因子=%.2f\n",
               ion.klob_vert_delay * 1e9, ion.klob_slant_factor);
    }
    return rc;
}

/* ── 初始化电离层表 ────────────────────────────────────── */
int wt_gnss_ion_db_init(const char *path) {
    (void)path;
    sqlite3 *db;
    if (sqlite3_open(WENTIAN_DB, &db) != SQLITE_OK) return -1;
    const char *sql = "CREATE TABLE IF NOT EXISTS local_ionosphere ("
        "ts INTEGER PRIMARY KEY,"
        "s4_gps REAL, s4_bds REAL,"
        "samples_gps INTEGER, samples_bds INTEGER,"
        "gps_snr_avg REAL, bds_snr_avg REAL, pdop_avg REAL,"
        "klob_vert_delay REAL, klob_slant_delay REAL,"
        "klob_slant_factor REAL, klob_period REAL,"
        "activity TEXT)";
    int rc = sqlite3_exec(db, sql, NULL, NULL, NULL);
    sqlite3_close(db);
    return rc == SQLITE_OK ? 0 : -1;
}