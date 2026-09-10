#include "wentian.h"
#include "api_nowcast.h"

int score_wind_shear(int metar_n, const time_t *ts_arr, const double *wd_arr,
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
void wt_metar_precip_level(const char *raw, char *out_level, int max_len,
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
void wt_false_cold_note(char *out, int max_len) {
    snprintf(out, max_len,
        "非GB/T标准术语; 指无降水伴随的温度骤降现象(类冷锋特征), "
        "GB/T对应: 冷锋4.2.7 + 温度骤降现象");
}

