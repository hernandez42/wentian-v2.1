#include "wentian.h"
#include "api_nowcast.h"

int score_thunderstorm(const wt_nowcast_t *nc, char *alert, int *pos,
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
    else if (nc->pwv_slope > 0.3) score += 5;  /* 极早期PWV上升, 微弱水汽信号也计入 */
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
    /* ⚠ 修复(2026-09-09 R5): 组合信号加成 — PWV急升 + 气压急降 是雷暴前兆的物理标志
     * (水汽辐合+抬升触发), 单独存在可能只是普通阵性, 但组合出现需提早关注。
     * 之前4档独立加分过于保守, 中等强度雷暴前兆(PWV斜率>0.8 + 气压降<-0.5)凑不到26分,
     * 导致0/30雷暴命中。引入组合加成(+12)使中等信号也能触发"关注"级。 */
    if (nc->pwv_slope > PWV_SLOPE_WATCH && nc->dp_3min < -0.5) {
        score += 12;
    }
    /* METAR TS/TSRA 确认加分 */
    if (has_ts) {
        score += 25;
    }

    if (score > 100) score = 100;

    if (score >= 20) {
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
int score_squall_line(int metar_n, const time_t *ts_arr, const double *p_arr,
                              const double *wd_arr, const double *ws_arr,
                              const char *raw_arr, int raw_len,
                              const double *pwv_times, const double *pwv_arr, int pwv_n,
                              double *squall_press, double *squall_wd, double *squall_pwv,
                              char *alert, int *pos) {
    (void)ws_arr; (void)raw_arr; (void)raw_len;
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
