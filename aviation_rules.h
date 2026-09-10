#ifndef AVIATION_RULES_H
#define AVIATION_RULES_H

/* ============================================================
 * aviation_rules.h - CAAC民航规章阈值 + ZPPP机场标准
 * ============================================================
 * 基准: CCAR-121, 辅助: CCAR-91, ZPPP机场使用细则
 * 
 * 所有阈值均为可配置宏, 随规章更新可重新编译
 * ============================================================ */

/* ── ZPPP跑道参数 ────────────────────────────────────────── */
#define ZPPP_RWY_ACTIVE      "21"          /* 默认运行跑道 */
#define ZPPP_RWY_LENGTH_M    4000           /* 长水跑道长度 */
#define ZPPP_ELEVATION_M     2103           /* 标高 */

/* ── 进近最低标准 (CCAR-121 §97, ZPPP仪表进近图) ───────── */
/* RVR/能见度最低要求(米) */
#define VIS_TAKEOFF_CAT1     400    /* CAT I 起飞 */
#define VIS_ILS_CAT1         550    /* ILS CAT I 进近 */
#define VIS_ILS_CAT2         300    /* ILS CAT II 进近 */
#define VIS_ILS_CAT3         75     /* ILS CAT III 进近 */
#define VIS_NONPRECISION     800    /* 非精密进近(VOR/NDB) */
#define VIS_CIRCLING         1500   /* 盘旋进近 */
#define VIS_VFR              5000   /* 目视飞行 */

/* ── 侧风限制(kt, B737典型值, 各航司手册为准) ──────────── */
#define XWIND_DRY            15.0   /* 干跑道最大侧风 */
#define XWIND_WET            12.0   /* 湿跑道最大侧风 */
#define XWIND_CONTAMINATED   8.0    /* 污染跑道/积冰 */
#define TWIND_MAX            10.0   /* 最大顺风分量 */

/* ── 特殊天气标准 ──────────────────────────────────────────── */
#define CB_AVOID_NM          20     /* 雷暴绕飞距离(NM) */
#define CB_AVOID_OVERHEAD_NM 10     /* 机场正上方雷暴停止作业距离 */
#define WS_WARN_THRESHOLD    15     /* 风切变预警: 空速变化>15kt */
#define ICE_TRACE_TEMP_C     2.0    /* 积冰可能温度上限 */
#define ICE_MODERATE_TEMP_C  0.0    /* 中等积冰典型温度 */
#define HAIL_WARN_SIZE_MM    5      /* 冰雹预警直径 */

/* ── 备降场标准 (CCAR-121 §97.63) ────────────────────────── */
#define ALTN_VIS_REQUIRED    800    /* 备降场能见度要求(m) */
#define ALTN_CEIL_REQUIRED   300    /* 备降场云高要求(ft AGL) */

/* ── 跑道状况 ────────────────────────────────────────────── */
/* 基于METAR和温度推算的跑道状况代码 */
#define RWY_DRY              0
#define RWY_WET              1
#define RWY_CONTAMINATED     2    /* 雪/冰/积水 */

/* ── 风险等级 ────────────────────────────────────────────── */
#define RISK_LOW             0     /* 绿色: 正常运行 */
#define RISK_CAUTION         1     /* 黄色: 关注, 需确认 */
#define RISK_WARNING         2     /* 橙色: 高于标准, 需决策 */
#define RISK_PROHIBITED      3     /* 红色: 禁止运行 */

#endif /* AVIATION_RULES_H */
