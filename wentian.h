/* ============================================================
 * wentian.h - 问天气象站 (WenTian Weather Station) 公共头文件
 * ============================================================
 * 项目: 问天 v1.1 — 主人C语言气象站 (BG8SBA / RK3588)
 * 所有者: 主人朱涛 (呼号BG8SBA), 昆明长水机场楼顶
 * 创建: 2026-09-03 (v1.0)
 * 升级: 2026-09-03 (v1.1) — 10项修复+7项新增
 * 锁定: 详见 WENTIAN-LOCK.md, 非授权LLM请勿擅改
 *
 * 架构设计 (主人原则: 1+1>2):
 *   17 开放免费API + 4 本地硬件 + 1 Kalman融合 = 22 维度
 *
 * 模块化设计:
 *   - wentian.c      主调度器 + Daemon (entry: main)
 *   - wentian_api.c  HTTP/JSON 工具 (libcurl + 手写JSON)
 *   - wentian_db.c   SQLite 持久化 (14张表)
 *   - api_openmeteo.c 室外权威 + 空气质量 + 海洋 + 洪水 + 气候
 *   - api_other.c    Aviation + NASA + wttr + USGS + SPA回退
 *   - api_local.c    主人硬件: UNO/GNSS/电离层/SDR
 *   - api_swpc.c     NOAA太空天气 + Kalman融合
 *   - api_rtk.c      GNSS-RTK预留
 *
 * 编译: gcc -O2 -Wall -Wextra -o wentian \
 *           wentian.c wentian_api.c wentian_db.c \
 *           api_openmeteo.c api_other.c api_local.c \
 *           api_swpc.c api_rtk.c \
 *           -lcurl -lsqlite3 -lm
 *
 * 系统要求:
 *   - libcurl 7.x+    (HTTP/HTTPS)
 *   - libsqlite3 3.x+ (持久化)
 *   - libc 2.31+      (strptime/timegm)
 *
 * 重要: 调用本头任何函数前必须先 wt_db_init() 初始化DB
 * ============================================================ */
#ifndef WENTIAN_H
#define WENTIAN_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <pthread.h>

#include "kalman.h"   /* Kalman滤波 (kf1d_t) */

/* ── 常量 ────────────────────────────────────────────────── */
#define WENTIAN_VERSION    "1.0.0"
#define WENTIAN_LAT        25.0820
#define WENTIAN_LON        102.9097
#define WENTIAN_ALT        2115         /* 主人家海拔 (北斗校准) */
#define WENTIAN_DB         "/root/data/wentian.db"
#define WENTIAN_FUSION_DIR "/root/data/fusion"

#define WENTIAN_USER_AGENT "WenTian/1.0 (RK3588; Linux)"

/* ── JSON 字符串提取 ─────────────────────────────────────── */
typedef struct {
    char  *data;
    size_t size;
    size_t cap;
} wt_buf_t;

void wt_buf_init(wt_buf_t *b);
void wt_buf_free(wt_buf_t *b);
int  wt_buf_append(wt_buf_t *b, const char *p, size_t n);

/* ISO8601 → time_t (公共) */
#include <time.h>
time_t parse_iso(const char *s);

/* 简单的JSON字段提取器 (够用就好, 避免依赖json-c) */
char *wt_json_dup(const char *json, const char *key);                  /* "key":"val" */
char *wt_json_dup_arr(const char *json, const char *key, int idx);     /* "key":[...] */
double wt_json_num(const char *json, const char *key, double defval);
int    wt_json_int(const char *json, const char *key, int defval);

/* ── HTTP GET (返回body字符串, 需要free) ─────────────────── */
char *wt_http_get(const char *url, int timeout_sec);
char *wt_http_post(const char *url, const char *body, int timeout_sec);

/* ── 各API模块 ──────────────────────────────────────────── */

/* Open-Meteo 一族 */
typedef struct {
    double  temperature;       /* °C */
    double  humidity;          /* % */
    double  pressure_msl;      /* hPa */
    double  wind_speed;        /* km/h */
    double  wind_dir;          /* deg */
    int     weather_code;      /* WMO code */
    double  precipitation;     /* mm */
    double  cloud_cover;       /* % */
    double  uv_index;
    double  visibility;        /* m */
    char    weather_text[32];
    time_t  fetched_at;
} wt_outdoor_t;

int wt_openmeteo_current(wt_outdoor_t *out);
int wt_openmeteo_air_quality(double *pm25, double *pm10, int *aqi);
int wt_openmeteo_marine(double *wave_height, double *wave_period);
int wt_openmeteo_flood(double *discharge, double *river_level);
int wt_openmeteo_climate(int month, int day, double *temp_mean);
int wt_openmeteo_geocode(const char *name, double *lat, double *lon);

/* AviationWeather (机场实测) */
typedef struct {
    char    icao[8];
    double  temp;          /* °C */
    double  dewpoint;
    int     wind_dir;
    int     wind_speed_kt;
    int     visibility_m;
    double  altim_hpa;
    char    raw[512];
    time_t  obs_time;
} wt_metar_t;

int wt_aviation_metar(const char *icao, wt_metar_t *out);

/* NASA (天文+空间气象) */
typedef struct {
    char    date[16];
    char    title[256];
    char    explanation[2048];
    char    url[512];
} wt_apod_t;

int wt_nasa_apod(wt_apod_t *out);

typedef enum {
    WT_DONKI_CME,    /* 日冕物质抛射 */
    WT_DONKI_FLR,    /* 太阳耀斑 */
    WT_DONKI_GST,    /* 地磁风暴 */
    WT_DONKI_SEP,    /* 高能粒子 */
} wt_donki_type_t;

typedef struct {
    char    id[64];
    char    type[16];     /* "CME", "FLR"... */
    char    classType[16]; /* "M1.9" 等 */
    time_t  begin;
    time_t  peak;
    time_t  end;
    char    note[512];
} wt_donki_event_t;

int wt_nasa_donki_list(wt_donki_type_t type, wt_donki_event_t *out, int max);

/* Sunrise / ISS */
typedef struct {
    time_t  sunrise;
    time_t  sunset;
    time_t  solar_noon;
    double  day_length_sec;
} wt_sun_t;

int wt_sun_times(double lat, double lon, time_t date, wt_sun_t *out);

typedef struct {
    double  lat, lon;
    double  altitude_km;
    double  velocity_kmh;
    time_t  ts;
} wt_iss_t;

int wt_iss_position(wt_iss_t *out);

/* USGS (地震+水文) */
typedef struct {
    double  mag;
    char    place[256];
    time_t  time;
    double  lat, lon;
    double  depth_km;
    char    url[256];
} wt_quake_t;

int wt_usgs_quakes_recent(wt_quake_t *out, int max);

/* wttr.in (冗余) */
int wt_wttr_in(const char *city, wt_outdoor_t *out);

/* NOAA SWPC 太空天气 */
typedef struct {
    time_t  ts;
    double  kp;
    int     kp_index;
    char    kp_text[8];
    int     station_count;
    double  a_running;
} wt_kp_t;
int wt_swpc_kp(wt_kp_t *out);

typedef struct {
    time_t  ts;
    double  flux_sfu;
    int     frequency_mhz;
    double  ninety_day_mean;
} wt_f107_t;
int wt_swpc_f107(wt_f107_t *out);

typedef struct {
    char    date[16];
    char    timestamp[16];
    int     g_scale;
    char    g_text[16];
    int     s_scale;
    char    s_text[16];
    int     s_prob;
    int     r_scale;
    char    r_text[16];
    int     r_minor_prob;
    int     r_major_prob;
} wt_swpc_scale_t;
int wt_swpc_scales(wt_swpc_scale_t *out);

/* Kalman滤波融合 (基于GitHub awesome-kalman-filter) */
typedef struct {
    kf1d_t  kf;             /* 卡尔曼滤波器 (真实类型, 非void*) */
    double  last_value;
    int     n_obs;
} wt_kf_filter_t;

void wt_kf_init(wt_kf_filter_t *f, double x0, double q, double r);
double wt_kf_fuse_pressure(wt_kf_filter_t *f, double p_uno, double p_om, double p_metar);
double wt_kf_fuse_temp(wt_kf_filter_t *f, double t_om, double t_metar);
double wt_kf_smooth_gnss(wt_kf_filter_t *f, double raw);

/* 历史Kalman滤波 (基线学习) */
typedef struct {
    double  baseline;
    double  noise_sigma;
    double  process_sigma;
    int     n_samples;
} wt_kf_baseline_t;

void wt_kf_baseline_init(wt_kf_baseline_t *b);
void wt_kf_baseline_update(wt_kf_baseline_t *b, double sample);
int wt_kf_baseline_anomaly(const wt_kf_baseline_t *b, double sample);

/* ═══ GNSS-RTK 精密定位 ═══════════════════════════════════ */
/* 基于RTKLIB思路: 原始伪距+载波相位 → 厘米级定位 */
typedef struct {
    double  lat;            /* 纬度 (度) */
    double  lon;            /* 经度 (度) */
    double  alt;            /* 椭球高 (m) */
    double  alt_msl;        /* 大地高 (m) */
    int     fix_type;       /* 0=NOFIX, 1=FLOAT, 2=FIXED */
    double  accuracy_cm;    /* 水平精度 (cm) */
    int     base_station_id;/* 基准站ID */
    int     age_sec;        /* 差分龄期 (秒) */
    int     n_sats;         /* 参与RTK解算的卫星数 */
    time_t  ts;
} wt_rtk_t;

/* 从ATGM336H原始NMEA提取RTK-ready观测值 */
int wt_rtk_from_nmea(const char *nmea_line, wt_rtk_t *out);

/* 单点RTK解算 (伪距+载波相位融合) */
int wt_rtk_solve(wt_rtk_t *out, const char *base_correction_url);

/* GNSS坐标Kalman平滑 (抗多路径) — 需3个独立滤波器实例 */
double wt_kf_smooth_lat(wt_kf_filter_t *f, double raw);
double wt_kf_smooth_lon(wt_kf_filter_t *f, double raw);
double wt_kf_smooth_alt(wt_kf_filter_t *f, double raw);

/* 旧版兼容 */
double wt_rtk_smooth(wt_kf_filter_t *f, double raw_lat, double raw_lon, double raw_alt);

/* 主人数据库 (北斗/UNO/电离层/SDR) */
typedef struct {
    double  cabinet_temp;     /* 机柜温度 (不能用于室外预测!) */
    double  cabinet_humid;
    double  cabinet_pressure;
    double  sea_level_pressure;
    double  altitude;
    char    weather[32];
    time_t  ts;
} wt_uno_t;

typedef struct {
    double  lat, lon, alt;
    int     fix;            /* 0=NOFIX, 1=1D, 3=3D, 6=6D */
    int     total_sats;
    int     gps_sats;
    int     bds_sats;       /* 北斗 */
    int     glonass_sats;
    double  pdop;
    double  hdop;
    double  vdop;
    double  altitude_msl;
    int     speed_kts;
    int     heading_deg;
    double  gps_snr;        /* GPS平均SNR(dB) */
    double  bds_snr;        /* 北斗平均SNR(dB) */
    time_t  ts;
} wt_gnss_t;

typedef struct {
    double  s4_gps;
    double  s4_bds;
    double  gps_snr_avg;
    double  bds_snr_avg;
    double  pdop_avg;
    double  vdop_avg;
    double  klob_vert_delay;
    double  klob_slant_delay;
    double  klob_slant_factor;
    double  klob_period_s;
    double  klob_amplitude;
    double  klob_geomag_lat;
    char    activity[16];
    time_t  ts;
} wt_iono_t;

typedef struct {
    char    file[128];
    double  noise_floor_dbm;
    double  peak_freq_mhz;
    double  peak_dbm;
    double  peak_snr;
    char    band[32];
    time_t  ts;
} wt_sdr_t;

int wt_local_uno(wt_uno_t *out);
int wt_local_gnss(wt_gnss_t *out);
int wt_local_iono(wt_iono_t *out);
int wt_local_sdr(wt_sdr_t *out, int max, int *count);
int wt_local_db_init(const char *path);
int wt_local_save_uno(const wt_uno_t *u);
int wt_local_save_gnss(const wt_gnss_t *g);
int wt_local_save_iono(const wt_iono_t *i);
int wt_local_save_sdr(const wt_sdr_t *s);

/* ── 短临雷暴Nowcasting(api_nowcast.c) ────────────────── */
typedef struct {
    time_t  ts;
    double  pwv_slope;
    double  dp_3min;
    double  dt_5min;
    int     score;
    char    level[16];      /* 主风险天气型 */
    char    forecast[64];
    int     pwv_score;
    int     pwv_abs_score;
    int     press_score;
    int     temp_score;
    double  pwv_current;
    double  pwv_15min_ago;
    double  press_current;
    double  press_3min_ago;
    double  temp_current;
    double  temp_5min_ago;
    char    alert_msg[128];
    /* v1.5: 各天气型独立评分 */
    int     thunder_score;
    int     squall_score;
    int     false_cold_score;
    int     stationary_score;
    int     wind_shear_score;
    /* v1.5: 各天气型特征值 */
    double  squall_press_rise;
    double  squall_wd_chg;
    double  squall_pwv_drop;
    double  fc_temp_drop;
    double  fc_press_v;
    double  stat_humid_avg;
    double  stat_press_var;
    double  shear_wd_chg;
    double  shear_wspd_chg;
    /* v2.0: METAR降水强度分级 (GB/T 4.1.15-17) */
    char    precip_intensity[16];  /* 小雨/中雨/大雨/暴雨/无降水 */
    double  precip_1h_mm;           /* 1小时降水量(mm) */
    /* v2.0: GB/T 28594-2012 警报等级字段 */
    char    warning_level[16];     /* 无/关注/预警/强预警 */
    /* v2.0: 假冷锋国标注释 (非GB/T标准术语) */
    char    false_cold_note[128];
} wt_nowcast_t;

int wt_nowcast_compute(wt_nowcast_t *out);
int wt_nowcast_run(void);
int wt_db_save_nowcast(const wt_nowcast_t *nc);
int wt_nowcast_db_init(const char *path);

/* ═══ 问天软件雷达 · 三路相干引擎(api_correlate.c) ════════ */
/* 把SDR射频枪 + GNSS信号枪 + UNO地面枪 当成同一部雷达的三路接收 */
/* 时间对齐 → 特征提取 → 互相关 → 模式匹配 → 天气型概率 */

typedef struct {
    time_t  ts;             /* 时间窗中心(UTC) */
    double  coherence;      /* 三路相干系数[0,1] */
    int     sdr_active;     /* SDR检测到异常信号 */
    int     gnss_anomaly;   /* GNSS检测到异常(PWV/S4/多径) */
    int     uno_pressure;   /* UNO气压变化>0.5hPa/10min */
    int     uno_temp;       /* UNO温度变化>1°C/10min */
    int     matched_pattern;/* 匹配到的天气型(0=未知) */
    double  confidence;     /* 置信度[0,1] */
    int     lead_time_min;  /* 提前量(分钟) */
    char    pattern_name[32]; /* 天气型名称 */
    /* 三路原始特征向量(用于事后分析) */
    double  sdr_features[16];
    double  gnss_features[8];
    double  uno_features[6];
    /* 互相关矩阵 */
    double  corr_sg;        /* SDR-GNSS */
    double  corr_su;        /* SDR-UNO */
    double  corr_gu;        /* GNSS-UNO */
} wt_radar_correl_t;

/* 天气型模式ID(与api_nowcast.c对齐) */
#define WT_PATTERN_UNKNOWN     0
#define WT_PATTERN_THUNDER     1   /* 雷暴 */
#define WT_PATTERN_SQUALL      2   /* 飑线 */
#define WT_PATTERN_FALSE_COLD  3   /* 假冷锋 */
#define WT_PATTERN_STATIONARY  4   /* 准静止锋 */
#define WT_PATTERN_WIND_SHEAR  5   /* 低空风切变 */

int wt_radar_correlate(wt_radar_correl_t *out, time_t ts, int window_min);
int wt_radar_correlate_run(void);           /* daemon入口: 当前时刻 */
int wt_radar_db_init(const char *path);     /* 初始化correl表 */
int wt_db_save_correl(const wt_radar_correl_t *c);

/* ═══ GNSS PWV实时反演(api_pwv.c) ════════════════════════ */
/* C实现Saastamoinen模型, 替代Python离线反演 */
typedef struct {
    time_t  ts;
    double  pwv_mm;
    double  ztd_m;
    double  zhd_m;
    double  zwd_m;
    double  delta_pwv;
    double  temp_c;
    double  humid_pct;
    double  press_hpa;
    int     storm_score;
} wt_pwv_t;

int wt_pwv_compute(wt_pwv_t *out, double last_pwv);
int wt_db_save_pwv(const wt_pwv_t *p);
int wt_pwv_run(void);
int wt_pwv_db_init(const char *path);

/* ═══ GNSS电离层闪烁监测(api_gnss_ion.c) ════════════════ */
/* C实现S4指数+Klobuchar模型, 替代Python gnss_ionosphere.py */
typedef struct {
    time_t  ts;
    double  s4_gps;         /* GPS闪烁指数(<0=无效) */
    double  s4_bds;         /* 北斗闪烁指数 */
    double  avg_gps_snr;    /* GPS平均SNR(dB) */
    double  avg_bds_snr;    /* 北斗平均SNR(dB) */
    double  avg_pdop;       /* 平均PDOP */
    double  klob_vert_delay;/* Klobuchar垂直延迟(s) */
    double  klob_slant_delay;/* Klobuchar斜向延迟(s) */
    double  klob_slant_factor;/* 斜向因子 */
    double  klob_period;    /* 周期(s) */
    const char *activity;   /* 活动级别: NONE/WEAK/MODERATE/STRONG/SEVERE */
    int     valid_gps_samples;/* SNR≥35dB-Hz的有效GPS样本数 */
    int     valid_bds_samples;/* SNR≥35dB-Hz的有效BDS样本数 */
    int     total_samples;    /* 总样本数 */
    char    algorithm_used[16];/* v2.0-Standard / v2.1-LowSNR */
} wt_gnss_ion_t;

int wt_gnss_ionosphere_revert(wt_gnss_ion_t *out, time_t ts);
int wt_gnss_ion_run(void);           /* daemon入口 */
int wt_gnss_ion_db_init(const char *path);

/* ── 多源融合预测 (api_predict.c, 原Python multi_source_predict.py v3.0) ── */
int wt_predict_run(void);           /* daemon入口: 1h/3h/6h 气压/温度/天气/风暴 */

/* ── 自进化自愈自完善引擎 (api_evolve.c) ────────────────── */
int wt_evo_run(void);                /* 评分闭环+自愈检查+阈值微调 */

/* ── 多源融合 S4 引擎 (api_multisrc.c, 主人2026-09-04指示) ── */
int wt_multisrc_run(void);            /* 5源加权 S4 融合 (SDR+UART+OpenMeteo+UNO+ScintPi) */

/* ── 开源专业数据集成 (api_open_data.c) ───────────────────── */
int wt_open_data_run(void);            /* NOAA SWPC + met.no + wttr.in + USGS */

/* ── 全系统自愈修复 (wt_self_repair.c) ────────────────────── */
int wt_full_self_repair(void);          /* APEX ΔG<0 自动修复所有数据源 */

/* ── METAR 多源备选 (wt_metar_fallback.c) ────────────────── */
int wt_metar_fallback_run(wt_metar_t *out);  /* Open-Meteo/NWS/OGIMET 自动降级 */

/* ── 数据库 ──────────────────────────────────────────────── */
int wt_db_init(const char *path);
int wt_db_save_outdoor(const wt_outdoor_t *out);
int wt_db_save_metar(const wt_metar_t *m);
int wt_db_save_quake(const wt_quake_t *q);
int wt_db_save_apod(const wt_apod_t *a);
int wt_db_save_donki(const wt_donki_event_t *e);
int wt_db_save_sun(const wt_sun_t *s, double lat, double lon);
int wt_db_save_iss(const wt_iss_t *iss);
int wt_db_save_air(double pm25, double pm10, int aqi);
int wt_db_save_marine(double wave_height, double wave_period);
int wt_db_save_flood(double discharge, double level);

/* NOAA SWPC 存储 (问天独有!) */
int wt_db_save_kp(const wt_kp_t *kp);
int wt_db_save_f107(const wt_f107_t *f107);
int wt_db_save_scale(const wt_swpc_scale_t *s);

/* Kalman气压融合存储 (问天独有!) */
int wt_db_save_fused_pressure(double fused, double p_uno, double p_om,
                              double p_metar, double sigma);

/* ── 问天主程序 ────────────────────────────────────────── */
int wentian_collect_all(void);    /* 同步抓取所有API */
int wentian_print_report(void);   /* 打印问天报告 */
int wentian_daemon(int interval_sec);  /* 后台循环 */

#endif /* WENTIAN_H */
