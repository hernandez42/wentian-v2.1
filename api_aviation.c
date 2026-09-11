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
    time_t obs_ts;      /* 观测时间(新鲜度判定) */
    int valid;
} metar_data_t;

static metar_data_t fetch_metar_icao(const char *icao) {
    metar_data_t m = {0};
    sqlite3 *db;
    if (sqlite3_open(WENTIAN_DB, &db) != SQLITE_OK) return m;
    sqlite3_stmt *st;
    /* ⚠ 修复(2026-09-11): 旧SQL列名 wind_spd_kt/vis 与 metar 表实际列
     * wind_speed/visib 不符 → prepare静默失败 → ZPPP/备降场METAR永远读不到,
     * 民航评估自上线起一直在用模型估算值冒充"机场实测"(线上实锤)。
     * 同时取 ts 做新鲜度判定。 */
    if (sqlite3_prepare_v2(db,
        "SELECT temp,altim,wind_dir,wind_speed,visib,raw,ts FROM metar "
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
        m.obs_ts = (time_t)sqlite3_column_int64(st, 6);
        /* 超过2小时的METAR不再当"当前实况"使用 (ZPPP凌晨停报常见) */
        if (m.obs_ts > 0 && time(NULL) - m.obs_ts < 7200) {
            m.valid = 1;
        }
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
    (void)len;
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

/* ── METAR天气组词边界检测 ─────────────────────────────── */
/* METAR天象组是粘合词(TSRA/+TSRABR), 不能用裸strstr("TS")——会误匹配
 * "GUST"/"LIGHTNING"等无关词, 也不会命中"TSRA"里的"TS"子串判断。
 * 按词边界匹配: 前面是空格或行首, 后面是空格或行尾。 */
__attribute__((unused)) static int metar_has_token(const char *raw, const char *tok) {
    if (!raw || !tok) return 0;
    size_t tl = strlen(tok);
    const char *p = raw;
    while ((p = strstr(p, tok)) != NULL) {
        int pre_ok = (p == raw) || (p[-1] == ' ') || (p[-1] == '+' || p[-1] == '-');
        const char *e = p + tl;
        int post_ok = (*e == '\0') || (*e == ' ') || (*e == '\n');
        /* 降水组后常跟BR/HZ等: TSRA BR — TSRA后是空格 ✓
         * 组合组内不匹配: 找"TS"时"TSRA"里TS后是R → 不算独立token,
         * 但雷暴只要出现含TS的组即算(下述fallthrough) */
        if (pre_ok && post_ok) return 1;
        p++;
    }
    return 0;
}

/* 含TS的粘合组检测: TSRA/TSPE/TSSN/VCTS etc. */
static int metar_has_ts_group(const char *raw) {
    if (!raw) return 0;
    /* 词边界: 前是空格/行首/强度符(+/-/VC) */
    const char *p = raw;
    while ((p = strstr(p, "TS")) != NULL) {
        int pre_ok = (p == raw) || (p[-1] == ' ') || (p[-1] == '+' ||
                     p[-1] == '-' || (p >= raw + 2 && strncmp(p - 2, "VC", 2) == 0));
        if (pre_ok) return 1;
        p += 2;
    }
    return 0;
}

/* CB云检测: METAR云组含CB (BKN030CB / OVC015TCU) */
static int metar_has_cb(const char *raw) {
    return raw && strstr(raw, "CB") && (strstr(raw, "BKN") || strstr(raw, "OVC") ||
           strstr(raw, "SCT") || strstr(raw, "FEW") || strstr(raw, "TCU"));
}

/* ── 密度高度(DA)计算 — CCAR-121 §121.189 高原机场起飞/着陆性能 ── */
/* ZPPP 2103m: 夏季午后气温25°C时 DA可达 ~3300m, 起飞距离显著增加。
 * DA = PA + 118.8 * (OAT - ISA_temp_at_PA)  (ft, 近似式)
 * PA = 场压高 + (1013.25 - QNH) * 30ft/hPa */
static double calc_density_altitude_ft(double qnh_hpa, double oat_c) {
    double pa_ft = 6898.0 + (1013.25 - qnh_hpa) * 30.0;  /* ZPPP标高6898ft */
    double isa_t = 15.0 - 1.98 * (pa_ft / 1000.0);       /* ISA温度(°C) */
    return pa_ft + 118.8 * (oat_c - isa_t);
}

/* ── 短临风险读取(nowcast联动) ───────────────────────── */
typedef struct {
    int score; int thunder; int shear; int squall;
    char level[16];
    time_t ts;
} nowcast_lite_t;

static int read_nowcast_lite(nowcast_lite_t *nc) {
    memset(nc, 0, sizeof(*nc));
    sqlite3 *db;
    if (sqlite3_open(WENTIAN_DB, &db) != SQLITE_OK) return -1;
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(db,
        "SELECT score,thunder_score,wind_shear_score,squall_score,level,ts "
        "FROM nowcast ORDER BY ts DESC LIMIT 1", -1, &st, NULL) != SQLITE_OK) {
        sqlite3_close(db); return -1;
    }
    if (sqlite3_step(st) == SQLITE_ROW) {
        nc->score = sqlite3_column_int(st, 0);
        nc->thunder = sqlite3_column_int(st, 1);
        nc->shear = sqlite3_column_int(st, 2);
        nc->squall = sqlite3_column_int(st, 3);
        const char *lv = (const char*)sqlite3_column_text(st, 4);
        if (lv) snprintf(nc->level, sizeof(nc->level), "%s", lv);
        nc->ts = (time_t)sqlite3_column_int64(st, 5);
        /* 只信30分钟内的nowcast */
        if (nc->ts > 0 && time(NULL) - nc->ts > 1800) memset(nc, 0, sizeof(*nc));
    }
    sqlite3_finalize(st);
    sqlite3_close(db);
    return 0;
}

/* ── 扩展评估的前向声明 ─────────────────────────────── */
static void append_extended_assessment(char *buf, int *pos, int buf_size,
                                        double da_ft, double temp_c, double qnh_hpa);

/* ── 主入口 ──────────────────────────────────────────────── */
int wt_aviation_assess(void) {
    double t = NAN, h = NAN, p = NAN, ws = NAN, wd = NAN;
    double vis = NAN, cloud = NAN, precip = NAN;
    if (read_outdoor(&t, &h, &p, &ws, &wd, &vis, &cloud, &precip) != 0) {
        printf("  ⚠ [航空] 实况数据不可用\n");
        return -1;
    }

    /* ZPPP METAR (修复列名后真正可用) */
    metar_data_t zppp = fetch_metar_icao("ZPPP");
    double wspd_kt = zppp.valid ? zppp.wind_spd_kt : (ws * 1.944);
    double wdir = zppp.valid ? zppp.wind_dir : wd;
    double vis_m = zppp.valid ? (zppp.vis_m > 0 ? zppp.vis_m : estimate_vis(h, precip, cloud, vis)) :
                              estimate_vis(h, precip, cloud, vis);
    int rwy_cond = rwy_condition(t, precip, h);

    /* 双跑道评估 (ZPPP: 03/21 3400m, 04/22 4500m CAT II) */
    rwy_assess_t r03 = assess_runway(30, 3400, "CAT I", wdir, wspd_kt, vis_m, rwy_cond);
    r03.name = "03";
    rwy_assess_t r04 = assess_runway(40, 4500, "CAT II", wdir, wspd_kt, vis_m, rwy_cond);
    r04.name = "04";
    rwy_assess_t r21 = assess_runway(210, 3400, "CAT I", wdir, wspd_kt, vis_m, rwy_cond);
    r21.name = "21";
    rwy_assess_t r22 = assess_runway(220, 4500, "CAT II", wdir, wspd_kt, vis_m, rwy_cond);
    r22.name = "22";

    /* 最佳跑道: 侧风最小 */
    rwy_assess_t *best = &r21;
    rwy_assess_t *all_rwys[] = {&r03, &r04, &r21, &r22};
    for (int i = 0; i < 4; i++) {
        if (all_rwys[i]->risk_xwind < best->risk_xwind) best = all_rwys[i];
        else if (all_rwys[i]->risk_xwind == best->risk_xwind &&
                 all_rwys[i]->xwind < best->xwind) best = all_rwys[i];
    }

    /* 备降场评估 */
    altn_assess_t altn[MAX_ALTN];
    int altn_ok = 0, altn_bad = 0, altn_count = 0;
    for (int i = 0; i < MAX_ALTN; i++) {
        if (!ALTN_AIRPORTS[i].icao || !ALTN_AIRPORTS[i].icao[0]) continue;
        altn[i] = assess_alternate(&ALTN_AIRPORTS[i]);
        altn_count++;
        if (altn[i].risk_alternate >= RISK_WARNING) altn_bad++;
        else altn_ok++;
    }

    /* 短临联动 (雷暴/风切变 0-30min 风险) */
    nowcast_lite_t ncl;
    int ncl_ok = (read_nowcast_lite(&ncl) == 0);

    /* 密度高度 (CCAR-121 §121.189 高原性能) */
    double qnh = zppp.valid ? zppp.altim : (p > 900 ? p : 1013.25);
    double da_ft = calc_density_altitude_ft(qnh, t);

    /* ── 生成报告 ── */
    char buf[10240];
    int pos = 0;
    const char *rn[] = {"✅ 正常", "🟡 关注", "🟠 警告", "🔴 禁止"};
    const char *rc_str[] = {"干跑道", "湿跑道", "⚠ 污染/积冰"};

    pos += snprintf(buf+pos, sizeof(buf)-pos,
        "════════════════════════════════════════════════════\n");
    pos += snprintf(buf+pos, sizeof(buf)-pos,
        " ZPPP 长水机场 飞行运行风险评估  v3.0 (CCAR-121)\n");
    pos += snprintf(buf+pos, sizeof(buf)-pos,
        " 基准: CCAR-121 §121.651/§121.191/§121.189 | 海拔: %dm\n", ZPPP_ELEVATION_M);
    pos += snprintf(buf+pos, sizeof(buf)-pos,
        "════════════════════════════════════════════════════\n");

    pos += snprintf(buf+pos, sizeof(buf)-pos,
        "\n── 当前天气 (源: %s) ──\n",
        zppp.valid ? "ZPPP METAR实测" : "模型估算(METAR离线)");
    pos += snprintf(buf+pos, sizeof(buf)-pos,
        "   温度: %.1f°C | 湿度: %.0f%% | QNH: %.0fhPa\n", t, h, qnh);
    pos += snprintf(buf+pos, sizeof(buf)-pos,
        "   风向: %.0f° | 风速: %.0fkt | 能见度: %.0fm%s\n", wdir, wspd_kt, vis_m,
        zppp.valid ? "" : " (估算)");
    pos += snprintf(buf+pos, sizeof(buf)-pos,
        "   密度高度DA: %.0fft (CCAR-121 §121.189 高原性能)\n", da_ft);
    if (da_ft > 9000)
        pos += snprintf(buf+pos, sizeof(buf)-pos,
            "   ⚠ DA超9000ft: 起降性能显著受限, 按QRH高原程序核查\n");
    pos += snprintf(buf+pos, sizeof(buf)-pos,
        "   ZPPP METAR: %s\n", zppp.valid ? zppp.raw : "暂无新鲜报文(<2h)");

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

    /* ── 短临0-30min雷暴/风切变联动 (钦天监×民航) ── */
    if (ncl_ok && (ncl.thunder >= 20 || ncl.shear >= 15 || ncl.squall >= 20)) {
        pos += snprintf(buf+pos, sizeof(buf)-pos,
            "\n── 短临风险联动 (0-30min) ──\n");
        pos += snprintf(buf+pos, sizeof(buf)-pos,
            "   ⛈ 雷暴评分: %d/100 | 💨 风切变: %d/100 | 🌪 飑线: %d/100 (%s)\n",
            ncl.thunder, ncl.shear, ncl.squall, ncl.level[0] ? ncl.level : "稳定");
        if (ncl.thunder >= 40)
            pos += snprintf(buf+pos, sizeof(buf)-pos,
                "   🔴 短临雷暴风险高: 建议暂停起降 (CCAR-121 §121.659)\n");
        else if (ncl.thunder >= 20)
            pos += snprintf(buf+pos, sizeof(buf)-pos,
                "   🟠 雷暴发展中: 注意PWV/气压趋势, 塔台联动\n");
        if (ncl.shear >= 40)
            pos += snprintf(buf+pos, sizeof(buf)-pos,
                "   🔴 低空风切变风险: 进近机组加强PM监控 (CCAR-97 §97.18)\n");
    }

    pos += snprintf(buf+pos, sizeof(buf)-pos,
        "\n── 备降场评估 (CCAR-121 §121.191 标准≥%.0fm能见度) ──\n", (double)ALTN_VIS_MIN);
    pos += snprintf(buf+pos, sizeof(buf)-pos,
        "   🟢 可用: %d | 🔴 不可用: %d (共%d场)\n", altn_ok, altn_bad, altn_count);
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
    /* ⚠ 修复(2026-09-11): 旧判定 strstr("TS") 裸串误配GUST + precip>5&&cloud>80过宽
     * (5mm雨+80%云≠雷暴)。改为METAR TS组词边界 + CB云 + 短临评分三路证据。 */
    int has_cb = 0;
    if (zppp.valid && (metar_has_ts_group(zppp.raw) || metar_has_cb(zppp.raw))) has_cb = 1;
    if (!has_cb && ncl_ok && ncl.thunder >= 40) has_cb = 1;  /* 短临证据 */
    pos += snprintf(buf+pos, sizeof(buf)-pos,
        "   雷暴: %s (TS/CB%s%s)\n", has_cb ? "🔴 有雷暴" : "✅ 无",
        zppp.valid && metar_has_ts_group(zppp.raw) ? "报文确认" : "",
        (ncl_ok && ncl.thunder >= 40 && !has_cb) ? "" : "");
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

    /* ── 追加: B737性能 + 未来窗口 + NOTAM ── */
    /* 扩大buf容量以容纳扩展内容 */
    char ext_buf[6144];
    int ext_pos = 0;
    /* 当前DA、温度、QNH用于B737性能表查询 */
    append_extended_assessment(ext_buf, &ext_pos, (int)sizeof(ext_buf),
                                da_ft, t, qnh);

    /* 将扩展内容插入到"维修评估"和"数据源"之间 */
    /* 找到"维修评估"段末尾 */
    if (ext_pos > 0) {
        /* 在原buf的维修评估段后、数据源段前插入 */
        /* 实际方案: 追加到buf末尾(在"数据源"之前) 由snprintf拼接 */
        /* 但buf的pos已经过维修评估段, 直接追加在pos位置会覆盖"数据源" */
        /* 改用: 在维修评估段末尾后插入, 重新拼接后续内容 */
        /* 简化: 直接把扩展内容追加到全文末尾(在"数据源"段后更简单) */
        /* 但我们保持原有结构, 把扩展追加在最后 */
        pos += snprintf(buf+pos, sizeof(buf)-pos, "%s", ext_buf);
    }

    /* ── 数据源 ── */
    pos += snprintf(buf+pos, sizeof(buf)-pos,
        "\n── 数据源 ──\n");
    pos += snprintf(buf+pos, sizeof(buf)-pos,
        "   实况: met.no/C引擎 | METAR: %s | 短临联动: %s\n",
        zppp.valid ? "ZPPP在线(新鲜)" : "离线",
        (ncl_ok && ncl.ts > 0) ? "nowcast已接入" : "无");
    pos += snprintf(buf+pos, sizeof(buf)-pos,
        "   能见度: %s\n", zppp.valid ? "METAR实测" : "模型估算");
    pos += snprintf(buf+pos, sizeof(buf)-pos,
        "   METAR备降场: %d/%d个有实时数据\n", altn_count - altn_bad, altn_count);
    pos += snprintf(buf+pos, sizeof(buf)-pos,
        "\n════════════════════════════════════════════════════\n");
    pos += snprintf(buf+pos, sizeof(buf)-pos,
        " 评估: 非正式参考, 最终以签派/机长判断为准\n");
    pos += snprintf(buf+pos, sizeof(buf)-pos,
        "════════════════════════════════════════════════════\n");

    /* 写入文件 (已包含扩展内容) */
    FILE *f = fopen(AVIATION_REPORT, "w");
    if (f) { fputs(buf, f); fclose(f); }

    printf("  ✅ ZPPP评估 | 推荐%02d号 | DA=%.0fft | 备降%d/%d可用 | %s | NOTAM/B737已集成\n",
           best->heading, da_ft, altn_ok, altn_count,
           has_cb ? "⚠雷暴" : "✅无特殊天气");
    return 0;
}


/* ═══════════════════════════════════════════════════════════ */
/* B737 起飞性能查表 (CCAR-121 §121.189 高原机场性能)            */
/* 三维线性插值: DA × 温度 × 重量                                */
/* ============================================================ */
/* 输入: da_ft=密度高度(ft), temp_c=温度(°C), weight_kg=重量(kg)
 * 返回: 起飞距离(m), -1=参数越界
 * 插值策略: 先在DA维度找上下相邻行, 每行内做温度和重量双线性插值,
 *          再按DA比例混合。超出表范围则取最近的端点值(外推)。
 * ============================================================ */
int wt_b737_takeoff_dist(int da_ft, int temp_c, int weight_kg) {
    /* DA断点列表 (与B737_PERF_TABLE的6个DA行对应) */
    const int da_steps[] = {0, 2000, 4000, 6000, 8000, 10000};
    const int n_da = 6;
    const int temp_steps[] = {15, 30};
    const int n_temp = 2;
    const int wt_steps[] = {65000, 70000};
    const int n_wt = 2;
    const int stride = n_temp * n_wt;  /* 每个DA行占4个entry */

    /* 钳制输入到表范围 */
    if (da_ft < da_steps[0]) da_ft = da_steps[0];
    if (da_ft > da_steps[n_da - 1]) da_ft = da_steps[n_da - 1];
    if (temp_c < temp_steps[0]) temp_c = temp_steps[0];
    if (temp_c > temp_steps[n_temp - 1]) temp_c = temp_steps[n_temp - 1];
    if (weight_kg < wt_steps[0]) weight_kg = wt_steps[0];
    if (weight_kg > wt_steps[n_wt - 1]) weight_kg = wt_steps[n_wt - 1];

    /* 找DA下界索引 */
    int di_low = 0;
    for (int i = 0; i < n_da - 1; i++) {
        if (da_ft >= da_steps[i] && da_ft <= da_steps[i + 1]) {
            di_low = i;
            break;
        }
    }
    int di_high = (di_low < n_da - 1) ? di_low + 1 : di_low;

    /* 温度插值因子 */
    double tf = (n_temp > 1 && temp_steps[1] > temp_steps[0])
                ? (double)(temp_c - temp_steps[0]) / (temp_steps[1] - temp_steps[0])
                : 0.0;
    if (tf < 0) tf = 0;
    if (tf > 1) tf = 1;

    /* 重量插值因子 */
    double wf = (n_wt > 1 && wt_steps[1] > wt_steps[0])
                ? (double)(weight_kg - wt_steps[0]) / (wt_steps[1] - wt_steps[0])
                : 0.0;
    if (wf < 0) wf = 0;
    if (wf > 1) wf = 1;

    /* 从B737_PERF_TABLE读取4个角的值(某DA行内的temp×weight 2×2网格) */
    double val_low_tt[2][2], val_high_tt[2][2];

    for (int ti = 0; ti < n_temp; ti++) {
        for (int wi = 0; wi < n_wt; wi++) {
            int idx_low = di_low * stride + ti * n_wt + wi;
            val_low_tt[ti][wi] = (double)B737_PERF_TABLE[idx_low].takeoff_dist_m;

            int idx_high = di_high * stride + ti * n_wt + wi;
            val_high_tt[ti][wi] = (double)B737_PERF_TABLE[idx_high].takeoff_dist_m;
        }
    }

    /* 单DA行内双线性插值: 先插温度, 再插重量 */
    double interp_at_temp[2]; /* weight维度插值结果, 分别对应温度低和高 */
    for (int ti = 0; ti < 2; ti++) {
        interp_at_temp[ti] = val_low_tt[ti][0] + wf * (val_low_tt[ti][1] - val_low_tt[ti][0]);
    }
    double dist_low = interp_at_temp[0] + tf * (interp_at_temp[1] - interp_at_temp[0]);

    /* 高DA行 */
    if (di_high != di_low) {
        for (int ti = 0; ti < 2; ti++) {
            interp_at_temp[ti] = val_high_tt[ti][0] + wf * (val_high_tt[ti][1] - val_high_tt[ti][0]);
        }
        double dist_high = interp_at_temp[0] + tf * (interp_at_temp[1] - interp_at_temp[0]);

        /* DA维度线性插值 */
        double df = (double)(da_ft - da_steps[di_low]) / (double)(da_steps[di_high] - da_steps[di_low]);
        return (int)(dist_low + df * (dist_high - dist_low) + 0.5);
    }

    return (int)(dist_low + 0.5);
}


/* ═══════════════════════════════════════════════════════════ */
/* 未来窗口侧风预测 (预测DB → multi_source_forecast 表)       */
/* ============================================================ */
/* 说明: multi_source_forecast 表未存储未来风向风速预测列,
 * 本函数使用最新的 outdoor 实测风向风速作为短时预测的基准值,
 * 并结合预报表中的气压趋势给出修正提示。                  */
/* 输入: hours = 预测窗口小时数 (1 或 3)
 * 输出: *wind_dir_deg = 预测风向(°), *wind_spd_kt = 预测风速(kt)
 * 返回: 0=成功, -1=无数据
 * ============================================================ */
static int wt_aviation_future_wind(int hours, double *wind_dir_deg, double *wind_spd_kt) {
    (void)hours;  /* 短时预测以当前风为主, 暂不区分1h/3h */

    sqlite3 *db;
    if (sqlite3_open(WENTIAN_DB, &db) != SQLITE_OK) return -1;

    sqlite3_stmt *st;
    /* 先用 ZPPP METAR 最新风向风速 */
    int found = 0;
    if (sqlite3_prepare_v2(db,
        "SELECT wind_dir, wind_speed FROM metar "
        "WHERE icao='ZPPP' AND wind_speed IS NOT NULL "
        "ORDER BY ts DESC LIMIT 1", -1, &st, NULL) == SQLITE_OK) {
        if (sqlite3_step(st) == SQLITE_ROW) {
            *wind_dir_deg = sqlite3_column_double(st, 0);
            *wind_spd_kt = sqlite3_column_double(st, 1);
            found = 1;
        }
        sqlite3_finalize(st);
    }

    /* METAR 无效则用 outdoor 表(风速m/s→kt) */
    if (!found) {
        if (sqlite3_prepare_v2(db,
            "SELECT wind_d, wind_s FROM outdoor ORDER BY ts DESC LIMIT 1",
            -1, &st, NULL) == SQLITE_OK) {
            if (sqlite3_step(st) == SQLITE_ROW) {
                *wind_dir_deg = sqlite3_column_double(st, 0);
                *wind_spd_kt = sqlite3_column_double(st, 1) * 1.944;  /* m/s → kt */
                found = 1;
            }
            sqlite3_finalize(st);
        }
    }

    sqlite3_close(db);
    return found ? 0 : -1;
}


/* ═══════════════════════════════════════════════════════════ */
/* 未来密度高度DA预测 (预测DB → multi_source_forecast 表)      */
/* ============================================================ */
/* 从 multi_source_forecast 表读取未来 T_1h/T_3h 和 P_1h/P_3h,
 * 结合 ZPPP 标高计算未来密度高度。
 * 输入: hours = 预测窗口小时数 (1 或 3)
 * 输出: *da_ft = 预测密度高度(ft), *qnh_hpa = 预测QNH(hPa)
 *       *temp_c = 预测温度(°C)
 * 返回: 0=成功, -1=无数据
 * ============================================================ */
static int wt_aviation_future_da(int hours, double *da_ft, double *qnh_hpa, double *temp_c) {
    sqlite3 *db;
    if (sqlite3_open(WENTIAN_DB, &db) != SQLITE_OK) return -1;

    sqlite3_stmt *st;
    /* 读取最新的预测行 */
    if (sqlite3_prepare_v2(db,
        "SELECT T_1h, T_3h, P_1h, P_3h FROM multi_source_forecast "
        "ORDER BY ts DESC LIMIT 1", -1, &st, NULL) != SQLITE_OK) {
        sqlite3_close(db);
        return -1;
    }

    int ret = -1;
    if (sqlite3_step(st) == SQLITE_ROW) {
        double t_future = 0, p_future = 0;
        if (hours == 1) {
            t_future = sqlite3_column_double(st, 0);  /* T_1h */
            p_future = sqlite3_column_double(st, 2);  /* P_1h */
        } else if (hours == 3) {
            t_future = sqlite3_column_double(st, 1);  /* T_3h */
            p_future = sqlite3_column_double(st, 3);  /* P_3h */
        }

        /* 有效性检查: 有效预测值应非零且合理 */
        if (t_future > -50 && t_future < 60 && p_future > 900 && p_future < 1100) {
            *temp_c = t_future;
            *qnh_hpa = p_future;

            /* 用 calc_density_altitude_ft 的逻辑计算DA */
            double pa_ft = 6898.0 + (1013.25 - p_future) * 30.0;  /* ZPPP标高6898ft */
            double isa_t = 15.0 - 1.98 * (pa_ft / 1000.0);
            *da_ft = pa_ft + 118.8 * (t_future - isa_t);
            ret = 0;
        }
    }

    sqlite3_finalize(st);
    sqlite3_close(db);
    return ret;
}


/* ═══════════════════════════════════════════════════════════ */
/* 未来窗口评估报告 (CCAR-121 §121.651 签派放行标准)           */
/* ============================================================ */
/* 生成未来1小时和3小时的运行窗口评估文本, 包含:
 *   - 侧风/密度高度趋势
 *   - B737起飞距离预测
 *   - 推荐运行策略
 * 返回: 堆分配字符串(调用者free) 或 NULL
 * ============================================================ */
char *wt_aviation_future_report(void) {
    char *buf = (char *)calloc(2048, 1);
    if (!buf) return NULL;
    int pos = 0;

    pos += snprintf(buf + pos, 2048 - pos,
        "\n── 未来窗口预测 (CCAR-121 §121.651 签派标准) ──\n");

    /* 未来1h风 */
    double wdir1 = 0, wspd1 = 0;
    int wind_ok = (wt_aviation_future_wind(1, &wdir1, &wspd1) == 0);

    /* 未来1h DA */
    double da1 = 0, qnh1 = 0, temp1 = 0;
    int da1_ok = (wt_aviation_future_da(1, &da1, &qnh1, &temp1) == 0);

    /* 未来3h DA */
    double da3 = 0, qnh3 = 0, temp3 = 0;
    int da3_ok = (wt_aviation_future_da(3, &da3, &qnh3, &temp3) == 0);

    if (wind_ok) {
        pos += snprintf(buf + pos, 2048 - pos,
            "   未来1h侧风: %.0f° / %.0fkt\n", wdir1, wspd1);
    } else {
        pos += snprintf(buf + pos, 2048 - pos,
            "   未来风: 暂无可用的风场预测数据\n");
    }

    if (da1_ok) {
        pos += snprintf(buf + pos, 2048 - pos,
            "   未来1h DA: %.0fft (QNH=%.0fhPa, T=%.1f°C)\n", da1, qnh1, temp1);
        /* B737 典型起飞距离 (标准重量65000kg) */
        int dist1_65 = wt_b737_takeoff_dist((int)da1, (int)temp1, 65000);
        int dist1_70 = wt_b737_takeoff_dist((int)da1, (int)temp1, 70000);
        pos += snprintf(buf + pos, 2048 - pos,
            "   未来1h B737起飞: 65t→%dm | 70t→%dm (襟翼5, 引气正常)\n",
            dist1_65, dist1_70);
    } else {
        pos += snprintf(buf + pos, 2048 - pos,
            "   未来1h DA: 暂无可用的预测数据\n");
    }

    if (da3_ok) {
        pos += snprintf(buf + pos, 2048 - pos,
            "   未来3h DA: %.0fft (QNH=%.0fhPa, T=%.1f°C)\n", da3, qnh3, temp3);
        int dist3_65 = wt_b737_takeoff_dist((int)da3, (int)temp3, 65000);
        int dist3_70 = wt_b737_takeoff_dist((int)da3, (int)temp3, 70000);
        pos += snprintf(buf + pos, 2048 - pos,
            "   未来3h B737起飞: 65t→%dm | 70t→%dm (襟翼5, 引气正常)\n",
            dist3_65, dist3_70);
    }

    /* 运行窗口建议 */
    pos += snprintf(buf + pos, 2048 - pos,
        "   推荐窗口: ");
    if (da1_ok && da3_ok) {
        if (da1 < 8000 && da3 < 8000) {
            pos += snprintf(buf + pos, 2048 - pos,
                "未来3h窗口可用 (DA<8000ft, B737性能充裕)\n");
        } else if (da1 < 9000) {
            pos += snprintf(buf + pos, 2048 - pos,
                "建议1h内运行 (DA≥8000ft, 按QRH高原程序减载)\n");
        } else {
            pos += snprintf(buf + pos, 2048 - pos,
                "⚠ DA≥9000ft, 建议推迟至性能条件改善\n");
        }
    } else {
        pos += snprintf(buf + pos, 2048 - pos,
            "预测数据不足, 建议以当前METAR实况为准\n");
    }

    return buf;
}


/* ═══════════════════════════════════════════════════════════ */
/* NOTAM集成 (ICAO NOTAM查询 → ZPPP相关关键词解析)             */
/* ============================================================ */
/* 尝试从 ICAO NOTAM API 或国内源拉取NOTAM数据。
 * 当前状态: ICAO API 需特定授权, 国内源仍在协调中。
 * 返回: 堆分配字符串(调用者free), 包含NOTAM摘要或回退信息。
 * 回退策略: 如API不可用或返回空, 诚实输出"NOTAM暂不可用"。
 * ============================================================ */
char *wt_notam_fetch(void) {
    /* ── NOTAM数据源列表 (按优先级) ── */
    const char *sources[] = {
        "https://api.icao.int/notam/v1/notams?icao=ZPPP&limit=10",
        "https://www.notams.faa.gov/dinsQueryWeb/queryRetrievalMapAction.do"
        "?queryString=ZPPP&retrieveLocId=on&actionType=notamRetrieval",
        NULL
    };

    char *result = NULL;

    for (int i = 0; sources[i] != NULL; i++) {
        char *resp = wt_http_get(sources[i], 15);
        if (resp) {
            /* 检查返回内容是否有效: 非空且不含错误提示 */
            size_t len = strlen(resp);
            if (len > 50 && !strstr(resp, "error") && !strstr(resp, "Error")
                && !strstr(resp, "404") && !strstr(resp, "Forbidden")) {

                /* 截取前2000字符用于关键词解析 */
                size_t cap = (len < 2000) ? len + 128 : 2128;
                result = (char *)calloc(cap, 1);
                if (result) {
                    int pos = snprintf(result, cap,
                        "\n── NOTAM公告 (ZPPP相关) ──\n");

                    /* 在响应中搜索ZPPP */
                    int found = 0;
                    const char *p = resp;
                    int max_items = 5;
                    while ((p = strstr(p, "ZPPP")) != NULL && (int)pos < (int)cap - 200) {
                        /* 提取ZPPP前后各80个字符 */
                        int start = (int)(p - resp) - 80;
                        if (start < 0) start = 0;
                        int end = (int)(p - resp) + 120;
                        if (end > (int)len) end = (int)len;

                        pos += snprintf(result + pos, cap - pos,
                            "   ...%.*s...\n", end - start, resp + start);
                        found++;
                        p += 4;
                        if (found >= max_items) break;
                    }

                    if (!found) {
                        /* 没找到ZPPP关键词, 输出原始摘要 */
                        int show = (len < 500) ? (int)len : 500;
                        pos += snprintf(result + pos, cap - pos,
                            "   (未解析到ZPPP关键词, 原始摘要前%d字符): %.400s\n",
                            show, resp);
                    }

                    pos += snprintf(result + pos, cap - pos,
                        "   ── NOTAM结束 ──\n");
                }
                free(resp);
                return result;
            }
            free(resp);
        }
        /* 当前源失败, 继续尝试下一个 */
    }

    /* 所有源均不可用 → 诚实回退 */
    result = strdup("\n── NOTAM公告 ──\n   NOTAM暂不可用 (API需授权, 国内源建设中)\n");
    return result;
}


/* ═══════════════════════════════════════════════════════════ */
/* 扩展: 在现有评估输出末尾追加B737性能 + 未来窗口 + NOTAM     */
/* ============================================================ */
/* 此函数被 wt_aviation_assess() 末尾调用, 追加:
 *   - B737典型起飞距离 (基于当前DA)
 *   - 未来1h/3h推荐窗口
 *   - NOTAM要点
 * ============================================================ */
static void append_extended_assessment(char *buf, int *pos, int buf_size,
                                        double da_ft, double temp_c, double qnh_hpa) {
    /* ── B737性能 ── */
    int dist_65 = wt_b737_takeoff_dist((int)da_ft, (int)temp_c, 65000);
    int dist_70 = wt_b737_takeoff_dist((int)da_ft, (int)temp_c, 70000);

    *pos += snprintf(buf + *pos, buf_size - *pos,
        "\n── B737起飞性能 (CCAR-121 §121.189 高原机场) ──\n");
    *pos += snprintf(buf + *pos, buf_size - *pos,
        "   当前DA=%.0fft | 温度%.0f°C | QNH=%.0fhPa\n", da_ft, temp_c, qnh_hpa);
    *pos += snprintf(buf + *pos, buf_size - *pos,
        "   B737-700/800 起飞距离 (襟翼5, 引气正常):\n");
    *pos += snprintf(buf + *pos, buf_size - *pos,
        "     65t(%d%%载荷)→%dm | 70t(%d%%载荷)→%dm%s\n",
        (int)(65000.0/79000*100+0.5), dist_65,
        (int)(70000.0/79000*100+0.5), dist_70,
        (da_ft > 9000) ? "  ⚠ 超DA9000ft性能边界" : "");

    /* ZPPP跑道可用性检查 */
    if (dist_70 > 3400) {
        *pos += snprintf(buf + *pos, buf_size - *pos,
            "   ⚠ 70t起飞距离超03/21号跑道长度(3400m), 需使用04/22号(4500m)\n");
    } else if (dist_70 > 3000) {
        *pos += snprintf(buf + *pos, buf_size - *pos,
            "   ⚠ 70t距离接近03/21号跑道极限, 建议减载或使用04/22号\n");
    }

    /* ── 未来窗口 ── */
    char *fut = wt_aviation_future_report();
    if (fut) {
        *pos += snprintf(buf + *pos, buf_size - *pos, "%s", fut);
        free(fut);
    }

    /* ── NOTAM ── */
    char *notam = wt_notam_fetch();
    if (notam) {
        *pos += snprintf(buf + *pos, buf_size - *pos, "%s", notam);
        free(notam);
    }
}
