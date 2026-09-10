#ifndef WENTIAN_NOWCAST_H
#define WENTIAN_NOWCAST_H

#include "wentian.h"

/* 辅助函数 (供各天气型模块共用) */
double wind_dir_diff(double d1, double d2);
double pwv_delta(const double *times, const double *pwv, int n, int minutes);

/* 雷暴检测 (api_nowcast_thunder.c) */
int score_thunderstorm(const wt_nowcast_t *nc, char *alert, int *pos,
                       const char *metar_raw);
int score_squall_line(int metar_n, const time_t *ts_arr, const double *p_arr,
                      const double *wd_arr, const double *ws_arr,
                      const char *raw_arr, int raw_len,
                      const double *pwv_times, const double *pwv_arr, int pwv_n,
                      double *squall_press, double *squall_wd, double *squall_pwv,
                      char *alert, int *pos);

/* 锋面检测 (api_nowcast_front.c) */
int score_false_cold(int metar_n, const time_t *ts_arr, const double *t_arr,
                     const double *p_arr, const char *raw_arr, int raw_len,
                     double *fc_temp_drop, double *fc_press_v,
                     char *alert, int *pos);
int score_stationary(int metar_n, const time_t *ts_arr, const double *t_arr,
                     const double *p_arr, const char *raw_arr, int raw_len,
                     double *stat_humid_avg, double *stat_press_var,
                     char *alert, int *pos, double *hum_recent);
void wt_false_cold_note(char *out, int max_len);

/* 风切变检测 (api_nowcast_wind.c) */
int score_wind_shear(int metar_n, const time_t *ts_arr, const double *wd_arr,
                     const double *ws_arr, const char *raw_arr, int raw_len,
                     double *shear_wd_chg, double *shear_wspd_chg,
                     char *alert, int *pos);
void wt_metar_precip_level(const char *raw, char *out_level, int max_len,
                           double *precip_1h_mm);

#define ENSO_MODE  1  /* 0=平时, 1=厄尔尼诺战备 (人工切换, 非自动) */

#if ENSO_MODE
  #define SQUALL_PRESS_RISE    1.5   /* 飑线气压骤升阈值 */
  #define SQUALL_WIND_DIR_CHG  45     /* 飑线风向突变阈值 */
  #define PWV_SLOPE_STORM      2.5   /* 雷暴PWV急升阈值 */
  #define PWV_SLOPE_ALERT      1.5   /* 雷暴预警阈值 */
  #define PWV_SLOPE_WATCH      0.8   /* 雷暴关注阈值 */
  #define PWV_SLOPE_EARLY      0.5   /* 雷暴早期预警 */
  #define SQUALL_PWV_DROP      1.5   /* 飑线PWV骤降阈值 */
  #define PWV_ABS_EXTREME      42.5  /* 雷暴PWV绝对值 */
  #define PWV_ABS_HIGH         38.3  
  #define PWV_ABS_MODERATE     34.0  
  #define FCF_TEMP_DROP        2.0   /* 假冷锋温度骤降阈值 */
  #define STAT_HUMID_HIGH      75     /* 准静止锋持续高湿阈值 */
  #define SHEAR_WIND_DIR_CHG   22     /* 风切变风向突变阈值 */
  #define SHEAR_WIND_SPD_CHG   3.8    /* 风切变风速差阈值 */
#endif

/* 非ENSO默认值 */
#ifndef SQUALL_PRESS_RISE
#define SQUALL_PRESS_RISE    2.0
#endif
#ifndef SQUALL_WIND_DIR_CHG
#define SQUALL_WIND_DIR_CHG  60
#endif
#ifndef SQUALL_PWV_DROP
#define SQUALL_PWV_DROP      2.0
#endif
#ifndef FCF_TEMP_DROP
#define FCF_TEMP_DROP        3.0
#endif
#define FCF_NO_PRECIP_WINDOW 30
#ifndef STAT_HUMID_HIGH
#define STAT_HUMID_HIGH      80
#endif
#define STAT_PRESS_STABLE    1.0
#define STAT_TEMP_STABLE     1.0
#define STAT_PRECIP_CONT     3
#ifndef SHEAR_WIND_DIR_CHG
#define SHEAR_WIND_DIR_CHG   30
#endif
#ifndef SHEAR_WIND_SPD_CHG
#define SHEAR_WIND_SPD_CHG   5.0
#endif

/* 安全snprintf(带pos指针参数) */
#define SAFE_SNPRINTF(fmt, ...) \
    do { \
        int n = snprintf(alert + (*pos), sizeof(alert) - (*pos), fmt, __VA_ARGS__); \
        if (n > 0 && n < (int)(sizeof(alert) - (*pos))) (*pos) += n; \
    } while (0)

#endif /* WENTIAN_NOWCAST_H */
