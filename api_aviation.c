/* ============================================================
 * api_aviation.c - 民航运行风险评估 (CCAR-121基准)
 * ============================================================
 * 功能: 融合多源气象数据 + CAAC规章阈值
 *       输出结构化签派/飞行/维修风险评估文本报告
 *
 * 数据源: met.no室外实况 + 5模型融合预报 + METAR
 * 基准: CCAR-121, CCAR-91, ZPPP机场使用细则
 *
 * 输出: /root/data/fusion/aviation_report.txt
 * ============================================================ */
#include "wentian.h"
#include "aviation_rules.h"
#include <sqlite3.h>
#include <math.h>
#include <string.h>

#define AVIATION_REPORT "/root/data/fusion/aviation_report.txt"

/* ── 从outdoor表读最新实况 ─────────────────────────────── */
static int read_outdoor(double *t, double *h, double *p, double *ws, double *wd,
                         double *vis, double *cloud, double *precip) {
    sqlite3 *db;
    if (sqlite3_open(WENTIAN_DB, &db) != SQLITE_OK) return -1;
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(db,
        "SELECT temp,humid,pressure,wind_s,wind_d,vis,cloud,precip FROM outdoor "
        "ORDER BY ts DESC LIMIT 1", -1, &st, NULL) != SQLITE_OK) {
        sqlite3_close(db); return -1;
    }
    if (sqlite3_step(st) != SQLITE_ROW) {
        sqlite3_finalize(st); sqlite3_close(db); return -1;
    }
    *t     = sqlite3_column_double(st, 0);
    *h     = sqlite3_column_double(st, 1);
    *p     = sqlite3_column_double(st, 2);
    *ws    = sqlite3_column_double(st, 3);
    *wd    = sqlite3_column_double(st, 4);
    *vis   = sqlite3_column_double(st, 5);
    *cloud = sqlite3_column_double(st, 6);
    *precip = sqlite3_column_double(st, 7);
    sqlite3_finalize(st);
    sqlite3_close(db);
    return 0;
}

/* ── 从METAR表读最新报文 ───────────────────────────────── */
static int read_metar(char *raw, int max_raw, double *altim, double *wind_dir,
                       double *wind_spd_kt, double *rvr) {
    sqlite3 *db;
    if (sqlite3_open(WENTIAN_DB, &db) != SQLITE_OK) return -1;
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(db,
        "SELECT raw,altim,wind_dir,wind_spd_kt,rvr FROM metar "
        "WHERE raw NOT LIKE 'SYNTHETIC%%' ORDER BY ts DESC LIMIT 1",
        -1, &st, NULL) != SQLITE_OK) {
        sqlite3_close(db); return -1;
    }
    if (sqlite3_step(st) != SQLITE_ROW) {
        sqlite3_finalize(st); sqlite3_close(db); return -1;
    }
    const char *r = (const char*)sqlite3_column_text(st, 0);
    if (r) strncpy(raw, r, max_raw-1);
    *altim = sqlite3_column_double(st, 1);
    *wind_dir = sqlite3_column_double(st, 2);
    *wind_spd_kt = sqlite3_column_double(st, 3);
    *rvr = sqlite3_column_double(st, 4);
    sqlite3_finalize(st);
    sqlite3_close(db);
    return 0;
}

/* ── 风向风速→侧风/顺风分量 ────────────────────────────── */
static void calc_wind_components(double rwy_dir, double wind_dir, double wind_spd,
                                  double *xwind, double *twind) {
    double diff = wind_dir - rwy_dir;
    double rad = diff * M_PI / 180.0;
    *xwind = wind_spd * sin(rad);
    *twind = wind_spd * cos(rad);
    if (*xwind < 0) *xwind = -*xwind;  /* 侧风取绝对值 */
}

/* ── 估算能见度(当无RVR时, 从湿度/降水/云量推算) ───────── */
static double estimate_vis(double rh, double precip, double cloud_cover,
                            double metar_vis) {
    if (metar_vis > 100) return metar_vis;  /* METAR有真实能见度 */
    /* 无METAR时经验估算 */
    if (precip > 5) return 2000;     /* 大雨 */
    if (precip > 1) return 5000;     /* 中雨 */
    if (rh > 95 && cloud_cover > 80) return 3000;  /* 高湿+阴→雾或低云 */
    if (rh > 85 && cloud_cover > 60) return 6000;
    return 10000;  /* 默认10km */
}

/* ── 判断跑道状况 ────────────────────────────────────────── */
static int runway_condition(double temp, double precip, double rh) {
    if (temp < 0 && (precip > 0 || rh > 85)) return RWY_CONTAMINATED;  /* 积冰可能 */
    if (precip > 0.1) return RWY_WET;
    return RWY_DRY;
}

/* ── 生成风险评估报告 ───────────────────────────────────── */
int wt_aviation_assess(void) {
    /* 读数据 */
    double t = NAN, h = NAN, p = NAN, ws = NAN, wd = NAN;
    double vis = NAN, cloud = NAN, precip = NAN;
    if (read_outdoor(&t, &h, &p, &ws, &wd, &vis, &cloud, &precip) != 0) {
        printf("  ⚠ [航空] 室外数据不可用\n");
        return -1;
    }

    char metar_raw[256] = {0};
    double altim = NAN, metar_wd = NAN, metar_ws = NAN, rvr = NAN;
    read_metar(metar_raw, sizeof(metar_raw), &altim, &metar_wd, &metar_ws, &rvr);

    /* 风速: 优先METAR, 其次outdoor */
    double wind_spd_kt = (metar_ws > 0) ? metar_ws : (ws * 1.944);  /* m/s→kt */
    double wind_dir = (metar_wd > 0) ? metar_wd : wd;

    /* 能见度 */
    double vis_m = estimate_vis(h, precip, cloud, rvr > 0 ? rvr : vis);

    /* 侧风/顺风分量 (ZPPP 21号跑道方向=210°) */
    double xwind, twind;
    calc_wind_components(210.0, wind_dir, wind_spd_kt, &xwind, &twind);

    /* 跑道状况 */
    int rwy = runway_condition(t, precip, h);
    double xwind_limit = (rwy == RWY_DRY) ? XWIND_DRY :
                         (rwy == RWY_WET) ? XWIND_WET : XWIND_CONTAMINATED;

    /* ── 评估各项 ── */
    int risk_takeoff = RISK_LOW;
    int risk_approach = RISK_LOW;
    int risk_xwind = RISK_LOW;
    int risk_cb = RISK_LOW;
    int risk_ice = RISK_LOW;

    /* 起飞标准 */
    if (vis_m < VIS_TAKEOFF_CAT1) risk_takeoff = RISK_PROHIBITED;
    else if (vis_m < VIS_TAKEOFF_CAT1 * 1.5) risk_takeoff = RISK_WARNING;

    /* 进近标准 */
    if (vis_m < VIS_ILS_CAT1) risk_approach = RISK_PROHIBITED;
    else if (vis_m < VIS_ILS_CAT1 * 1.3) risk_approach = RISK_WARNING;
    else if (vis_m < VIS_NONPRECISION) risk_approach = RISK_CAUTION;

    /* 侧风 */
    if (xwind >= xwind_limit) risk_xwind = RISK_PROHIBITED;
    else if (xwind >= xwind_limit * 0.85) risk_xwind = RISK_WARNING;
    else if (xwind >= xwind_limit * 0.7) risk_xwind = RISK_CAUTION;

    /* 雷暴判定: 从METAR或降水强度 */
    int has_cb = 0;
    if (strstr(metar_raw, "TS") || strstr(metar_raw, "CB")) has_cb = 1;
    if (precip > 5 && cloud > 80) has_cb = 1;  /* 推测可能雷暴 */
    if (has_cb) risk_cb = RISK_PROHIBITED;

    /* 积冰: 温度+湿度+降水 */
    if (t < ICE_MODERATE_TEMP_C && precip > 0) risk_ice = RISK_WARNING;
    else if (t < ICE_TRACE_TEMP_C && h > 80) risk_ice = RISK_CAUTION;

    /* ── 生成报告 ── */
    char buf[4096];
    int pos = 0;

    pos += snprintf(buf + pos, sizeof(buf) - pos,
        "════════════════════════════════════════════════════\n");
    pos += snprintf(buf + pos, sizeof(buf) - pos,
        " ZPPP 长水机场 飞行运行风险评估\n");
    pos += snprintf(buf + pos, sizeof(buf) - pos,
        " 基准: CCAR-121 | ZPPP 21号跑道\n");
    pos += snprintf(buf + pos, sizeof(buf) - pos,
        " 时间: %s\n", "(daemon cycle)");
    pos += snprintf(buf + pos, sizeof(buf) - pos,
        "════════════════════════════════════════════════════\n");
    pos += snprintf(buf + pos, sizeof(buf) - pos,
        "\n── 当前天气 ──\n");
    pos += snprintf(buf + pos, sizeof(buf) - pos,
        "   温度: %.1f°C | 湿度: %.0f%% | 气压: %.0fhPa\n", t, h, p);
    pos += snprintf(buf + pos, sizeof(buf) - pos,
        "   风向: %.0f° | 风速: %.0fkt | 侧风: %.0fkt | 顺风: %.0fkt\n",
        wind_dir, wind_spd_kt, xwind, twind);
    pos += snprintf(buf + pos, sizeof(buf) - pos,
        "   能见度: %.0fm | 云量: %.0f%% | 降水: %.1fmm\n", vis_m, cloud, precip);

    pos += snprintf(buf + pos, sizeof(buf) - pos,
        "\n── 签派评估 ──\n");
    const char *risk_name[] = {"✅ 正常", "🟡 关注", "🟠 警告", "🔴 禁止"};
    pos += snprintf(buf + pos, sizeof(buf) - pos,
        "   起飞标准: %s (能见度%.0fm/要求%.0fm)\n",
        risk_name[risk_takeoff], vis_m, (double)VIS_TAKEOFF_CAT1);
    pos += snprintf(buf + pos, sizeof(buf) - pos,
        "   侧风限制: %s (%.0fkt/限制%.0fkt/%s跑道)\n",
        risk_name[risk_xwind], xwind, xwind_limit,
        rwy == RWY_DRY ? "干" : rwy == RWY_WET ? "湿" : "污染");
    pos += snprintf(buf + pos, sizeof(buf) - pos,
        "   备降场: 能见度要求≥%.0fm\n", (double)ALTN_VIS_REQUIRED);

    pos += snprintf(buf + pos, sizeof(buf) - pos,
        "\n── 飞行评估 ──\n");
    pos += snprintf(buf + pos, sizeof(buf) - pos,
        "   进近标准(ILS): %s (能见度%.0fm/CAT I最低%.0fm)\n",
        risk_name[risk_approach], vis_m, (double)VIS_ILS_CAT1);
    pos += snprintf(buf + pos, sizeof(buf) - pos,
        "   雷暴风险: %s\n", risk_name[risk_cb]);
    pos += snprintf(buf + pos, sizeof(buf) - pos,
        "   积冰风险: %s (温度%.1f°C/湿度%.0f%%)\n",
        risk_name[risk_ice], t, h);

    pos += snprintf(buf + pos, sizeof(buf) - pos,
        "\n── 维修评估 ──\n");
    pos += snprintf(buf + pos, sizeof(buf) - pos,
        "   跑道状况: %s\n",
        rwy == RWY_DRY ? "干跑道" : rwy == RWY_WET ? "湿跑道" : "⚠ 污染/积冰可能");
    pos += snprintf(buf + pos, sizeof(buf) - pos,
        "   雷击风险: %s\n",
        has_cb ? "🟠 有雷暴活动, 注意停机位安全" : "✅ 无雷暴活动");

    pos += snprintf(buf + pos, sizeof(buf) - pos,
        "\n── 数据源 ──\n");
    pos += snprintf(buf + pos, sizeof(buf) - pos,
        "   气象实况: met.no/Open-Meteo (C引擎)\n");
    pos += snprintf(buf + pos, sizeof(buf) - pos,
        "   METAR: %s\n", metar_raw[0] ? metar_raw : "暂无");
    pos += snprintf(buf + pos, sizeof(buf) - pos,
        "   能见度: %s\n", rvr > 0 ? "RVR实测" : "模型估算");

    pos += snprintf(buf + pos, sizeof(buf) - pos,
        "\n════════════════════════════════════════════════════\n");
    pos += snprintf(buf + pos, sizeof(buf) - pos,
        " 评估: 非正式参考, 最终决策以签派/机长判断为准\n");
    pos += snprintf(buf + pos, sizeof(buf) - pos,
        "════════════════════════════════════════════════════\n");

    /* 写入文件 */
    FILE *f = fopen(AVIATION_REPORT, "w");
    if (f) {
        fputs(buf, f);
        fclose(f);
    }

    printf("  ✅ 运行风险评估 | 起飞%s 进近%s 侧风%s 雷暴%s 积冰%s\n",
           risk_name[risk_takeoff], risk_name[risk_approach],
           risk_name[risk_xwind], risk_name[risk_cb], risk_name[risk_ice]);
    return 0;
}
