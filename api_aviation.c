/* ============================================================
 * api_aviation.c v2.0 - 民航运行风险评估
 * ============================================================
 * 功能: 双跑道评估 + 云南省内备降场 + 贵阳/成都
 * 基准: CCAR-121, CCAR-91, ZPPP机场使用细则
 * ============================================================ */
#include "wentian.h"
#include "aviation_rules.h"
#include <sqlite3.h>
#include <math.h>
#include <string.h>

#define AVIATION_REPORT "/root/data/fusion/aviation_report.txt"

/* ── 工具: 风向差计算 ──────────────────────────────────── */
static double wind_diff_abs(double wdir, double rwy_hdg) {
    double d = fabs(wdir - rwy_hdg);
    if (d > 180.0) d = 360.0 - d;
    return d;
}

/* ── 侧风/顺风分量 ────────────────────────────────────── */
static void calc_wind_comp(double wind_dir, double wind_spd_kt,
                            double rwy_hdg, double *xw, double *tw) {
    double diff = wind_dir - rwy_hdg;
    double rad = diff * M_PI / 180.0;
    *xw = fabs(wind_spd_kt * sin(rad));
    *tw = wind_spd_kt * cos(rad);
}

/* ── 能见度估算(无RVR时) ────────────────────────────────── */
static double estimate_vis(double rh, double precip, double cloud_cov,
                            double metar_vis) {
    if (metar_vis > 100) return metar_vis;
    if (precip > 5) return 2000;
    if (precip > 1) return 5000;
    if (rh > 95 && cloud_cov > 80) return 3000;
    if (rh > 85 && cloud_cov > 60) return 6000;
    return 10000;
}

/* ── 读METAR(通用, 指定ICAO) ────────────────────────────── */
typedef struct {
    double temp; double dew; double altim;
    double wind_dir; double wind_spd_kt;
    double vis_m; char raw[256];
    int valid;
} metar_data_t;

static metar_data_t fetch_metar_icao(const char *icao) {
    metar_data_t m = {0};
    sqlite3 *db;
    if (sqlite3_open(WENTIAN_DB, &db) != SQLITE_OK) return m;
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(db,
        "SELECT temp,altim,wind_dir,wind_spd_kt,vis,raw FROM metar "
        "WHERE icao=? AND raw NOT LIKE 'SYNTHETIC%%' "
        "ORDER BY ts DESC LIMIT 1", -1, &st, NULL) != SQLITE_OK) {
        sqlite3_close(db); return m;
    }
    sqlite3_bind_text(st, 1, icao, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(st) == SQLITE_ROW) {
        m.temp = sqlite3_column_double(st, 0);
        m.altim = sqlite3_column_double(st, 1);
        m.wind_dir = sqlite3_column_double(st, 2);
        m.wind_spd_kt = sqlite3_column_double(st, 3);
        m.vis_m = sqlite3_column_double(st, 4);
        const char *r = (const char*)sqlite3_column_text(st, 5);
        if (r) strncpy(m.raw, r, sizeof(m.raw)-1);
        m.valid = 1;
    }
    sqlite3_finalize(st);
    sqlite3_close(db);
    return m;
}

/* ── 读ZPPP实况 ────────────────────────────────────────── */
static int read_outdoor(double *t, double *h, double *p,
                         double *ws, double *wd, double *vis,
                         double *cloud, double *precip) {
    sqlite3 *db;
    if (sqlite3_open(WENTIAN_DB, &db) != SQLITE_OK) return -1;
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(db,
        "SELECT temp,humid,pressure,wind_s,wind_d,vis,cloud,precip "
        "FROM outdoor ORDER BY ts DESC LIMIT 1", -1, &st, NULL) != SQLITE_OK)
        { sqlite3_close(db); return -1; }
    if (sqlite3_step(st) != SQLITE_ROW)
        { sqlite3_finalize(st); sqlite3_close(db); return -1; }
    *t = sqlite3_column_double(st, 0);
    *h = sqlite3_column_double(st, 1);
    *p = sqlite3_column_double(st, 2);
    *ws = sqlite3_column_double(st, 3);
    *wd = sqlite3_column_double(st, 4);
    *vis = sqlite3_column_double(st, 5);
    *cloud = sqlite3_column_double(st, 6);
    *precip = sqlite3_column_double(st, 7);
    sqlite3_finalize(st); sqlite3_close(db);
    return 0;
}

/* ── 跑道状况判定 ──────────────────────────────────────── */
static int rwy_condition(double temp, double precip, double rh) {
    if (temp < 0 && (precip > 0 || rh > 85)) return RWY_CONTAMINATED;
    if (precip > 0.1) return RWY_WET;
    return RWY_DRY;
}

/* ── 单跑道评估 ────────────────────────────────────────── */
typedef struct {
    const char *name;
    int heading;
    double xwind;
    double twind;
    int risk_takeoff;
    int risk_approach;
    int risk_xwind;
} rwy_assess_t;

static rwy_assess_t assess_runway(int hdg, int len, const char *ils,
                                    double wdir, double wspd_kt, double vis_m,
                                    int rwy_cond) {
    rwy_assess_t r = {.heading = hdg, .name = ""};
    double xw, tw;
    calc_wind_comp(wdir, wspd_kt, (double)hdg, &xw, &tw);
    r.xwind = xw; r.twind = tw;

    double xw_limit = (rwy_cond == RWY_DRY) ? XWIND_DRY :
                      (rwy_cond == RWY_WET) ? XWIND_WET : XWIND_CONTAMINATED;

    if (xw >= xw_limit) r.risk_xwind = RISK_PROHIBITED;
    else if (xw >= xw_limit * 0.85) r.risk_xwind = RISK_WARNING;
    else if (xw >= xw_limit * 0.7) r.risk_xwind = RISK_CAUTION;

    if (vis_m < VIS_TAKEOFF_CAT1) r.risk_takeoff = RISK_PROHIBITED;
    else if (vis_m < VIS_TAKEOFF_CAT1 * 1.5) r.risk_takeoff = RISK_WARNING;

    /* 进近: 根据ILS类型 */
    double ils_min = VIS_ILS_CAT1;
    if (strstr(ils, "CAT II")) ils_min = VIS_ILS_CAT2;
    else if (strstr(ils, "CAT III")) ils_min = VIS_ILS_CAT3;
    else if (strstr(ils, "LOC") || strstr(ils, "VOR")) ils_min = VIS_NONPRECISION;

    if (vis_m < ils_min) r.risk_approach = RISK_PROHIBITED;
    else if (vis_m < ils_min * 1.3) r.risk_approach = RISK_WARNING;
    else if (vis_m < VIS_NONPRECISION) r.risk_approach = RISK_CAUTION;

    return r;
}

/* ── 备降场评估 ────────────────────────────────────────── */
typedef struct {
    const char *icao;
    const char *name;
    int dist_nm;
    int is_yunnan;
    double vis_m;
    double altim;
    double wind_spd_kt;
    int risk_alternate;
    int metar_ok;
    char raw[64];
} altn_assess_t;

static altn_assess_t assess_alternate(const altn_airport_t *ap) {
    altn_assess_t a;
    memset(&a, 0, sizeof(a));
    a.icao = ap->icao;
    a.name = ap->name;
    a.dist_nm = ap->dist_nm;
    a.is_yunnan = ap->is_yunnan;

    metar_data_t m = fetch_metar_icao(ap->icao);
    if (!m.valid) {
        a.risk_alternate = RISK_CAUTION;  /* 无METAR → 不确定 */
        snprintf(a.raw, sizeof(a.raw), "无METAR");
        return a;
    }
    a.metar_ok = 1;
    a.vis_m = m.vis_m;
    a.altim = m.altim;
    a.wind_spd_kt = m.wind_spd_kt;
    snprintf(a.raw, sizeof(a.raw), "%.0fm/风%.0fkt", m.vis_m, m.wind_spd_kt);

    if (m.vis_m < ALTN_VIS_MIN) a.risk_alternate = RISK_PROHIBITED;
    else if (m.vis_m < ALTN_VIS_MIN * 1.5) a.risk_alternate = RISK_WARNING;
    else a.risk_alternate = RISK_LOW;

    return a;
}

/* ── 主入口 ──────────────────────────────────────────────── */
int wt_aviation_assess(void) {
    double t = NAN, h = NAN, p = NAN, ws = NAN, wd = NAN;
    double vis = NAN, cloud = NAN, precip = NAN;
    if (read_outdoor(&t, &h, &p, &ws, &wd, &vis, &cloud, &precip) != 0) {
        printf("  ⚠ [航空] 实况数据不可用\n");
        return -1;
    }

    /* ZPPP METAR */
    metar_data_t zppp = fetch_metar_icao("ZPPP");
    double wspd_kt = zppp.valid ? zppp.wind_spd_kt : (ws * 1.944);
    double wdir = zppp.valid ? zppp.wind_dir : wd;
    double vis_m = estimate_vis(h, precip, cloud, zppp.vis_m > 0 ? zppp.vis_m : vis);
    int rwy_cond = rwy_condition(t, precip, h);

    /* 双跑道评估 */
    rwy_assess_t r03 = assess_runway(30, 3400, "CAT I", wdir, wspd_kt, vis_m, rwy_cond);
    r03.name = "03";
    rwy_assess_t r04 = assess_runway(40, 4500, "CAT II", wdir, wspd_kt, vis_m, rwy_cond);
    r04.name = "04";
    rwy_assess_t r21 = assess_runway(210, 3400, "CAT I", wdir, wspd_kt, vis_m, rwy_cond);
    r21.name = "21";
    rwy_assess_t r22 = assess_runway(220, 4500, "CAT II", wdir, wspd_kt, vis_m, rwy_cond);
    r22.name = "22";

    /* 最佳跑道: 侧风最小的 */
    rwy_assess_t *best = &r21;
    rwy_assess_t *all_rwys[] = {&r03, &r04, &r21, &r22};
    for (int i = 0; i < 4; i++) {
        if (all_rwys[i]->risk_xwind < best->risk_xwind) best = all_rwys[i];
        else if (all_rwys[i]->risk_xwind == best->risk_xwind &&
                 all_rwys[i]->xwind < best->xwind) best = all_rwys[i];
    }

    /* 备降场评估 */
    altn_assess_t altn[MAX_ALTN];
    int altn_ok = 0, altn_bad = 0;
    for (int i = 0; i < MAX_ALTN; i++) {
        if (!ALTN_AIRPORTS[i].icao || !ALTN_AIRPORTS[i].icao[0]) continue;
        altn[i] = assess_alternate(&ALTN_AIRPORTS[i]);
        if (altn[i].risk_alternate >= RISK_WARNING) altn_bad++;
        else altn_ok++;
    }

    /* ── 生成报告 ── */
    char buf[8192];
    int pos = 0;
    const char *rn[] = {"✅ 正常", "🟡 关注", "🟠 警告", "🔴 禁止"};
    const char *rc_str[] = {"干跑道", "湿跑道", "⚠ 污染/积冰"};

    pos += snprintf(buf+pos, sizeof(buf)-pos,
        "════════════════════════════════════════════════════\n");
    pos += snprintf(buf+pos, sizeof(buf)-pos,
        " ZPPP 长水机场 飞行运行风险评估  v2.0\n");
    pos += snprintf(buf+pos, sizeof(buf)-pos,
        " 基准: CCAR-121 | 海拔: %dm\n", ZPPP_ELEVATION_M);
    pos += snprintf(buf+pos, sizeof(buf)-pos,
        "════════════════════════════════════════════════════\n");

    pos += snprintf(buf+pos, sizeof(buf)-pos,
        "\n── 当前天气 ──\n");
    pos += snprintf(buf+pos, sizeof(buf)-pos,
        "   温度: %.1f°C | 湿度: %.0f%% | 气压: %.0fhPa\n", t, h, p);
    pos += snprintf(buf+pos, sizeof(buf)-pos,
        "   风向: %.0f° | 风速: %.0fkt | 能见度: %.0fm\n", wdir, wspd_kt, vis_m);
    pos += snprintf(buf+pos, sizeof(buf)-pos,
        "   ZPPP METAR: %s\n", zppp.valid ? zppp.raw : "暂无实时报文");

    pos += snprintf(buf+pos, sizeof(buf)-pos,
        "\n── 跑道评估 ──\n");
    for (int i = 0; i < 4; i++) {
        rwy_assess_t *rw = all_rwys[i];
        int star = (rw == best) ? 1 : 0;
        pos += snprintf(buf+pos, sizeof(buf)-pos,
            "   %s%02d号 [%s] 侧风%.0fkt/顺风%.0fkt 起降%s 进近%s\n",
            star ? "👉" : "  ", rw->heading, rw->name,
            rw->xwind, rw->twind, rn[rw->risk_takeoff], rn[rw->risk_approach]);
    }
    pos += snprintf(buf+pos, sizeof(buf)-pos,
        "   推荐跑道: %02d号 (侧风最小)\n", best->heading);

    pos += snprintf(buf+pos, sizeof(buf)-pos,
        "\n── 签派评估 ──\n");
    pos += snprintf(buf+pos, sizeof(buf)-pos,
        "   侧风限制: %s (%.0fkt/限制%.0fkt/%s)\n",
        rn[best->risk_xwind], best->xwind,
        rwy_cond==RWY_DRY?XWIND_DRY:rwy_cond==RWY_WET?XWIND_WET:XWIND_CONTAMINATED,
        rc_str[rwy_cond]);
    pos += snprintf(buf+pos, sizeof(buf)-pos,
        "   顺风限制: %s (%.0fkt/最大%.0fkt)\n",
        fabs(best->twind) > TWIND_MAX ? "⚠ 超标" : "✅ 正常",
        fabs(best->twind), TWIND_MAX);

    pos += snprintf(buf+pos, sizeof(buf)-pos,
        "\n── 备降场评估 (CCAR-121 标准≥%.0fm能见度) ──\n", (double)ALTN_VIS_MIN);
    pos += snprintf(buf+pos, sizeof(buf)-pos,
        "   🟢 可用: %d | 🔴 不可用: %d\n", altn_ok, altn_bad);
    pos += snprintf(buf+pos, sizeof(buf)-pos,
        "   ── 云南省内 ──\n");
    for (int i = 0; i < MAX_ALTN; i++) {
        if (!ALTN_AIRPORTS[i].icao || !ALTN_AIRPORTS[i].icao[0]) continue;
        if (!ALTN_AIRPORTS[i].is_yunnan) continue;
        pos += snprintf(buf+pos, sizeof(buf)-pos,
            "   %s %s(%s) %s %s\n",
            rn[altn[i].risk_alternate], ALTN_AIRPORTS[i].icao,
            ALTN_AIRPORTS[i].name, altn[i].raw,
            altn[i].metar_ok ? "" : "⚠");
    }
    pos += snprintf(buf+pos, sizeof(buf)-pos,
        "   ── 省外备降 ──\n");
    for (int i = 0; i < MAX_ALTN; i++) {
        if (!ALTN_AIRPORTS[i].icao || !ALTN_AIRPORTS[i].icao[0]) continue;
        if (ALTN_AIRPORTS[i].is_yunnan) continue;
        pos += snprintf(buf+pos, sizeof(buf)-pos,
            "   %s %s(%s) %s %s\n",
            rn[altn[i].risk_alternate], ALTN_AIRPORTS[i].icao,
            ALTN_AIRPORTS[i].name, altn[i].raw,
            altn[i].metar_ok ? "" : "⚠");
    }

    pos += snprintf(buf+pos, sizeof(buf)-pos,
        "\n── 飞行评估 ──\n");
    /* 雷暴: 查METAR里是否有TS/CB */
    int has_cb = 0;
    if (zppp.valid && (strstr(zppp.raw, "TS") || strstr(zppp.raw, "CB"))) has_cb = 1;
    if (precip > 5 && cloud > 80) has_cb = 1;
    pos += snprintf(buf+pos, sizeof(buf)-pos,
        "   雷暴: %s\n", has_cb ? "🔴 有雷暴" : "✅ 无");
    pos += snprintf(buf+pos, sizeof(buf)-pos,
        "   积冰: %s (温度%.1f°C/湿度%.0f%%)\n",
        (t < ICE_TRACE_TEMP_C && h > 80) ? "🟠 有积冰可能" : "✅ 无积冰风险", t, h);
    pos += snprintf(buf+pos, sizeof(buf)-pos,
        "   推荐进近: %s(%s)\n",
        best->heading <= 90 ? "北向" : "南向",
        best->risk_approach == RISK_LOW ? "ILS可用" :
        best->risk_approach == RISK_CAUTION ? "关注最低标准" : "建议备降");

    pos += snprintf(buf+pos, sizeof(buf)-pos,
        "\n── 维修评估 ──\n");
    pos += snprintf(buf+pos, sizeof(buf)-pos,
        "   跑道: %s\n", rc_str[rwy_cond]);
    pos += snprintf(buf+pos, sizeof(buf)-pos,
        "   雷击: %s\n", has_cb ? "⚠ 有雷暴, 注意停机坪安全" : "✅ 安全");

    pos += snprintf(buf+pos, sizeof(buf)-pos,
        "\n── 数据源 ──\n");
    pos += snprintf(buf+pos, sizeof(buf)-pos,
        "   实况: met.no/C引擎 | METAR: %s\n", zppp.valid ? "ZPPP在线" : "离线");
    pos += snprintf(buf+pos, sizeof(buf)-pos,
        "   能见度: %s\n", zppp.vis_m > 0 ? "METAR实测" : "模型估算");
    pos += snprintf(buf+pos, sizeof(buf)-pos,
        "   METAR备降场: %d/11个有实时数据\n",
        (int)(sizeof(altn)/sizeof(altn[0]) - altn_bad));
    pos += snprintf(buf+pos, sizeof(buf)-pos,
        "\n════════════════════════════════════════════════════\n");
    pos += snprintf(buf+pos, sizeof(buf)-pos,
        " 评估: 非正式参考, 最终以签派/机长判断为准\n");
    pos += snprintf(buf+pos, sizeof(buf)-pos,
        "════════════════════════════════════════════════════\n");

    FILE *f = fopen(AVIATION_REPORT, "w");
    if (f) { fputs(buf, f); fclose(f); }

    printf("  ✅ ZPPP评估 | 推荐%02d号 | 备降场%d/%d可用 | %s\n",
           best->heading, altn_ok, altn_ok+altn_bad,
           has_cb ? "⚠雷暴" : "✅无特殊天气");
    return 0;
}
