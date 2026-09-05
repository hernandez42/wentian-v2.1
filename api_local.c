/* ============================================================
 * api_local.c - 主人自家硬件 (UNO/北斗/SDR/电离层) v1.1
 * ============================================================
 * 项目: 问天 v1.1 (WenTian Weather Station)
 * 所有者: 主人朱涛 BG8SBA, 昆明长水机场楼顶
 * 锁定: 详见 WENTIAN-LOCK.md, 非授权LLM请勿擅改
 *
 * 函数清单:
 *   wt_local_uno()      UNO机柜温/湿/压  ←SQLite
 *   wt_local_gnss()     ATGM336H GPS+北斗 ←SQLite
 *   wt_local_iono()     GNSS电离层 S4+Klobuchar ←SQLite
 *   wt_local_sdr()      V4 SDR扫频峰值 ←CSV文件
 *   wt_local_db_init()  本地表schema初始化
 *   wt_local_save_*()   保存到 /root/data/wentian.db
 *
 * 数据源:
 *   主库:   /root/data/ano_weather.db (主人UNO+北斗硬件DB)
 *   CSV:    /root/data/sdr/v4_sweep_20260902_v2/*.csv
 *
 * UNO表schema (ano_weather):
 *   ts, source, t, h, p, pa, alt, wx
 *   source='UNO_v2.0_bridge' 表示UNO_v2固件主动桥接
 *
 * GPS表schema (gps_log):
 *   lat, lon, alt, fix, sats, hdop, gps_sats, bds_sats,
 *   glonass_sats, pdop, vdop, alt_msl, speed_kts, heading_deg, ts
 *
 * v1.1备注:
 *   - 字段 ts 是字符串 "YYYY-MM-DDTHH:MM:SS" (无时区)
 *   - wt_local_gnss空数据时, wentian.c有NMEA串口回退
 * ============================================================ */
#include "wentian.h"
#include <sys/stat.h>
#include <sqlite3.h>

/* 主人数据库路径 (硬编码,因为是主人专用数据库) */
#define OWNER_DB "/root/data/ano_weather.db"

/* ── UNO 气压校准 ────────────────────────────────────────── */
/* 长水机场 ZPPP: 海拔 2103.5m
 * 问天使用海平面气压公式: P_msl = P_obs * exp(h / 8430)
 * UNO 在机柜内(室内), 实测气压 ~822hPa
 * Open-Meteo 海平面气压 ~1016hPa
 * 校准偏移 = 1016 - 822 * exp(2104/8430)
 *           = 1016 - 822 * 1.283 = 1016 - 1054.8 ≈ -38.8 hPa
 * 但机柜非密闭, 实际偏移用线性回归校准 */
#define UNO_P_OFFSET_HPA     -38.8  /* 校准偏移: 机柜气压→海平面 */

int wt_local_uno(wt_uno_t *out) {
    memset(out, 0, sizeof(*out));
    sqlite3 *db;
    if (sqlite3_open(OWNER_DB, &db) != SQLITE_OK) return -1;
    sqlite3_stmt *st;
    int rc = sqlite3_prepare_v2(db,
        "SELECT ts,t,h,p,pa,alt,wx FROM ano_weather "
        "WHERE source='UNO_v2.0_bridge' ORDER BY ts DESC LIMIT 1",
        -1, &st, NULL);
    if (rc != SQLITE_OK) { sqlite3_close(db); return -1; }
    if (sqlite3_step(st) != SQLITE_ROW) { sqlite3_finalize(st); sqlite3_close(db); return -1; }

    const char *ts = (const char *)sqlite3_column_text(st, 0);
    out->ts = time(NULL);
    /* ts 格式: 2026-09-03T08:48:15 (无时区后缀, 系统时区) */
    struct tm tm = {0};
    if (ts) {
        /* 解析 ISO8601 本地时间 */
        if (sscanf(ts, "%d-%d-%dT%d:%d:%d",
            &tm.tm_year, &tm.tm_mon, &tm.tm_mday,
            &tm.tm_hour, &tm.tm_min, &tm.tm_sec) == 6) {
            tm.tm_year -= 1900;
            tm.tm_mon -= 1;
            /* 用mktime按系统时区解析 */
            out->ts = mktime(&tm);
        }
    }

    out->cabinet_temp     = sqlite3_column_double(st, 1);
    out->cabinet_humid    = sqlite3_column_double(st, 2);
    out->cabinet_pressure = sqlite3_column_double(st, 3);
    /* 校准: UNO 机柜气压 → 海平面气压 */
    out->sea_level_pressure = sqlite3_column_double(st, 4);
    /* 如果 sea_level_pressure 为0或NULL, 用 UNO 气压 + 校准偏移 */
    if (out->sea_level_pressure < 900 || out->sea_level_pressure > 1100) {
        double raw_p = out->cabinet_pressure;
        double alt_km = 2.104;
        /* 标准大气: P_msl = P_raw * exp(alt_km * 1000 / 8430) + offset
         * offset 由机柜室内环境决定 (实测对比 Open-Meteo 校准) */
        out->sea_level_pressure = raw_p * exp(alt_km * 1000.0 / 8430.0) + UNO_P_OFFSET_HPA;
    }
    out->altitude         = sqlite3_column_double(st, 5);
    const unsigned char *wx = sqlite3_column_text(st, 6);
    if (wx) strncpy(out->weather, (const char *)wx, sizeof(out->weather)-1);
    sqlite3_finalize(st);
    sqlite3_close(db);
    return 0;
}

/* ── ATGM336H GPS+北斗 ──────────────────────────────── */

int wt_local_gnss(wt_gnss_t *out) {
    memset(out, 0, sizeof(*out));
    sqlite3 *db;
    if (sqlite3_open(OWNER_DB, &db) != SQLITE_OK) return -1;
    sqlite3_stmt *st;
    int rc = sqlite3_prepare_v2(db,
        "SELECT lat,lon,alt,fix,sats,hdop,gps_sats,bds_sats,glonass_sats,"
        "pdop,vdop,alt_msl,speed_kts,heading_deg,gps_snr_avg,bds_snr_avg,ts "
        "FROM gps_log ORDER BY ts DESC LIMIT 1",
        -1, &st, NULL);
    if (rc != SQLITE_OK) { sqlite3_close(db); return -1; }
    if (sqlite3_step(st) != SQLITE_ROW) { sqlite3_finalize(st); sqlite3_close(db); return -1; }

    out->lat = sqlite3_column_double(st, 0);
    out->lon = sqlite3_column_double(st, 1);
    out->alt = sqlite3_column_double(st, 2);
    out->fix = sqlite3_column_int(st, 3);
    out->total_sats = sqlite3_column_int(st, 4);
    out->hdop = sqlite3_column_double(st, 5);
    out->gps_sats = sqlite3_column_int(st, 6);
    out->bds_sats = sqlite3_column_int(st, 7);
    out->glonass_sats = sqlite3_column_int(st, 8);
    out->pdop = sqlite3_column_double(st, 9);
    out->vdop = sqlite3_column_double(st, 10);
    out->altitude_msl = sqlite3_column_double(st, 11);
    out->speed_kts = sqlite3_column_int(st, 12);
    out->heading_deg = sqlite3_column_int(st, 13);
    /* SNR字段 */
    out->gps_snr = sqlite3_column_double(st, 14);
    out->bds_snr = sqlite3_column_double(st, 15);

    const unsigned char *ts = sqlite3_column_text(st, 16);
    if (ts) {
        struct tm tm = {0};
        if (sscanf((const char *)ts, "%d-%d-%dT%d:%d:%d",
            &tm.tm_year, &tm.tm_mon, &tm.tm_mday,
            &tm.tm_hour, &tm.tm_min, &tm.tm_sec) == 6) {
            tm.tm_year -= 1900;
            tm.tm_mon -= 1;
            out->ts = mktime(&tm);
        }
    }
    sqlite3_finalize(st);
    sqlite3_close(db);
    return 0;
}

/* ── GNSS电离层 (S4+Klobuchar) ───────────────────────── */

int wt_local_iono(wt_iono_t *out) {
    memset(out, 0, sizeof(*out));
    sqlite3 *db;
    if (sqlite3_open(OWNER_DB, &db) != SQLITE_OK) return -1;
    sqlite3_stmt *st;
    int rc = sqlite3_prepare_v2(db,
        "SELECT s4_gps,s4_bds,gps_snr_avg,bds_snr_avg,pdop_avg,vdop_avg,"
        "klob_vert_delay,klob_slant_delay,klob_slant_factor,klob_period_s,"
        "klob_amplitude,klob_geomag_lat,activity,ts "
        "FROM ionosphere ORDER BY ts DESC LIMIT 1",
        -1, &st, NULL);
    if (rc != SQLITE_OK) { sqlite3_close(db); return -1; }
    if (sqlite3_step(st) != SQLITE_ROW) { sqlite3_finalize(st); sqlite3_close(db); return -1; }

    out->s4_gps = sqlite3_column_double(st, 0);
    out->s4_bds = sqlite3_column_double(st, 1);
    out->gps_snr_avg = sqlite3_column_double(st, 2);
    out->bds_snr_avg = sqlite3_column_double(st, 3);
    out->pdop_avg = sqlite3_column_double(st, 4);
    out->vdop_avg = sqlite3_column_double(st, 5);
    out->klob_vert_delay = sqlite3_column_double(st, 6);
    out->klob_slant_delay = sqlite3_column_double(st, 7);
    out->klob_slant_factor = sqlite3_column_double(st, 8);
    out->klob_period_s = sqlite3_column_double(st, 9);
    out->klob_amplitude = sqlite3_column_double(st, 10);
    out->klob_geomag_lat = sqlite3_column_double(st, 11);

    const unsigned char *act = sqlite3_column_text(st, 12);
    if (act) strncpy(out->activity, (const char *)act, sizeof(out->activity)-1);

    const unsigned char *ts = sqlite3_column_text(st, 13);
    if (ts) {
        struct tm tm = {0};
        if (sscanf((const char *)ts, "%d-%d-%dT%d:%d:%d",
            &tm.tm_year, &tm.tm_mon, &tm.tm_mday,
            &tm.tm_hour, &tm.tm_min, &tm.tm_sec) == 6) {
            tm.tm_year -= 1900;
            tm.tm_mon -= 1;
            out->ts = mktime(&tm);
        }
    }
    sqlite3_finalize(st);
    sqlite3_close(db);
    return 0;
}

/* ── SDR扫频峰值 (V4 SDR + LNA + 全向天线) ───────────── */

int wt_local_sdr(wt_sdr_t *out, int max, int *count) {
    *count = 0;
    /* CSV格式: date, time, start_hz, end_hz, bin_hz, num_bins, dBm1, dBm2, ... */
    const char *files[] = {
        "/root/data/sdr/v4_sweep_20260902_v2/amateu2m_144M-148M.csv",
        "/root/data/sdr/v4_sweep_20260902_v2/amateu70cm_430M-440M.csv",
        "/root/data/sdr/v4_sweep_20260902_v2/marine_156M-163M.csv",
    };
    const char *bands[] = {
        "业余2m (144-148MHz)",
        "业余70cm (430-440MHz)",
        "海事 (156-163MHz)"
    };
    struct stat st_buf;

    for (int f = 0; f < 3 && *count < max; f++) {
        if (stat(files[f], &st_buf) != 0) continue;
        FILE *fp = fopen(files[f], "r");
        if (!fp) continue;

        char line[4096];
        double file_peak_dbm = -200, file_peak_freq = 0;
        double file_noise_sum = 0;
        int file_noise_count = 0;
        while (fgets(line, sizeof(line), fp)) {
            char date[32], time_str[32];
            double start_hz, end_hz, bin_hz;
            int num_bins;
            int n = sscanf(line, "%31[^,],%31[^,],%lf,%lf,%lf,%d",
                date, time_str, &start_hz, &end_hz, &bin_hz, &num_bins);
            if (n < 6 || num_bins <= 0 || num_bins > 2000 || bin_hz <= 0) continue;

            /* 跳过前6个逗号 */
            char *p = line;
            for (int i = 0; i < 6; i++) {
                p = strchr(p, ',');
                if (!p) break;
                p++;
            }
            if (!p) continue;

            for (int i = 0; i < num_bins && *p; i++) {
                double dbm;
                if (sscanf(p, "%lf", &dbm) != 1) break;
                double freq = start_hz + i * bin_hz;
                if (dbm > file_peak_dbm) {
                    file_peak_dbm = dbm;
                    file_peak_freq = freq;
                }
                file_noise_sum += dbm;
                file_noise_count++;
                char *q = strchr(p, ',');
                if (!q) break;
                p = q + 1;
            }
        }
        fclose(fp);

        if (file_noise_count > 0) {
            wt_sdr_t *s = &out[*count];
            memset(s, 0, sizeof(*s));
            double noise = file_noise_sum / file_noise_count;
            strncpy(s->file, files[f], sizeof(s->file)-1);
            strncpy(s->band, bands[f], sizeof(s->band)-1);
            s->peak_freq_mhz = file_peak_freq / 1e6;
            s->peak_dbm = file_peak_dbm;
            s->peak_snr = file_peak_dbm - noise;
            s->noise_floor_dbm = noise;
            s->ts = st_buf.st_mtime;
            (*count)++;
        }
    }
    return 0;
}

/* ── 数据库 schema 扩展 (本地数据) ──────────────────── */
int wt_local_db_init(const char *path) {
    sqlite3 *db;
    if (sqlite3_open(path, &db) != SQLITE_OK) return -1;
    char *err = NULL;
    const char *sqls[] = {
        "CREATE TABLE IF NOT EXISTS local_uno ("
        "  ts INTEGER, cabinet_t REAL, cabinet_h REAL, cabinet_p REAL,"
        "  sea_p REAL, altitude REAL, weather TEXT)",
        "CREATE TABLE IF NOT EXISTS local_gnss ("
        "  ts INTEGER, lat REAL, lon REAL, alt REAL, fix INTEGER,"
        "  total_sats INTEGER, gps_sats INTEGER, bds_sats INTEGER,"
        "  glonass_sats INTEGER, pdop REAL, hdop REAL, vdop REAL,"
        "  altitude_msl REAL, speed_kts INTEGER, heading_deg INTEGER,"
        "  gps_snr REAL, bds_snr REAL)",
        "CREATE TABLE IF NOT EXISTS local_iono ("
        "  ts INTEGER, s4_gps REAL, s4_bds REAL, gps_snr REAL, bds_snr REAL,"
        "  pdop_avg REAL, vdop_avg REAL, klob_slant REAL, activity TEXT)",
        "CREATE TABLE IF NOT EXISTS local_sdr ("
        "  ts INTEGER, file TEXT, band TEXT, noise_dbm REAL,"
        "  peak_mhz REAL, peak_dbm REAL, peak_snr REAL)",
        "CREATE INDEX IF NOT EXISTS idx_local_uno_ts ON local_uno(ts)",
        "CREATE INDEX IF NOT EXISTS idx_local_gnss_ts ON local_gnss(ts)",
        "CREATE INDEX IF NOT EXISTS idx_local_iono_ts ON local_iono(ts)",
        "CREATE INDEX IF NOT EXISTS idx_local_sdr_ts ON local_sdr(ts)",
        NULL
    };
    for (int i = 0; sqls[i]; i++) {
        if (sqlite3_exec(db, sqls[i], NULL, NULL, &err) != SQLITE_OK) {
            fprintf(stderr, "[local_db] SQL失败: %s\n", err);
            sqlite3_free(err);
            sqlite3_close(db);
            return -1;
        }
    }
    sqlite3_close(db);
    return 0;
}

int wt_local_save_uno(const wt_uno_t *u) {
    sqlite3 *db; sqlite3_stmt *st;
    if (sqlite3_open(WENTIAN_DB, &db) != SQLITE_OK) return -1;
    if (sqlite3_prepare_v2(db,
        "INSERT INTO local_uno VALUES (?,?,?,?,?,?,?)", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, u->ts);
        sqlite3_bind_double(st, 2, u->cabinet_temp);
        sqlite3_bind_double(st, 3, u->cabinet_humid);
        sqlite3_bind_double(st, 4, u->cabinet_pressure);
        sqlite3_bind_double(st, 5, u->sea_level_pressure);
        sqlite3_bind_double(st, 6, u->altitude);
        sqlite3_bind_text(st, 7, u->weather, -1, SQLITE_TRANSIENT);
        sqlite3_step(st);
    }
    sqlite3_finalize(st); sqlite3_close(db);
    return 0;
}

int wt_local_save_gnss(const wt_gnss_t *g) {
    sqlite3 *db; sqlite3_stmt *st;
    if (sqlite3_open(WENTIAN_DB, &db) != SQLITE_OK) return -1;
    if (sqlite3_prepare_v2(db,
        "INSERT INTO local_gnss VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, g->ts);
        sqlite3_bind_double(st, 2, g->lat);
        sqlite3_bind_double(st, 3, g->lon);
        sqlite3_bind_double(st, 4, g->alt);
        sqlite3_bind_int(st, 5, g->fix);
        sqlite3_bind_int(st, 6, g->total_sats);
        sqlite3_bind_int(st, 7, g->gps_sats);
        sqlite3_bind_int(st, 8, g->bds_sats);
        sqlite3_bind_int(st, 9, g->glonass_sats);
        sqlite3_bind_double(st, 10, g->pdop);
        sqlite3_bind_double(st, 11, g->hdop);
        sqlite3_bind_double(st, 12, g->vdop);
        sqlite3_bind_double(st, 13, g->altitude_msl);
        sqlite3_bind_int(st, 14, g->speed_kts);
        sqlite3_bind_int(st, 15, g->heading_deg);
        sqlite3_step(st);
    }
    sqlite3_finalize(st); sqlite3_close(db);
    return 0;
}

int wt_local_save_iono(const wt_iono_t *i) {
    sqlite3 *db; sqlite3_stmt *st;
    if (sqlite3_open(WENTIAN_DB, &db) != SQLITE_OK) return -1;
    if (sqlite3_prepare_v2(db,
        "INSERT INTO local_iono VALUES (?,?,?,?,?,?,?,?,?)", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, i->ts);
        sqlite3_bind_double(st, 2, i->s4_gps);
        sqlite3_bind_double(st, 3, i->s4_bds);
        sqlite3_bind_double(st, 4, i->gps_snr_avg);
        sqlite3_bind_double(st, 5, i->bds_snr_avg);
        sqlite3_bind_double(st, 6, i->pdop_avg);
        sqlite3_bind_double(st, 7, i->vdop_avg);
        sqlite3_bind_double(st, 8, i->klob_slant_delay);
        sqlite3_bind_text(st, 9, i->activity, -1, SQLITE_TRANSIENT);
        sqlite3_step(st);
    }
    sqlite3_finalize(st); sqlite3_close(db);
    return 0;
}

int wt_local_save_sdr(const wt_sdr_t *s) {
    sqlite3 *db; sqlite3_stmt *st;
    if (sqlite3_open(WENTIAN_DB, &db) != SQLITE_OK) return -1;
    if (sqlite3_prepare_v2(db,
        "INSERT INTO local_sdr VALUES (?,?,?,?,?,?,?)", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, s->ts);
        sqlite3_bind_text(st, 2, s->file, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 3, s->band, -1, SQLITE_TRANSIENT);
        sqlite3_bind_double(st, 4, s->noise_floor_dbm);
        sqlite3_bind_double(st, 5, s->peak_freq_mhz);
        sqlite3_bind_double(st, 6, s->peak_dbm);
        sqlite3_bind_double(st, 7, s->peak_snr);
        sqlite3_step(st);
    }
    sqlite3_finalize(st); sqlite3_close(db);
    return 0;
}