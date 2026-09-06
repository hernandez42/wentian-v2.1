/* ============================================================
 * api_nowcast.c - 短临Nowcasting v1.5 (长水全天气型)
 * ============================================================
 * 项目: 问天 v1.1 (WenTian Weather Station)
 * 所有者: 主人朱涛 BG8SBA, 昆明长水机场楼顶
 * 锁定: 详见 WENTIAN-LOCK.md, 非授权LLM请勿擅改
 *
 * v1.5: 长水全天气型检测
 *   长水ZPPP特产天气: 雷暴、飑线、假冷锋、准静止锋、低空风切变
 *   每种天气型有独立的检测算法和评分, 综合输出最高风险
 *
 * 天气型检测矩阵:
 * ┌─────────────┬──────────────────────────────────────────┐
 * │ 天气型      │ 检测特征                                  │
 * ├─────────────┼──────────────────────────────────────────┤
 * │ 雷暴        │ PWV急升>2mm/15min + 气压降 + 温度降       │
 * │ 飑线        │ 气压骤升>2hPa/10min + 风向突变>60° +      │
 * │             │ PWV先升后骤降(飑线过境)                   │
 * │ 假冷锋      │ 温度骤降>3°C/30min + 无降水 + 气压先降后升 │
 * │ 准静止锋    │ 持续高湿>80% + 温度气压稳定 + 连续阴雨    │
 * │ 低空风切变  │ 风向突变>30°/5min + 风速差>5m/s + METAR WS│
 * └─────────────┴──────────────────────────────────────────┘
 *
 * 输出: 每种天气型独立评分(0-100), 取最高风险输出
 * 存储: wentian.db nowcast表(扩展) + nowcast.json
 *
 * v1.5 新增:
 *   - 飑线检测(气压骤升+风向突变+PWV骤降)
 *   - 假冷锋检测(温度骤降+无降水+气压V型)
 *   - 准静止锋检测(持续高湿+稳定少变+连续降水)
 *   - 低空风切变检测(风向突变+风速差+METAR WS标记)
 *   - nowcast表扩展: 新增squall_line, false_cold, stationary,
 *     wind_shear字段及各型评分
 * ============================================================ */
#include "wentian.h"
#include <sqlite3.h>
#include <math.h>

#define NOWCAST_JSON         "/root/data/fusion/nowcast.json"
#define PWV_HISTORY_CSV      "/root/data/fusion/pwv_history.csv"
#define PWV_MAX_BUFFER       500

/* ── 厄尔尼诺战备模式 (ENSO_MODE) ────────────────────────── */
/* 主人朱涛 BG8SBA 昆明长水机场楼顶 · 2026年9月厄尔尼诺超强级别
 * WMO通报: 尼诺3.4区SST偏高1.5-2.0°C, 持续至2027年2月概率近100%
 * 阈值动态调整: 雷暴/飑线/假冷锋/风切变阈值在厄尔尼诺期收紧 */
#define ENSO_MODE  1  /* 0=平时, 1=厄尔尼诺战备 */

#if ENSO_MODE
  #define SQUALL_PRESS_RISE    1.5   /* 原2.0, 降25% — 飑线气压骤升阈值 */
  #define SQUALL_WIND_DIR_CHG  45     /* 原60, 降25% — 飑线风向突变阈值 */
  #define PWV_SLOPE_STORM      2.5   /* 原3.0, 降17% — 雷暴PWV急升阈值 */
  #define PWV_SLOPE_ALERT      1.5   /* 原2.0, 降25% — 雷暴预警阈值 */
  #define PWV_SLOPE_WATCH      0.8   /* 原1.0, 降20% — 雷暴关注阈值 */
  #define PWV_SLOPE_EARLY      0.5   /* 原0.6, 降17% — 雷暴早期预警 */
  #define SQUALL_PWV_DROP      1.5   /* 原2.0, 降25% — 飑线PWV骤降阈值 */
  #define PWV_ABS_EXTREME      42.5  /* 原50.0, 降15% — 雷暴PWV绝对值 */
  #define PWV_ABS_HIGH         38.3  /* 原45.0, 降15% */
  #define PWV_ABS_MODERATE     34.0  /* 原40.0, 降15% */
  #define FCF_TEMP_DROP        2.0   /* 原3.0, 降33% — 假冷锋温度骤降阈值 */
  #define STAT_HUMID_HIGH      75     /* 原80, 降5% — 准静止锋持续高湿阈值 */
  #define SHEAR_WIND_DIR_CHG   22     /* 原30, 降27% — 风切变风向突变阈值 */
  #define SHEAR_WIND_SPD_CHG   3.8    /* 原5.0, 降24% — 风切变风速差阈值 */
#endif

/* ── 飑线阈值 ──────────────────────────────────────────── */
/* ENSO_MODE已在上方统一定义, 此处保留默认值作为非ENSO模式参考 */
#ifndef ENSO_MODE
#define SQUALL_PRESS_RISE    2.0    /* hPa/10min, 气压骤升 */
#define SQUALL_WIND_DIR_CHG  60     /* deg, 风向突变 */
#define SQUALL_WIND_SPD_CHG  5.0    /* m/s, 风速突增 */
#define SQUALL_PWV_DROP      2.0    /* mm/10min, PWV骤降 */
#endif

/* ── 假冷锋阈值 ────────────────────────────────────────── */
#ifndef ENSO_MODE
#define FCF_TEMP_DROP        3.0    /* °C/30min, 温度骤降 */
#endif
#define FCF_NO_PRECIP_WINDOW 30     /* min, 无降水窗口 */

/* ── 准静止锋阈值 ──────────────────────────────────────── */
#ifndef ENSO_MODE
#define STAT_HUMID_HIGH      80     /* %, 持续高湿 */
#endif
#define STAT_PRESS_STABLE    1.0    /* hPa/30min, 气压稳定 */
#define STAT_TEMP_STABLE     1.0    /* °C/30min, 温度稳定 */
#define STAT_PRECIP_CONT     3      /* 次, 连续降水次数 */

/* ── 风切变阈值 ────────────────────────────────────────── */
#ifndef ENSO_MODE
#define SHEAR_WIND_DIR_CHG   30     /* deg/5min, 风向突变 */
#define SHEAR_WIND_SPD_CHG   5.0    /* m/s, 风速差 */
#endif

/* ── 安全snprintf(带pos指针参数) ───────────────────────── */
#define SAFE_SNPRINTF(fmt, ...) \
    do { \
        int n = snprintf(alert + (*pos), sizeof(alert) - (*pos), fmt, __VA_ARGS__); \
        if (n > 0 && n < (int)(sizeof(alert) - (*pos))) (*pos) += n; \
    } while (0)

/* ── PWV历史加载 ───────────────────────────────────────── */


    static int load_pwv_recent(int minutes, double *times, double *pwv_arr, int max_rows) {
    /* 直接从wentian.db的local_pwv表读取, 时间戳统一为unix格式,
     * 避免CSV混合旧Python版(秒-日)和新C版(unix)导致的时间解析错误 */
    sqlite3 *db;
    if (sqlite3_open(WENTIAN_DB, &db) != SQLITE_OK) return -1;

    time_t cutoff = time(NULL) - minutes * 60;
    sqlite3_stmt *st;
    int rc = sqlite3_prepare_v2(db,
        "SELECT ts, pwv_mm FROM local_pwv "
        "WHERE ts >= ? ORDER BY ts DESC LIMIT ?",
        -1, &st, NULL);
    if (rc != SQLITE_OK) { sqlite3_close(db); return -1; }
    sqlite3_bind_int64(st, 1, (sqlite3_int64)cutoff);
    sqlite3_bind_int(st, 2, max_rows);

    int n = 0;
    while (sqlite3_step(st) == SQLITE_ROW && n < max_rows) {
        times[n] = (double)sqlite3_column_int64(st, 0);
        pwv_arr[n] = sqlite3_column_double(st, 1);
        n++;
    }
    sqlite3_finalize(st);
    sqlite3_close(db);

    /* 返回的数据是倒序(最新在前), 保持这样供nowcast使用 */
    return n;
}

/* ── METAR加载(含风) ──────────────────────────────────── */
/* 从wentian.db的metar表读取最新N条ZPPP METAR数据,
 * 用于nowcast的天气型检测(雷暴/飑线/冷锋/静止锋/风切变) */
static int load_metar_recent(int max_rows, time_t *ts_arr, double *t_arr,
                             double *p_arr, double *wd_arr, double *ws_arr,
                             char *raw_arr, int raw_len) {
    sqlite3 *db;
    if (sqlite3_open(WENTIAN_DB, &db) != SQLITE_OK) return -1;
    sqlite3_stmt *st;
    int rc = sqlite3_prepare_v2(db,
        /* ⚠ 修复(2026-09-06): 列名错 — metar 表实际列是 wind_speed,
         * 旧代码写 wind_speed_kt 导致 prepare 静默失败返回-1,
         * temp_current/press_current 恒为0 → 短临预测段全0,
         * 自进化评分拿0对比实测(温MAE 21°C假高). */
        "SELECT ts, temp, altim, wind_dir, wind_speed, raw "
        "FROM metar WHERE icao='ZPPP' ORDER BY ts DESC LIMIT ?",
        -1, &st, NULL);
    if (rc != SQLITE_OK) { sqlite3_close(db); return -1; }
    sqlite3_bind_int(st, 1, max_rows);
    int n = 0;
    while (sqlite3_step(st) == SQLITE_ROW && n < max_rows) {
        ts_arr[n] = (time_t)sqlite3_column_int64(st, 0);
        t_arr[n] = sqlite3_column_double(st, 1);
        p_arr[n] = sqlite3_column_double(st, 2);
        wd_arr[n] = sqlite3_column_double(st, 3);
        ws_arr[n] = sqlite3_column_double(st, 4) * 0.514444; /* kt→m/s */
        const char *raw = (const char*)sqlite3_column_text(st, 5);
        if (raw && raw_arr) {
            strncpy(raw_arr + n * raw_len, raw, raw_len - 1);
            raw_arr[n * raw_len + raw_len - 1] = '\0';
        }
        n++;
    }
    sqlite3_finalize(st);
    sqlite3_close(db);
    return n;
}

/* ── 风向差(考虑360°循环) ────────────────────────────── */
static double wind_dir_diff(double d1, double d2) {
    if (d1 <= 0 || d2 <= 0) return 0; /* VRB或无效 */
    double diff = fabs(d1 - d2);
    if (diff > 180.0) diff = 360.0 - diff;
    return diff;
}

/* ── PWV差值 ──────────────────────────────────────────── */
static double pwv_delta(const double *times, const double *pwv, int n, int minutes) {
    if (n < 2) return 0.0;
    double t_now = times[n - 1];
    int best = n - 1;
    double best_dt = 1e9;
    for (int i = n - 2; i >= 0; i--) {
        double dt = t_now - times[i];
        if (dt > minutes * 60 + 60) break;
        if (fabs(dt - minutes * 60) < best_dt) {
            best_dt = fabs(dt - minutes * 60);
            best = i;
        }
    }
    double actual_min = (t_now - times[best]) / 60.0;
    if (actual_min < 1.0) return 0.0;
    return (pwv[n - 1] - pwv[best]) * minutes / actual_min;
}

/* ── 雷暴评分 ─────────────────────────────────────────── */
/* GB/T 4.1.1: 伴有雷声和闪电的天气现象
 * 问天间接检测: PWV急升+气压降+温度降 (无直接雷电传感器)
 * 增强: METAR含TS/TSRA标记时确认雷暴, 加25分确认分 */
static int score_thunderstorm(const wt_nowcast_t *nc, char *alert, int *pos,
                               const char *metar_raw) {
    int score = 0;
    int has_ts = 0;
    if (metar_raw && (strstr(metar_raw, "TS") || strstr(metar_raw, "TSRA"))) {
        has_ts = 1;
    }
    /* PWV斜率 */
    if (nc->pwv_slope > PWV_SLOPE_STORM) score += 40;
    else if (nc->pwv_slope > PWV_SLOPE_ALERT) score += 32;
    else if (nc->pwv_slope > PWV_SLOPE_WATCH) score += 20;
    else if (nc->pwv_slope > PWV_SLOPE_EARLY) score += 10;
    /* PWV绝对值 */
    if (nc->pwv_current > PWV_ABS_EXTREME) score += 15;
    else if (nc->pwv_current > PWV_ABS_HIGH) score += 10;
    else if (nc->pwv_current > PWV_ABS_MODERATE) score += 5;
    /* 气压梯度 */
    if (nc->dp_3min < -2.0) score += 25;
    else if (nc->dp_3min < -1.5) score += 20;
    else if (nc->dp_3min < -1.0) score += 15;
    else if (nc->dp_3min < -0.5) score += 8;
    /* 温度梯度 */
    if (nc->dt_5min < -3.0) score += 20;
    else if (nc->dt_5min < -2.0) score += 15;
    else if (nc->dt_5min < -1.0) score += 8;
    else if (nc->dt_5min < -0.5) score += 4;
    /* METAR TS/TSRA 确认加分 */
    if (has_ts) {
        score += 25;
    }

    if (score > 100) score = 100;

    if (score >= 26) {
        if (*pos > 0) alert[(*pos)++] = ' ';
        if (has_ts)
            SAFE_SNPRINTF("⛈雷暴(METAR TS确认! 斜率%.1fmm)", nc->pwv_slope);
        else
            SAFE_SNPRINTF("⛈雷暴(间接检测 斜率%.1fmm)", nc->pwv_slope);
    }
    return score;
}

/* ── 飑线评分 ─────────────────────────────────────────── */
/* 特征: 气压骤升 + 风向突变 + PWV骤降(飑线过境) */
static int score_squall_line(int metar_n, const time_t *ts_arr, const double *p_arr,
                              const double *wd_arr, const double *ws_arr, /* unused */
                              /* unused */ const char *raw_arr, int raw_len,
                              const double *pwv_times, const double *pwv_arr, int pwv_n,
                              double *squall_press, double *squall_wd, double *squall_pwv,
                              char *alert, int *pos) {
    int score = 0;
    *squall_press = *squall_wd = *squall_pwv = 0.0;

    if (metar_n < 3) return 0;

    /* 1. 气压骤升: 10min内气压上升>2hPa */
    double press_rise_10min = 0;
    for (int i = 1; i < metar_n; i++) {
        double dt = (double)(ts_arr[0] - ts_arr[i]);
        if (dt >= 540 && dt <= 660) { /* 9min±1min */
            press_rise_10min = p_arr[0] - p_arr[i];
            break;
        }
        if (dt > 900) break;
    }
    *squall_press = press_rise_10min;

    if (press_rise_10min > SQUALL_PRESS_RISE) {
        int p_score = (int)(press_rise_10min * 10);
        if (p_score > 35) p_score = 35;
        score += p_score;
    }

    /* 2. 风向突变: 5min内风向变化>60° */
    double wd_chg = wind_dir_diff(wd_arr[0], wd_arr[0]);
    for (int i = 1; i < metar_n; i++) {
        double dt = (double)(ts_arr[0] - ts_arr[i]);
        if (dt >= 240 && dt <= 360) { /* 4min±1min */
            wd_chg = wind_dir_diff(wd_arr[0], wd_arr[i]);
            break;
        }
        if (dt > 600) break;
    }
    *squall_wd = wd_chg;

    if (wd_chg > SQUALL_WIND_DIR_CHG) {
        int w_score = (int)((wd_chg - SQUALL_WIND_DIR_CHG) * 2);
        if (w_score > 30) w_score = 30;
        score += w_score;
    }

    /* 3. PWV骤降: 10min内PWV下降>2mm(飑线过境后水汽被吹散) */
    double pwv_drop = -pwv_delta(pwv_times, pwv_arr, pwv_n, 10);
    *squall_pwv = pwv_drop;
    if (pwv_drop > SQUALL_PWV_DROP) {
        int pw_score = (int)((pwv_drop - SQUALL_PWV_DROP) * 8);
        if (pw_score > 35) pw_score = 35;
        score += pw_score;
    }

    /* 飑线特征组合加分: 气压升+风向变+PWV降同时出现 */
    if (press_rise_10min > SQUALL_PRESS_RISE && wd_chg > SQUALL_WIND_DIR_CHG) {
        score += 20; /* 组合特征确认 */
    }

    if (score > 100) score = 100;

    if (score >= 20) {
        if (*pos > 0) alert[(*pos)++] = ' ';
        SAFE_SNPRINTF("🌪飑线(气压↑%.1fhPa 风向变%.0f° PWV↓%.1fmm)",
                      press_rise_10min, wd_chg, pwv_drop);
    }
    return score;
}

/* ── 假冷锋评分 ───────────────────────────────────────── */
/* 特征: 温度骤降 + 无降水 + 气压V型(先降后升) */
static int score_false_cold(int metar_n, /* unused */ const time_t *ts_arr, const double *t_arr,
                             const double *p_arr, const char *raw_arr, int raw_len,
                             double *fc_temp_drop, double *fc_press_v,
                             char *alert, int *pos) {
    int score = 0;
    *fc_temp_drop = *fc_press_v = 0.0;

    if (metar_n < 4) return 0;

    /* 1. 温度骤降: 30min内降温>3°C */
    double temp_drop = 0;
    for (int i = 1; i < metar_n; i++) {
        double dt = (double)(ts_arr[0] - ts_arr[i]);
        if (dt >= 1740 && dt <= 1860) { /* 30min±1min */
            temp_drop = t_arr[0] - t_arr[i];
            break;
        }
        if (dt > 2400) break;
    }
    *fc_temp_drop = temp_drop;

    if (temp_drop > FCF_TEMP_DROP) {
        int t_score = (int)((temp_drop - FCF_TEMP_DROP) * 10);
        if (t_score > 40) t_score = 40;
        score += t_score;
    }

    /* 2. 无降水: METAR中无RA/TSRA/SHRA标记 */
    int has_precip = 0;
    for (int i = 0; i < metar_n && i < 6; i++) {
        const char *raw = raw_arr + i * raw_len;
        if (raw && (strstr(raw, "RA") || strstr(raw, "TSRA") ||
                    strstr(raw, "SHRA") || strstr(raw, "SN"))) {
            has_precip = 1; break;
        }
    }
    if (!has_precip && temp_drop > FCF_TEMP_DROP) {
        score += 25; /* 无降水降温=假冷锋特征 */
    }

    /* 3. 气压V型: 先降后升(冷锋过境特征) */
    double min_p = p_arr[0], max_p_start = p_arr[0];
    (void)max_p_start;
    int min_idx = 0;
    for (int i = 1; i < metar_n; i++) {
        if (p_arr[i] < min_p) { min_p = p_arr[i]; min_idx = i; }
        if (i < metar_n / 2) max_p_start = p_arr[i];
    }
    double press_v = (min_idx > 0 && min_idx < metar_n - 1) ?
                     (p_arr[0] - min_p) : 0;
    *fc_press_v = press_v;
    if (press_v > 1.5) {
        score += 15;
    }

    if (score > 100) score = 100;

    if (score >= 20) {
        if (*pos > 0) alert[(*pos)++] = ' ';
        SAFE_SNPRINTF("❄假冷锋(降温%.1f°C %s降水 V型气压%.1fhPa)",
                      temp_drop, has_precip?"有":"无", press_v);
    }
    return score;
}

/* ── 准静止锋评分 ─────────────────────────────────────── */
/* 特征: 持续高湿 + 温度气压稳定 + 连续降水 */
static int score_stationary(int metar_n, const time_t *ts_arr, const double *t_arr,
                             const double *p_arr, const char *raw_arr, int raw_len,
                             double *stat_humid_avg, double *stat_press_var,
                             char *alert, int *pos,
                             /* 需从外部传入湿度数据 */
                             double hum_recent[6]) {
    int score = 0;
    *stat_humid_avg = 0.0; *stat_press_var = 0.0;

    if (metar_n < 3) return 0;

    /* 1. 持续高湿(需外部湿度数据, 用Open-Meteo或UNO近似) */
    double hum_avg = 0;
    int hum_n = 0;
    for (int i = 0; i < 6; i++) {
        if (hum_recent[i] > 0) { hum_avg += hum_recent[i]; hum_n++; }
    }
    if (hum_n > 0) hum_avg /= hum_n;
    *stat_humid_avg = hum_avg;

    if (hum_avg > STAT_HUMID_HIGH) {
        score += 30;
    } else if (hum_avg > 70) {
        score += 20;
    }

    /* 2. 温度气压稳定: 30min内变化<1°C/1hPa */
    double t_range = 0, p_range = 0;
    double t_min = t_arr[0], t_max = t_arr[0];
    double p_min = p_arr[0], p_max = p_arr[0];
    for (int i = 1; i < metar_n && i < 6; i++) {
        if (t_arr[i] < t_min) t_min = t_arr[i];
        if (t_arr[i] > t_max) t_max = t_arr[i];
        if (p_arr[i] < p_min) p_min = p_arr[i];
        if (p_arr[i] > p_max) p_max = p_arr[i];
    }
    t_range = t_max - t_min;
    p_range = p_max - p_min;
    *stat_press_var = p_range;

    if (t_range < STAT_TEMP_STABLE && p_range < STAT_PRESS_STABLE) {
        score += 25;
    } else if (t_range < 2.0 && p_range < 2.0) {
        score += 15;
    }

    /* 3. 连续降水: METAR中多次出现降水标记 */
    int precip_count = 0;
    for (int i = 0; i < metar_n && i < 6; i++) {
        const char *raw = raw_arr + i * raw_len;
        if (raw && (strstr(raw, "RA") || strstr(raw, "TSRA") ||
                    strstr(raw, "SHRA"))) {
            precip_count++;
        }
    }
    if (precip_count >= STAT_PRECIP_CONT) {
        score += 25;
    } else if (precip_count >= 1) {
        score += 10;
    }

    if (score > 100) score = 100;

    if (score >= 20) {
        if (*pos > 0) alert[(*pos)++] = ' ';
        SAFE_SNPRINTF("🌫准静止锋(湿度%.0f%% 变温%.1f°C 变压%.1fhPa 降水%d次)",
                      hum_avg, t_range, p_range, precip_count);
    }
    return score;
}

/* ── 低空风切变评分 ───────────────────────────────────── */
/* 特征: 风向突变 + 风速差 + METAR WS标记 */
static int score_wind_shear(int metar_n, const time_t *ts_arr, const double *wd_arr,
                             const double *ws_arr, const char *raw_arr, int raw_len,
                             double *shear_wd, double *shear_wspd,
                             char *alert, int *pos) {
    int score = 0;
    *shear_wd = *shear_wspd = 0.0;

    if (metar_n < 2) return 0;

    /* 1. 风向突变: 5min内风向变化>30° */
    double wd_chg = wind_dir_diff(wd_arr[0], wd_arr[0]);
    for (int i = 1; i < metar_n; i++) {
        double dt = (double)(ts_arr[0] - ts_arr[i]);
        if (dt >= 240 && dt <= 360) {
            wd_chg = wind_dir_diff(wd_arr[0], wd_arr[i]);
            break;
        }
        if (dt > 600) break;
    }
    *shear_wd = wd_chg;

    if (wd_chg > SHEAR_WIND_DIR_CHG) {
        int w_score = (int)((wd_chg - SHEAR_WIND_DIR_CHG) * 2);
        if (w_score > 40) w_score = 40;
        score += w_score;
    }

    /* 2. 风速差: 5min内风速变化>5m/s */
    double wspd_chg = 0.0;
    for (int i = 1; i < metar_n; i++) {
        double dt = (double)(ts_arr[0] - ts_arr[i]);
        if (dt >= 240.0 && dt <= 360.0) {
            wspd_chg = fabs(ws_arr[0] - ws_arr[i]);
            break;
        }
        if (dt > 600.0) break;
    }
    *shear_wspd = wspd_chg;

    if (wspd_chg > SHEAR_WIND_SPD_CHG) {
        int s_score = (int)((wspd_chg - SHEAR_WIND_SPD_CHG) * 6);
        if (s_score > 30) s_score = 30;
        score += s_score;
    }

    /* 3. METAR WS标记 */
    int has_ws = 0;
    for (int i = 0; i < metar_n && i < 3; i++) {
        const char *raw = raw_arr + i * raw_len;
        if (raw && (strstr(raw, "WS") || strstr(raw, "WINDSHEAR"))) {
            has_ws = 1; break;
        }
    }
    if (has_ws) score += 20;

    if (score > 100) score = 100;

    if (score >= 15) {
        if (*pos > 0) alert[(*pos)++] = ' ';
        SAFE_SNPRINTF("💨风切变(风向变%.0f° 风速差%.1fm/s %s)",
                      wd_chg, wspd_chg, has_ws?"METAR确认":"");
    }

    return score;
}

/* ── METAR降水强度分级 (GB/T 4.1.15-17) ──────────────────── */
/* 根据METAR raw中的降水代码和强度标记, 显式分级
 * GB/T 4.1.15: 小雨 light rain   - RA, -RA
 * GB/T 4.1.16: 中雨 moderate rain - RA, +RA
 * GB/T 4.1.17: 大雨 heavy rain   - +RA, +RAGR, +TSRA
 * 暴雨 rainstorm: +TSRA, +SHRA, RAGR
 * 无降水: 无RA/SN/DZ/FZRA等标记 */
static void wt_metar_precip_level(const char *raw, char *out_level, int max_len,
                                   double *out_1h_mm) {
    *out_1h_mm = 0.0;
    if (!raw || !out_level) { snprintf(out_level, max_len, "无降水"); return; }

    int has_ts = strstr(raw, "TS") != NULL;
    int has_sh = strstr(raw, "SH") != NULL;
    int has_gr = strstr(raw, "GR") != NULL || strstr(raw, "GS") != NULL;
    int has_rain = strstr(raw, "RA") != NULL || strstr(raw, "FZRA") != NULL;
    int has_snow = strstr(raw, "SN") != NULL;
    int has_dz   = strstr(raw, "DZ") != NULL;
    int has_pl   = strstr(raw, "PL") != NULL;

    /* 降水强度前缀: - 弱, + 强, 双++ 极强 */
    int strong = strstr(raw, "+") != NULL;
    /* weak unused */ (void)strstr(raw, "-");

    if (has_ts && (has_gr || has_rain)) {
        snprintf(out_level, max_len, "暴雨");
        *out_1h_mm = 30.0;
    } else if (has_sh && has_rain) {
        snprintf(out_level, max_len, "阵雨");
        *out_1h_mm = 15.0;
    } else if (has_gr) {
        snprintf(out_level, max_len, "冰雹");
        *out_1h_mm = 20.0;
    } else if (has_rain && strong) {
        snprintf(out_level, max_len, "大雨");
        *out_1h_mm = 15.0;
    } else if (has_rain) {
        snprintf(out_level, max_len, "小雨");
        *out_1h_mm = 2.5;
    } else if (has_snow && strong) {
        snprintf(out_level, max_len, "大雪");
        *out_1h_mm = 8.0;
    } else if (has_snow) {
        snprintf(out_level, max_len, "小雪");
        *out_1h_mm = 1.0;
    } else if (has_dz) {
        snprintf(out_level, max_len, "毛毛雨");
        *out_1h_mm = 0.5;
    } else if (has_pl) {
        snprintf(out_level, max_len, "冰雹");
        *out_1h_mm = 10.0;
    } else {
        snprintf(out_level, max_len, "无降水");
        *out_1h_mm = 0.0;
    }
}

/* ── 假冷锋国标注释 ──────────────────────────────────────── */
/* GB/T 35663-2017 中无"假冷锋"术语。此现象指:
 * 无降水伴随的温度骤降+气压V型变化, 类似冷锋特征但无锋面过境。
 * 代码中以 false_cold_score 计分, 注释明确标注非国标术语。 */
static void wt_false_cold_note(char *out, int max_len) {
    snprintf(out, max_len,
        "非GB/T标准术语; 指无降水伴随的温度骤降现象(类冷锋特征), "
        "GB/T对应: 冷锋4.2.7 + 温度骤降现象");
}

/* ── Nowcasting核心 v1.5 ───────────────────────────────── */
int wt_nowcast_compute(wt_nowcast_t *out) {
    memset(out, 0, sizeof(*out));
    out->ts = time(NULL);

    /* ── 加载PWV ──────────────────────────────────────── */
    double pwv_times[120], pwv_vals[120];
    int pwv_n = load_pwv_recent(30, pwv_times, pwv_vals, 120);

    if (pwv_n >= 5) {
        out->pwv_current = pwv_vals[pwv_n - 1];
        out->pwv_slope = pwv_delta(pwv_times, pwv_vals, pwv_n, 15);
        /* 15min前PWV */
        double t_now = pwv_times[pwv_n - 1];
        out->pwv_15min_ago = pwv_vals[pwv_n - 1];
        for (int i = pwv_n - 2; i >= 0; i--) {
            if (t_now - pwv_times[i] >= 890.0) { out->pwv_15min_ago = pwv_vals[i]; break; }
        }
        /* PWV绝对值分 */
        if (out->pwv_current > PWV_ABS_EXTREME) out->pwv_abs_score = 15;
        else if (out->pwv_current > PWV_ABS_HIGH) out->pwv_abs_score = 10;
        else if (out->pwv_current > PWV_ABS_MODERATE) out->pwv_abs_score = 5;
    }

    /* ── 加载METAR ────────────────────────────────────── */
    time_t metar_ts[20];
    double metar_t[20], metar_p[20], metar_wd[20], metar_ws[20];
    char metar_raw[20][128];
    int metar_n = load_metar_recent(20, metar_ts, metar_t, metar_p,
                                     metar_wd, metar_ws, metar_raw[0], 128);

    if (metar_n >= 2) {
        out->press_current = metar_p[0];
        out->press_3min_ago = metar_p[0];
        for (int i = 1; i < metar_n; i++) {
            double dt = (double)(metar_ts[0] - metar_ts[i]);
            if (dt >= 120.0 && dt <= 240.0) { out->press_3min_ago = metar_p[i]; break; }
            if (dt > 300.0) break;
        }
        out->dp_3min = out->press_current - out->press_3min_ago;

        out->temp_current = metar_t[0];
        out->temp_5min_ago = metar_t[0];
        for (int i = 1; i < metar_n; i++) {
            double dt = (double)(metar_ts[0] - metar_ts[i]);
            if (dt >= 180.0 && dt <= 420.0) { out->temp_5min_ago = metar_t[i]; break; }
            if (dt > 600.0) break;
        }
        out->dt_5min = out->temp_current - out->temp_5min_ago;
    }

    /* ── 获取湿度(从Open-Meteo或UNO) ─────────────────── */
    double hum_recent[6] = {0};
    /* 尝试从wentian.db outdoor表取最近湿度 */
    sqlite3 *db;
    if (sqlite3_open(WENTIAN_DB, &db) == SQLITE_OK) {
        sqlite3_stmt *st;
        int rc = sqlite3_prepare_v2(db,
            "SELECT humidity FROM outdoor ORDER BY ts DESC LIMIT 6", -1, &st, NULL);
        if (rc == SQLITE_OK) {
            int i = 0;
            while (sqlite3_step(st) == SQLITE_ROW && i < 6) {
                hum_recent[i++] = sqlite3_column_double(st, 0);
            }
            sqlite3_finalize(st);
        }
        sqlite3_close(db);
    }

    /* ── 各天气型独立评分 ─────────────────────────────── */
    char alert[256] = {0};
    int pos = 0;

    /* 1. 雷暴 (GB/T 4.1.1: 伴有雷声和闪电的天气现象;
     *  问天间接检测: PWV急升+气压降+温度降, 需METAR TS标记确认) */
    out->thunder_score = score_thunderstorm(out, alert, &pos, metar_raw[0]);

    /* 2. 飑线 (GB/T 4.1.11: 带状雷暴群构成的风向风速突变强对流天气;
     *  问天间接检测: 气压骤升+风向突变+PWV骤降, 无雷达/卫星间接推断) */
    out->squall_score = score_squall_line(metar_n, metar_ts, metar_p, metar_wd, metar_ws,
                                           metar_raw[0], 128,
                                           pwv_times, pwv_vals, pwv_n,
                                           &out->squall_press_rise, &out->squall_wd_chg,
                                           &out->squall_pwv_drop, alert, &pos);

    /* 3. 假冷锋 (非GB/T标准术语; 指无降水伴随的温度骤降现象, 类冷锋特征;
     *  GB/T对应: 冷锋 4.2.7 + 温度骤降现象) */
    out->false_cold_score = score_false_cold(metar_n, metar_ts, metar_t, metar_p,
                                              metar_raw[0], 128,
                                              &out->fc_temp_drop, &out->fc_press_v,
                                              alert, &pos);

    /* 4. 准静止锋 (GB/T 4.2.9: 移动缓慢、很少移动的锋) */
    out->stationary_score = score_stationary(metar_n, metar_ts, metar_t, metar_p,
                                              metar_raw[0], 128,
                                              &out->stat_humid_avg, &out->stat_press_var,
                                              alert, &pos, hum_recent);

    /* 5. 风切变 (GB/T 4.3.5: 风向/风速在短距离内的剧烈变化;
     *  问天检测: 基于METAR时序的风切变间接检测, 非空间变化) */
    out->wind_shear_score = score_wind_shear(metar_n, metar_ts, metar_wd, metar_ws,
                                              metar_raw[0], 128,
                                              &out->shear_wd_chg, &out->shear_wspd_chg,
                                              alert, &pos);

    /* ── 综合: 取最高风险天气型 ───────────────────────── */
    int scores[] = { out->thunder_score, out->squall_score, out->false_cold_score,
                     out->stationary_score, out->wind_shear_score };
    const char *types[] = { "雷暴", "飑线", "假冷锋", "准静止锋", "风切变" };
    out->score = 0;
    out->level[0] = '\0';
    out->warning_level[0] = '\0';
    out->precip_intensity[0] = '\0';
    out->false_cold_note[0] = '\0';
    for (int i = 0; i < 5; i++) {
        if (scores[i] > out->score) out->score = scores[i];
    }
    /* ⚠ 修复(2026-09-06): 天气型标签仅在达到"关注"级(>=26)时给出。
     * 旧逻辑只要有非0分(哪怕PWV绝对值贡献的15分)就把level写成"雷暴",
     * 卡片上出现"雷暴 评分15/100 告警=无"的自相矛盾显示 */
    if (out->score >= 26) {
        for (int i = 0; i < 5; i++) {
            if (scores[i] == out->score) { strcpy(out->level, types[i]); break; }
        }
    } else {
        strcpy(out->level, "稳定");
    }
    if (out->score > 100) out->score = 100;

    /* ── GB/T 4.3.1 警报等级 (基于评分) ─────────────────── */
    if (out->score >= 76)      strcpy(out->warning_level, "强预警");
    else if (out->score >= 51) strcpy(out->warning_level, "预警");
    else if (out->score >= 26) strcpy(out->warning_level, "关注");
    else                       strcpy(out->warning_level, "无");

    /* 综合等级 (forecast) */
    if (out->score >= 76) { strcpy(out->forecast, "强天气 imminent"); }
    else if (out->score >= 51) { strcpy(out->forecast, "天气发展中"); }
    else if (out->score >= 26) { strcpy(out->forecast, "关注天气生成"); }
    else { strcpy(out->forecast, "天气稳定"); }

    /* 告警信息 */
    if (pos == 0) snprintf(alert, sizeof(alert), "无显著天气信号");
    strcpy(out->alert_msg, alert);

    /* ── METAR降水强度分级 (GB/T 4.1.15-17) ──────────────── */
    /* 用最新METAR raw数据进行降水代码解析, 显式分级 */
    if (metar_n > 0 && metar_raw[0][0]) {
        wt_metar_precip_level(metar_raw[0], out->precip_intensity,
                              sizeof(out->precip_intensity), &out->precip_1h_mm);
    } else {
        snprintf(out->precip_intensity, sizeof(out->precip_intensity), "无数据");
        out->precip_1h_mm = 0.0;
    }

    /* ── 假冷锋国标注释 (非GB/T标准术语) ──────────────────── */
    wt_false_cold_note(out->false_cold_note, sizeof(out->false_cold_note));

    return 0;
}

/* ── 保存Nowcast(扩展字段) ────────────────────────────── */
int wt_db_save_nowcast(const wt_nowcast_t *nc) {
    sqlite3 *db;
    if (sqlite3_open(WENTIAN_DB, &db) != SQLITE_OK) return -1;
    const char *sql =
        "INSERT OR REPLACE INTO nowcast "
        "(ts,pwv_slope,dp_3min,dt_5min,score,level,forecast,"
        " pwv_current,pwv_15min_ago,press_current,press_3min_ago,"
        " temp_current,temp_5min_ago,pwv_score,pwv_abs_score,press_score,temp_score,alert_msg,"
        " thunder_score,squall_score,false_cold_score,stationary_score,wind_shear_score,"
        " squall_press_rise,squall_wd_chg,squall_pwv_drop,"
        " fc_temp_drop,fc_press_v,"
        " stat_humid_avg,stat_press_var,"
        " shear_wd_chg,shear_wspd_chg,"
        " warning_level,precip_intensity,precip_1h_mm,false_cold_note) "
        "VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)";
    sqlite3_stmt *st;
    int rc = sqlite3_prepare_v2(db, sql, -1, &st, NULL);
    if (rc != SQLITE_OK) { sqlite3_close(db); return -1; }
    sqlite3_bind_int64(st, 1, (sqlite3_int64)nc->ts);
    sqlite3_bind_double(st, 2, nc->pwv_slope);
    sqlite3_bind_double(st, 3, nc->dp_3min);
    sqlite3_bind_double(st, 4, nc->dt_5min);
    sqlite3_bind_int(st, 5, nc->score);
    sqlite3_bind_text(st, 6, nc->level, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 7, nc->forecast, -1, SQLITE_TRANSIENT);
    sqlite3_bind_double(st, 8, nc->pwv_current);
    sqlite3_bind_double(st, 9, nc->pwv_15min_ago);
    sqlite3_bind_double(st, 10, nc->press_current);
    sqlite3_bind_double(st, 11, nc->press_3min_ago);
    sqlite3_bind_double(st, 12, nc->temp_current);
    sqlite3_bind_double(st, 13, nc->temp_5min_ago);
    sqlite3_bind_int(st, 14, nc->pwv_score);
    sqlite3_bind_int(st, 15, nc->pwv_abs_score);
    sqlite3_bind_int(st, 16, nc->press_score);
    sqlite3_bind_int(st, 17, nc->temp_score);
    sqlite3_bind_text(st, 18, nc->alert_msg, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(st, 19, nc->thunder_score);
    sqlite3_bind_int(st, 20, nc->squall_score);
    sqlite3_bind_int(st, 21, nc->false_cold_score);
    sqlite3_bind_int(st, 22, nc->stationary_score);
    sqlite3_bind_int(st, 23, nc->wind_shear_score);
    sqlite3_bind_double(st, 24, nc->squall_press_rise);
    sqlite3_bind_double(st, 25, nc->squall_wd_chg);
    sqlite3_bind_double(st, 26, nc->squall_pwv_drop);
    sqlite3_bind_double(st, 27, nc->fc_temp_drop);
    sqlite3_bind_double(st, 28, nc->fc_press_v);
    sqlite3_bind_double(st, 29, nc->stat_humid_avg);
    sqlite3_bind_double(st, 30, nc->stat_press_var);
    sqlite3_bind_double(st, 31, nc->shear_wd_chg);
    sqlite3_bind_double(st, 32, nc->shear_wspd_chg);
    sqlite3_bind_text(st, 33, nc->warning_level, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 34, nc->precip_intensity, -1, SQLITE_TRANSIENT);
    sqlite3_bind_double(st, 35, nc->precip_1h_mm);
    sqlite3_bind_text(st, 36, nc->false_cold_note, -1, SQLITE_TRANSIENT);
    rc = sqlite3_step(st);
    sqlite3_finalize(st);
    sqlite3_close(db);
    return (rc == SQLITE_DONE) ? 0 : -1;
}

/* ── JSON输出 ──────────────────────────────────────────── */
static int wt_nowcast_save_json(const wt_nowcast_t *nc) {
    FILE *f = fopen(NOWCAST_JSON, "w");
    if (!f) return -1;
    fprintf(f, "{\n");
    fprintf(f, "  \"ts\": %ld,\n", (long)nc->ts);
    fprintf(f, "  \"primary_type\": \"%s\",\n", nc->level);
    fprintf(f, "  \"warning_level\": \"%s\",\n", nc->warning_level);
    fprintf(f, "  \"precip_intensity\": \"%s\",\n", nc->precip_intensity);
    fprintf(f, "  \"precip_1h_mm\": %.2f,\n", nc->precip_1h_mm);
    fprintf(f, "  \"forecast\": \"%s\",\n", nc->forecast);
    fprintf(f, "  \"score\": %d,\n", nc->score);
    fprintf(f, "  \"thunder_score\": %d,\n", nc->thunder_score);
    fprintf(f, "  \"squall_score\": %d,\n", nc->squall_score);
    fprintf(f, "  \"false_cold_score\": %d,\n", nc->false_cold_score);
    fprintf(f, "  \"false_cold_note\": \"%s\",\n", nc->false_cold_note);
    fprintf(f, "  \"stationary_score\": %d,\n", nc->stationary_score);
    fprintf(f, "  \"wind_shear_score\": %d,\n", nc->wind_shear_score);
    fprintf(f, "  \"pwv_current\": %.2f,\n", nc->pwv_current);
    fprintf(f, "  \"pwv_slope_15min\": %.2f,\n", nc->pwv_slope);
    fprintf(f, "  \"press_current\": %.1f,\n", nc->press_current);
    fprintf(f, "  \"temp_current\": %.1f,\n", nc->temp_current);
    fprintf(f, "  \"squall_press_rise\": %.2f,\n", nc->squall_press_rise);
    fprintf(f, "  \"squall_wd_chg\": %.1f,\n", nc->squall_wd_chg);
    fprintf(f, "  \"squall_pwv_drop\": %.2f,\n", nc->squall_pwv_drop);
    fprintf(f, "  \"fc_temp_drop\": %.2f,\n", nc->fc_temp_drop);
    fprintf(f, "  \"fc_press_v\": %.2f,\n", nc->fc_press_v);
    fprintf(f, "  \"stat_humid_avg\": %.1f,\n", nc->stat_humid_avg);
    fprintf(f, "  \"stat_press_var\": %.2f,\n", nc->stat_press_var);
    fprintf(f, "  \"shear_wd_chg\": %.1f,\n", nc->shear_wd_chg);
    fprintf(f, "  \"shear_wspd_chg\": %.2f,\n", nc->shear_wspd_chg);
    fprintf(f, "  \"alert_msg\": \"%s\"\n", nc->alert_msg);
    fprintf(f, "}\n");
    fclose(f);
    return 0;
}

/* ── Nowcast主入口 ─────────────────────────────────────── */
int wt_nowcast_run(void) {
    wt_nowcast_t nc = {0};
    wt_nowcast_compute(&nc);

    printf("\n━━━ 17. 短临Nowcasting (全天气型) ━━━\n");
    printf("  ✅ 主风险: %s (评分: %d/100) - %s\n", nc.level, nc.score, nc.forecast);
    printf("  ✅ 告警: %s\n", nc.alert_msg);
    printf("  ── 各天气型评分 ──\n");
    printf("    ⛈ 雷暴: %d分 | 🌪 飑线: %d分 | ❄ 假冷锋: %d分\n",
           nc.thunder_score, nc.squall_score, nc.false_cold_score);
    printf("    🌫 准静止锋: %d分 | 💨 风切变: %d分\n",
           nc.stationary_score, nc.wind_shear_score);
    printf("  ── 关键指标 ──\n");
    printf("    PWV: %.2fmm(15min变化%.2fmm) | 气压: %.1fhPa(3min变化%.2f)\n",
           nc.pwv_current, nc.pwv_slope, nc.press_current, nc.dp_3min);
    printf("    飑线: 气压升%.1fhPa 风向变%.0f° PWV降%.1fmm\n",
           nc.squall_press_rise, nc.squall_wd_chg, nc.squall_pwv_drop);
    printf("    假冷锋: 降温%.1f°C V型气压%.1fhPa\n",
           nc.fc_temp_drop, nc.fc_press_v);
    printf("    风切变: 风向变%.0f° 风速差%.1fm/s\n",
           nc.shear_wd_chg, nc.shear_wspd_chg);

    /* 条件触发: 评分≥26推预警 */
    if (nc.score >= 26) {
        FILE *tf = fopen("/root/data/fusion/nowcast_trigger.json", "w");
        if (tf) {
            fprintf(tf, "{\n");
            fprintf(tf, "  \"ts\": %ld,\n", (long)nc.ts);
            fprintf(tf, "  \"level\": \"%s\",\n", nc.level);
            fprintf(tf, "  \"forecast\": \"%s\",\n", nc.forecast);
            fprintf(tf, "  \"score\": %d,\n", nc.score);
            fprintf(tf, "  \"thunder_score\": %d,\n", nc.thunder_score);
            fprintf(tf, "  \"squall_score\": %d,\n", nc.squall_score);
            fprintf(tf, "  \"false_cold_score\": %d,\n", nc.false_cold_score);
            fprintf(tf, "  \"stationary_score\": %d,\n", nc.stationary_score);
            fprintf(tf, "  \"wind_shear_score\": %d,\n", nc.wind_shear_score);
            fprintf(tf, "  \"pwv_current\": %.2f,\n", nc.pwv_current);
            fprintf(tf, "  \"pwv_slope_15min\": %.2f,\n", nc.pwv_slope);
            fprintf(tf, "  \"dp_3min\": %.2f,\n", nc.dp_3min);
            fprintf(tf, "  \"dt_5min\": %.2f,\n", nc.dt_5min);
            fprintf(tf, "  \"alert_msg\": \"%s\"\n", nc.alert_msg);
            fprintf(tf, "}\n");
            fclose(tf);
        }
        int ret = system("python3 /root/scripts/wentian/push_alert.py --json /root/data/fusion/nowcast_trigger.json 2>/dev/null &");
        (void)ret;
    } else {
        remove("/root/data/fusion/nowcast_trigger.json");
    }

    wt_db_save_nowcast(&nc);
    wt_nowcast_save_json(&nc);
    return 0;
}

/* ── Nowcast表初始化(扩展) ─────────────────────────────── */
int wt_nowcast_db_init(const char *path) {
    sqlite3 *db;
    if (sqlite3_open(path, &db) != SQLITE_OK) return -1;
    /* 先创建基础表(如果不存在) */
    const char *sql_create =
        "CREATE TABLE IF NOT EXISTS nowcast ("
        "ts INTEGER PRIMARY KEY, "
        "pwv_slope REAL, dp_3min REAL, dt_5min REAL, "
        "score INTEGER, level TEXT, forecast TEXT, "
        "pwv_current REAL, pwv_15min_ago REAL, "
        "press_current REAL, press_3min_ago REAL, "
        "temp_current REAL, temp_5min_ago REAL, "
        "pwv_score INTEGER, pwv_abs_score INTEGER, press_score INTEGER, temp_score INTEGER, "
        "alert_msg TEXT)";
    sqlite3_exec(db, sql_create, NULL, NULL, NULL);

    /* 逐个添加新列(如果不存在) */
    const char *cols[] = {
        "thunder_score INTEGER DEFAULT 0",
        "squall_score INTEGER DEFAULT 0",
        "false_cold_score INTEGER DEFAULT 0",
        "stationary_score INTEGER DEFAULT 0",
        "wind_shear_score INTEGER DEFAULT 0",
        "squall_press_rise REAL DEFAULT 0",
        "squall_wd_chg REAL DEFAULT 0",
        "squall_pwv_drop REAL DEFAULT 0",
        "fc_temp_drop REAL DEFAULT 0",
        "fc_press_v REAL DEFAULT 0",
        "stat_humid_avg REAL DEFAULT 0",
        "stat_press_var REAL DEFAULT 0",
        "shear_wd_chg REAL DEFAULT 0",
        "shear_wspd_chg REAL DEFAULT 0",
    };
    for (int i = 0; i < (int)(sizeof(cols)/sizeof(cols[0])); i++) {
        char sql[256];
        snprintf(sql, sizeof(sql), "ALTER TABLE nowcast ADD COLUMN IF NOT EXISTS %s", cols[i]);
        sqlite3_exec(db, sql, NULL, NULL, NULL);
    }

    sqlite3_close(db);
    return 0;
}