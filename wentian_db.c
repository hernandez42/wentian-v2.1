/* ============================================================
 * wentian_db.c - 问天 SQLite 持久化 v1.1
 * ============================================================
 * 项目: 问天 v1.1 (WenTian Weather Station)
 * 锁定: 详见 WENTIAN-LOCK.md, 非授权LLM请勿擅改
 *
 * DB路径: /root/data/wentian.db (主人硬性指定, 永不变)
 *
 * 14张表清单:
 *   开放API数据:
 *     outdoor   - Open-Meteo室外气象 (12列)
 *     metar     - AviationWeather机场实测 (9列)
 *     air       - 空气质量 PM2.5/PM10/AQI (4列)
 *     marine    - 海洋气象 浪高/周期 (3列)
 *     flood     - 河流流量 (2列, 修过schema错)
 *     apod      - NASA APOD (4列)
 *     donki     - NASA DONKI 5类事件 (5列)
 *     sun       - 日出日落 (6列)
 *     iss       - ISS位置 (3列)
 *     quake     - USGS地震 (7列)
 *
 *   本地硬件数据 (来自 /root/data/ano_weather.db):
 *     local_uno    - UNO机柜温湿压 (7列)
 *     local_gnss   - ATGM336H GPS+北斗 (15列)
 *     local_iono   - 电离层 S4+Klobuchar (9列)
 *     local_sdr    - V4 SDR扫频峰值 (7列)
 *
 * DB_SAVE宏: 打开+prepare+bind+step+close 的胶水代码
 *   使用方法:
 *     DB_SAVE(outdoor, "INSERT INTO outdoor VALUES (?,?,?)",
 *         sqlite3_bind_int64(st, 1, o->fetched_at);
 *         sqlite3_bind_double(st, 2, o->temperature);
 *         sqlite3_bind_text(st, 3, o->weather_text, -1, SQLITE_TRANSIENT));
 *
 * 并发安全: ❌ 单进程写, 多进程会SQLITE_BUSY
 * ============================================================ */
#include "wentian.h"
#include <sqlite3.h>

int wt_db_init(const char *path) {
    sqlite3 *db;
    if (sqlite3_open(path, &db) != SQLITE_OK) {
        fprintf(stderr, "[DB] 无法打开 %s: %s\n", path, sqlite3_errmsg(db));
        return -1;
    }
    char *err = NULL;
    const char *sqls[] = {
        "CREATE TABLE IF NOT EXISTS outdoor ("
        "  ts INTEGER, temp REAL, humid REAL, pressure REAL,"
        "  wind_s REAL, wind_d REAL, weather_code INTEGER, weather TEXT,"
        "  precip REAL, cloud REAL, uv REAL, vis REAL)",
        "CREATE TABLE IF NOT EXISTS metar ("
        "  ts INTEGER, icao TEXT, temp REAL, dewpoint REAL,"
        "  wind_dir INTEGER, wind_speed INTEGER, visib INTEGER, altim REAL, raw TEXT)",
        "CREATE TABLE IF NOT EXISTS air ("
        "  ts INTEGER, pm25 REAL, pm10 REAL, aqi INTEGER)",
        "CREATE TABLE IF NOT EXISTS marine ("
        "  ts INTEGER, wave_height REAL, wave_period REAL)",
        "CREATE TABLE IF NOT EXISTS flood (ts INTEGER, discharge REAL)",
        "CREATE TABLE IF NOT EXISTS apod (ts INTEGER, date TEXT, title TEXT, url TEXT)",
        "CREATE TABLE IF NOT EXISTS donki ("
        "  ts INTEGER, type TEXT, class TEXT, id TEXT, note TEXT)",
        "CREATE TABLE IF NOT EXISTS sun ("
        "  ts INTEGER, lat REAL, lon REAL, sunrise INTEGER, sunset INTEGER, day_length REAL)",
        "CREATE TABLE IF NOT EXISTS iss (ts INTEGER, lat REAL, lon REAL)",
        "CREATE TABLE IF NOT EXISTS quake ("
        "  ts INTEGER, mag REAL, place TEXT, lat REAL, lon REAL, depth REAL, url TEXT)",
        /* v1.1.2: 问天独有 - NOAA SWPC + Kalman融合存储 */
        "CREATE TABLE IF NOT EXISTS swpc_kp ("
        "  ts INTEGER, kp REAL, kp_index INTEGER, kp_text TEXT,"
        "  station_count INTEGER, a_running REAL)",
        "CREATE TABLE IF NOT EXISTS swpc_f107 ("
        "  ts INTEGER, flux_sfu REAL, frequency_mhz INTEGER, ninety_day_mean REAL)",
        "CREATE TABLE IF NOT EXISTS swpc_scale ("
        "  ts INTEGER, date TEXT, g_scale INTEGER, s_scale INTEGER, r_scale INTEGER,"
        "  g_text TEXT, s_text TEXT, r_text TEXT)",
        "CREATE TABLE IF NOT EXISTS kf_pressure ("
        "  ts INTEGER, fused REAL, p_uno REAL, p_om REAL, p_metar REAL, sigma REAL)",
        "CREATE INDEX IF NOT EXISTS idx_outdoor_ts ON outdoor(ts)",
        "CREATE INDEX IF NOT EXISTS idx_quake_ts ON quake(ts)",
        "CREATE INDEX IF NOT EXISTS idx_kf_ts ON kf_pressure(ts)",
        NULL
    };
    for (int i = 0; sqls[i]; i++) {
        if (sqlite3_exec(db, sqls[i], NULL, NULL, &err) != SQLITE_OK) {
            fprintf(stderr, "[DB] SQL失败: %s\n", err);
            sqlite3_free(err);
            sqlite3_close(db);
            return -1;
        }
    }
    sqlite3_close(db);
    printf("[DB] %s 初始化成功\n", path);
    return 0;
}

#define DB_SAVE(name, sql, ...) do { \
    sqlite3 *db; if (sqlite3_open(WENTIAN_DB, &db) == SQLITE_OK) { \
        sqlite3_stmt *st; \
        if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) == SQLITE_OK) { \
            __VA_ARGS__; \
            sqlite3_step(st); \
        } \
        sqlite3_finalize(st); \
        sqlite3_close(db); \
    } \
} while (0)

int wt_db_save_outdoor(const wt_outdoor_t *o) {
    DB_SAVE(outdoor,
        "INSERT INTO outdoor VALUES (?,?,?,?,?,?,?,?,?,?,?,?)",
        sqlite3_bind_int64(st, 1, o->fetched_at);
        sqlite3_bind_double(st, 2, o->temperature);
        sqlite3_bind_double(st, 3, o->humidity);
        sqlite3_bind_double(st, 4, o->pressure_msl);
        sqlite3_bind_double(st, 5, o->wind_speed);
        sqlite3_bind_double(st, 6, o->wind_dir);
        sqlite3_bind_int(st, 7, o->weather_code);
        sqlite3_bind_text(st, 8, o->weather_text, -1, SQLITE_TRANSIENT);
        sqlite3_bind_double(st, 9, o->precipitation);
        sqlite3_bind_double(st, 10, o->cloud_cover);
        sqlite3_bind_double(st, 11, o->uv_index);
        sqlite3_bind_double(st, 12, o->visibility));
    return 0;
}

int wt_db_save_metar(const wt_metar_t *m) {
    DB_SAVE(metar,
        "INSERT INTO metar VALUES (?,?,?,?,?,?,?,?,?)",
        sqlite3_bind_int64(st, 1, m->obs_time);
        sqlite3_bind_text(st, 2, m->icao, -1, SQLITE_TRANSIENT);
        sqlite3_bind_double(st, 3, m->temp);
        sqlite3_bind_double(st, 4, m->dewpoint);
        sqlite3_bind_int(st, 5, m->wind_dir);
        sqlite3_bind_int(st, 6, m->wind_speed_kt);
        sqlite3_bind_int(st, 7, m->visibility_m);
        sqlite3_bind_double(st, 8, m->altim_hpa);
        sqlite3_bind_text(st, 9, m->raw, -1, SQLITE_TRANSIENT));
    return 0;
}

int wt_db_save_quake(const wt_quake_t *q) {
    DB_SAVE(quake,
        "INSERT INTO quake VALUES (?,?,?,?,?,?,?)",
        sqlite3_bind_int64(st, 1, q->time);
        sqlite3_bind_double(st, 2, q->mag);
        sqlite3_bind_text(st, 3, q->place, -1, SQLITE_TRANSIENT);
        sqlite3_bind_double(st, 4, q->lat);
        sqlite3_bind_double(st, 5, q->lon);
        sqlite3_bind_double(st, 6, q->depth_km);
        sqlite3_bind_text(st, 7, q->url, -1, SQLITE_TRANSIENT));
    return 0;
}

int wt_db_save_apod(const wt_apod_t *a) {
    DB_SAVE(apod,
        "INSERT INTO apod VALUES (?,?,?,?)",
        sqlite3_bind_int64(st, 1, time(NULL));
        sqlite3_bind_text(st, 2, a->date, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 3, a->title, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 4, a->url, -1, SQLITE_TRANSIENT));
    return 0;
}

int wt_db_save_donki(const wt_donki_event_t *e) {
    DB_SAVE(donki,
        "INSERT INTO donki VALUES (?,?,?,?,?)",
        sqlite3_bind_int64(st, 1, time(NULL));
        sqlite3_bind_text(st, 2, e->type, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 3, e->classType, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 4, e->id, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 5, e->note, -1, SQLITE_TRANSIENT));
    return 0;
}

int wt_db_save_sun(const wt_sun_t *s, double lat, double lon) {
    DB_SAVE(sun,
        "INSERT INTO sun VALUES (?,?,?,?,?,?)",
        sqlite3_bind_int64(st, 1, time(NULL));
        sqlite3_bind_double(st, 2, lat);
        sqlite3_bind_double(st, 3, lon);
        sqlite3_bind_int64(st, 4, s->sunrise);
        sqlite3_bind_int64(st, 5, s->sunset);
        sqlite3_bind_double(st, 6, s->day_length_sec));
    return 0;
}

int wt_db_save_iss(const wt_iss_t *iss) {
    DB_SAVE(iss,
        "INSERT INTO iss VALUES (?,?,?)",
        sqlite3_bind_int64(st, 1, iss->ts);
        sqlite3_bind_double(st, 2, iss->lat);
        sqlite3_bind_double(st, 3, iss->lon));
    return 0;
}

int wt_db_save_air(double pm25, double pm10, int aqi) {
    DB_SAVE(air, "INSERT INTO air VALUES (?,?,?,?)",
        sqlite3_bind_int64(st, 1, time(NULL));
        sqlite3_bind_double(st, 2, pm25);
        sqlite3_bind_double(st, 3, pm10);
        sqlite3_bind_int(st, 4, aqi));
    return 0;
}

int wt_db_save_marine(double wave_height, double wave_period) {
    DB_SAVE(marine, "INSERT INTO marine VALUES (?,?,?)",
        sqlite3_bind_int64(st, 1, time(NULL));
        sqlite3_bind_double(st, 2, wave_height);
        sqlite3_bind_double(st, 3, wave_period));
    return 0;
}

int wt_db_save_flood(double discharge, double level) {
    (void)level;  /* schema仅2列 */
    DB_SAVE(flood, "INSERT INTO flood VALUES (?,?)",
        sqlite3_bind_int64(st, 1, time(NULL));
        sqlite3_bind_double(st, 2, discharge));
    return 0;
}

/* v1.1.2: NOAA SWPC 存储 (问天独有) */
int wt_db_save_kp(const wt_kp_t *kp) {
    DB_SAVE(swpc_kp, "INSERT INTO swpc_kp VALUES (?,?,?,?,?,?)",
        sqlite3_bind_int64(st, 1, kp->ts);
        sqlite3_bind_double(st, 2, kp->kp);
        sqlite3_bind_int(st, 3, kp->kp_index);
        sqlite3_bind_text(st, 4, kp->kp_text, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(st, 5, kp->station_count);
        sqlite3_bind_double(st, 6, kp->a_running));
    return 0;
}

int wt_db_save_f107(const wt_f107_t *f107) {
    DB_SAVE(swpc_f107, "INSERT INTO swpc_f107 VALUES (?,?,?,?)",
        sqlite3_bind_int64(st, 1, f107->ts);
        sqlite3_bind_double(st, 2, f107->flux_sfu);
        sqlite3_bind_int(st, 3, f107->frequency_mhz);
        sqlite3_bind_double(st, 4, f107->ninety_day_mean));
    return 0;
}

int wt_db_save_scale(const wt_swpc_scale_t *s) {
    DB_SAVE(swpc_scale, "INSERT INTO swpc_scale VALUES (?,?,?,?,?,?,?,?)",
        sqlite3_bind_int64(st, 1, time(NULL));
        sqlite3_bind_text(st, 2, s->date, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(st, 3, s->g_scale);
        sqlite3_bind_int(st, 4, s->s_scale);
        sqlite3_bind_int(st, 5, s->r_scale);
        sqlite3_bind_text(st, 6, s->g_text, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 7, s->s_text, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 8, s->r_text, -1, SQLITE_TRANSIENT));
    return 0;
}

/* v1.1.2: Kalman气压融合存储 (问天独有) */
int wt_db_save_fused_pressure(double fused, double p_uno, double p_om,
                               double p_metar, double sigma) {
    DB_SAVE(kf_pressure, "INSERT INTO kf_pressure VALUES (?,?,?,?,?,?)",
        sqlite3_bind_int64(st, 1, time(NULL));
        sqlite3_bind_double(st, 2, fused);
        sqlite3_bind_double(st, 3, p_uno);
        sqlite3_bind_double(st, 4, p_om);
        sqlite3_bind_double(st, 5, p_metar);
        sqlite3_bind_double(st, 6, sigma));
    return 0;
}