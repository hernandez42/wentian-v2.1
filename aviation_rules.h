#ifndef AVIATION_RULES_H
#define AVIATION_RULES_H

/* ============================================================
 * aviation_rules.h v2.0 - CAAC规章阈值 + ZPPP机场数据库
 * ============================================================
 * 基准: CCAR-121, 辅助: CCAR-91, ZPPP机场使用细则
 * 更新: 2026-09-10 加入双跑道+备降场数据库
 * ============================================================ */

/* ── ZPPP 长水机场 ──────────────────────────────────────── */
#define ZPPP_ELEVATION_M     2103
#define ZPPP_RWY_COUNT       2
/* 跑道参数: {名称, 磁航向, 长度m, ILS类型} */
#define ZPPP_RWY_03          30,    3400, "CAT I"
#define ZPPP_RWY_04          40,    4500, "CAT II"
#define ZPPP_RWY_21          210,   3400, "CAT I"
#define ZPPP_RWY_22          220,   4500, "CAT II"

/* ── 进近最低标准(米) ──────────────────────────────────── */
#define VIS_TAKEOFF_CAT1     400
#define VIS_ILS_CAT1         550
#define VIS_ILS_CAT2         300
#define VIS_ILS_CAT3         75
#define VIS_NONPRECISION     800
#define VIS_CIRCLING         1500
#define VIS_VFR              5000

/* ── 侧风/顺风限制(kt, B737典型值) ────────────────────── */
#define XWIND_DRY            15.0
#define XWIND_WET            12.0
#define XWIND_CONTAMINATED   8.0
#define TWIND_MAX            10.0

/* ── 特殊天气标准 ────────────────────────────────────────── */
#define CB_AVOID_NM          20
#define WS_WARN_THRESHOLD    15
#define ICE_TRACE_TEMP_C     2.0
#define ICE_MODERATE_TEMP_C  0.0

/* ── 备降场标准(CCAR-121 §97.63) ────────────────────────── */
#define ALTN_VIS_MIN         800     /* 备降场最低能见度(m) */
#define ALTN_CEIL_MIN        300     /* 备降场最低云高(ft AGL) */

/* ── 跑道状况 ────────────────────────────────────────────── */
#define RWY_DRY              0
#define RWY_WET              1
#define RWY_CONTAMINATED     2

/* ── 风险等级 ────────────────────────────────────────────── */
#define RISK_LOW             0       /* 绿色: 正常运行 */
#define RISK_CAUTION         1       /* 黄色: 关注 */
#define RISK_WARNING         2       /* 橙色: 需决策 */
#define RISK_PROHIBITED      3       /* 红色: 禁止 */

/* ── 备降场数据库 ────────────────────────────────────────── */
#define MAX_ALTN 13

typedef struct {
    const char *icao;       /* ICAO代码 */
    const char *name;       /* 机场名称 */
    double      lat;        /* 纬度 */
    double      lon;        /* 经度 */
    int         elevation;  /* 海拔(米) */
    int         rwy_length; /* 跑道长度(米) */
    const char *ils_type;   /* ILS类型 */
    int         dist_nm;    /* 距ZPPP距离(NM) */
    int         is_yunnan;  /* 是否云南省内 */
} altn_airport_t;

static const altn_airport_t ALTN_AIRPORTS[MAX_ALTN] = {
    /* 云南省内 */
    {"ZPLJ", "丽江三义",      26.679, 100.246, 2240, 3000, "CAT I",  200, 1},
    {"ZPDQ", "迪庆香格里拉",  27.804, 99.674, 3280, 3600, "CAT I",  280, 1},
    {"ZPBS", "保山云瑞",      25.058, 99.177, 1664, 2400, "LOC",    310, 1},
    {"ZPLC", "临沧博尚",      23.738, 100.025, 1898, 2400, "LOC",    300, 1},
    {"ZPJH", "西双版纳嘎洒",  21.974, 100.760, 553,  2400, "CAT I",  260, 1},
    {"ZPPE", "普洱思茅",      22.793, 100.749, 1303, 2500, "LOC",    240, 1},
    {"ZPZT", "昭通",          27.357, 103.756, 1936, 2700, "LOC",    230, 1},
    {"ZPMS", "芒市德宏",      24.400, 98.500,  876,  2600, "CAT I",  290, 1},
    {"ZPWS", "文山普者黑",    23.567, 104.333, 1590, 2400, "CAT I",  220, 1},
    /* 省外备降 */
    {"ZUGY", "贵阳龙洞堡",    26.538, 106.801, 1139, 3200, "CAT II", 300, 0},
    {"ZUUU", "成都双流",      30.578, 103.947, 495,  3600, "CAT II", 350, 0},
    {"ZUTF", "成都天府",      30.320, 104.441, 440,  4000, "CAT III",360, 0},
    {"ZULS", "昆明长水备",    0,0,0,0,"",0,0},  /* 占位 */
};

#endif /* AVIATION_RULES_H */
