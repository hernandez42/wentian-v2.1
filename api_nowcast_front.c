#include "wentian.h"
#include "api_nowcast.h"

int score_false_cold(int metar_n, /* unused */ const time_t *ts_arr, const double *t_arr,
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
        SAFE_SNPRINTF(alert, NOWCAST_ALERT_SIZE, "❄假冷锋(降温%.1f°C %s降水 V型气压%.1fhPa)",
                      temp_drop, has_precip?"有":"无", press_v);
    }
    return score;
}

/* ── 准静止锋评分 ─────────────────────────────────────── */
/* 特征: 持续高湿 + 温度气压稳定 + 连续降水 */
int score_stationary(int metar_n, const time_t *ts_arr, const double *t_arr,
                             const double *p_arr, const char *raw_arr, int raw_len,
                             double *stat_humid_avg, double *stat_press_var,
                             char *alert, int *pos,
                             /* 需从外部传入湿度数据 */
                             double hum_recent[6]) {
    (void)ts_arr; (void)raw_arr; (void)raw_len;
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
        SAFE_SNPRINTF(alert, NOWCAST_ALERT_SIZE, "🌫准静止锋(湿度%.0f%% 变温%.1f°C 变压%.1fhPa 降水%d次)",
                      hum_avg, t_range, p_range, precip_count);
    }
    return score;
}

