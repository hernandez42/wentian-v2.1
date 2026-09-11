/* ───────────────────────────────────────────────────────────────
 * api_imperial.c — 钦天监 C 原生实现
 * 替代: imperial_observatory.py (388行) + astral.py (128行)
 * 迁移自 Python → C (2026-09-11 GLM5.3审计建议#1)
 *
 * 核心功能:
 *   1. 节气计算(太阳视黄经 → 24节气)
 *   2. 五行生克(温/湿/风/压 → 五行映射)
 *   3. 卦象推演(五行 → 八卦 → 预警阈值)
 *   4. 星象推演(28宿 + 月相)
 *   5. 增强因子输出(storm/precip/pressure + system_stable)
 *
 * 所有计算纯C, 零fork, 零文件IPC.
 * ─────────────────────────────────────────────────────────────── */

#include "wentian.h"
#include <math.h>
#include <time.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sqlite3.h>

/* ── 24节气名称 ──────────────────────────────────────── */
static const char *SOLAR_TERMS[24] = {
    "立春","雨水","惊蛰","春分","清明","谷雨",
    "立夏","小满","芒种","夏至","小暑","大暑",
    "立秋","处暑","白露","秋分","寒露","霜降",
    "立冬","小雪","大雪","冬至","小寒","大寒"
};

/* ── 节气气候经验知识库 ──────────────────────────────── */
typedef struct {
    const char *note;
    const char *trend;
    double storm_risk;
} term_knowledge_t;

static const term_knowledge_t TERM_KNOWLEDGE[24] = {
    {"东风解冻",  "升温", 0.3}, {"降水增多",  "波动", 0.4},
    {"雷始发声",  "快速升温",0.6}, {"昼夜均分",  "温和", 0.4},
    {"天气清明",  "稳定", 0.3}, {"雨生百谷",  "温湿", 0.5},
    {"万物繁茂",  "升温", 0.6}, {"麦穗渐满",  "暖",   0.5},
    {"忙种时节",  "炎热", 0.6}, {"日长之至",  "高温", 0.7},
    {"暑气初至",  "高温", 0.7}, {"暑气极盛",  "酷热", 0.8},
    {"凉风至",    "转凉", 0.6}, {"暑气渐消",  "凉爽", 0.5},
    {"露凝而白",  "转凉", 0.4}, {"昼夜平分",  "凉爽", 0.3},
    {"露水更寒",  "寒冷", 0.3}, {"露结为霜",  "寒冷", 0.3},
    {"万物收藏",  "寒意", 0.4}, {"虹藏不见",  "寒冷", 0.3},
    {"大雪纷飞",  "严寒", 0.3}, {"日短之至",  "严寒", 0.2},
    {"寒气未极",  "严寒", 0.3}, {"寒气极盛",  "严寒", 0.3}
};

/* ── 28宿: (名称, 起始黄经, 终止黄经) ──────────────── */
typedef struct { const char *name; double start; double end; } mansion_t;

static const mansion_t LUNAR_MANSIONS[28] = {
    {"角",0,12},{"亢",12,26},{"氐",26,42},{"房",42,54},{"心",54,66},{"尾",66,80},{"箕",80,94},
    {"斗",94,108},{"牛",108,122},{"女",122,136},{"虚",136,148},{"危",148,162},{"室",162,176},{"壁",176,188},
    {"奎",188,204},{"娄",204,216},{"胃",216,230},{"昴",230,240},{"毕",240,254},{"觜",254,260},{"参",260,276},
    {"井",276,294},{"鬼",294,304},{"柳",304,316},{"星",316,328},{"张",328,342},{"翼",342,356},{"轸",356,360}
};

/* ── 八卦能量/稳定度表 ──────────────────────────────── */
typedef struct {
    int stable;         /* 0=变动, 1=稳定 */
    const char *energy; /* 能量描述 */
    double alert_boost; /* 预警阈值修正 */
} hexagram_info_t;

static const hexagram_info_t HEXAGRAM_MAP[8] = {
    /* 乾兑离震巽坎艮坤 按先天八卦序 */
    {0, "强",   0.9},  /* ䷀ 乾 */
    {0, "中",   1.0},  /* ䷹ 兑 */
    {0, "强",   0.8},  /* ䷝ 离 */
    {0, "极强", 0.7},  /* ䷲ 震 */
    {0, "中",   1.1},  /* ䷸ 巽 */
    {0, "中",   0.9},  /* ䷜ 坎 */
    {1, "静",   1.0},  /* ䷳ 艮 */
    {1, "静",   1.0},  /* ䷁ 坤 */
};

static const char *HEXAGRAM_NAMES[8] = {"乾","兑","离","震","巽","坎","艮","坤"};

/* ── Sun apparent longitude (NOAA low-precision, 误差<0.01°) ─ */
/* 复用自 astral.py/imperial_observatory.py 的Meeus/NOAA算法 */
static double sun_apparent_longitude(time_t utc_ts) {
    struct tm *gmt = gmtime(&utc_ts);
    double y = gmt->tm_year + 1900;
    double m = gmt->tm_mon + 1;
    double d = gmt->tm_mday +
        (gmt->tm_hour + (gmt->tm_min + gmt->tm_sec/60.0)/60.0)/24.0;

    if (m <= 2) { y -= 1; m += 12; }
    int a = (int)(y / 100);
    int b = 2 - a + a / 4;
    double jd = (int)(365.25 * (y + 4716)) + (int)(30.6001 * (m + 1)) + d + b - 1524.5;
    double t = (jd - 2451545.0) / 36525.0;

    double L0 = 280.46646 + 36000.76983*t + 0.0003032*t*t;
    double M = 357.52911 + 35999.05029*t - 0.0001537*t*t;
    double Mr = M * M_PI / 180.0;
    double C = sin(Mr)*(1.914602 - 0.004817*t - 0.000014*t*t)
             + sin(2*Mr)*(0.019993 - 0.000101*t)
             + sin(3*Mr)*0.000289;
    double true_lon = L0 + C;
    double om = 125.04 - 1934.136 * t;
    double lon = true_lon - 0.00569 - 0.00478 * sin(om * M_PI / 180.0);
    lon = fmod(lon, 360.0);
    if (lon < 0) lon += 360.0;
    return lon;
}

/* ── Moon ecliptic longitude (Meeus ch.47 low-precision, ~0.3°) ── */
static double moon_ecliptic_longitude(time_t utc_ts) {
    struct tm *gmt = gmtime(&utc_ts);
    double y = gmt->tm_year + 1900;
    double m = gmt->tm_mon + 1;
    double d = gmt->tm_mday +
        (gmt->tm_hour + (gmt->tm_min + gmt->tm_sec/60.0)/60.0)/24.0;

    if (m <= 2) { y -= 1; m += 12; }
    int a = (int)(y / 100);
    int b = 2 - a + a / 4;
    double jd = (int)(365.25 * (y + 4716)) + (int)(30.6001 * (m + 1)) + d + b - 1524.5;
    double T = (jd - 2451545.0) / 36525.0;

    double Lp = fmod(218.3164477 + 481267.88123421*T - 0.0015786*T*T, 360.0);
    double D  = fmod(297.8501921 + 445267.1114034*T - 0.0018819*T*T, 360.0);
    double M  = fmod(357.5291092 + 35999.0502909*T - 0.0001536*T*T, 360.0);
    double Mp = fmod(134.9633964 + 477198.8675055*T + 0.0087414*T*T, 360.0);
    double F  = fmod(93.2720950 + 483202.0175233*T - 0.0036539*T*T, 360.0);

    double r = M_PI / 180.0;
    double lon = Lp
        + 6.288774*sin(Mp*r) + 1.274027*sin((2*D-Mp)*r)
        + 0.658314*sin(2*D*r) + 0.213618*sin(2*Mp*r)
        - 0.185116*sin(M*r) - 0.114336*sin(2*F*r);
    lon = fmod(lon, 360.0);
    if (lon < 0) lon += 360.0;
    return lon;
}

/* ── 黄经→28宿 ──────────────────────────────────────── */
static const char *ra_to_mansion(double ra_deg) {
    double r = fmod(ra_deg, 360.0);
    if (r < 0) r += 360.0;
    for (int i = 0; i < 28; i++) {
        if (r >= LUNAR_MANSIONS[i].start && r < LUNAR_MANSIONS[i].end)
            return LUNAR_MANSIONS[i].name;
    }
    return "角";
}

/* ── 太阳黄经→节气 ────────────────────────────────────── */
static int sun_lon_to_term_index(double sun_lon) {
    /* 立春从315°起, 每15°一节气 */
    double norm = fmod(sun_lon - 315.0 + 360.0, 360.0);
    int idx = (int)(norm / 15.0);
    if (idx >= 24) idx = 23;
    if (idx < 0) idx = 0;
    return idx;
}

/* ── 月相计算 ────────────────────────────────────────── */
static void calc_moon_phase(double moon_lon, double sun_lon,
                            const char **phase_name, const char **phase_emoji,
                            double *phase_angle) {
    double ph = fmod(moon_lon - sun_lon + 360.0, 360.0);
    *phase_angle = ph;
    if (ph < 45 || ph >= 315)       { *phase_name = "朔(新月)";   *phase_emoji = "🌑"; }
    else if (ph < 90)               { *phase_name = "蛾眉月";    *phase_emoji = "🌒"; }
    else if (ph < 135)              { *phase_name = "上弦月";    *phase_emoji = "🌓"; }
    else if (ph < 180)              { *phase_name = "盈凸月";    *phase_emoji = "🌔"; }
    else if (ph < 225)              { *phase_name = "望(满月)"; *phase_emoji = "🌕"; }
    else if (ph < 270)              { *phase_name = "亏凸月";    *phase_emoji = "🌖"; }
    else if (ph < 315)              { *phase_name = "下弦月";    *phase_emoji = "🌗"; }
    else                            { *phase_name = "残月";      *phase_emoji = "🌘"; }
}

/* ── 五行映射: 温/湿/风/压/Kp → 五行值 ──────────────── */
static void wuxing_map(double temp, double humid, double wind,
                       double press, double kp,
                       double *fire, double *water, double *wood,
                       double *metal, double *earth) {
    *fire  = fmin(100, fabs(temp - 20) * 4 + kp * 15);
    *water = fmin(100, humid * 0.8);
    *wood  = fmin(100, wind * 6);
    double press_dev = fabs(press - 1013.25) * 2;
    *metal = fmin(100, press_dev);
    *earth = fmax(0, 100 - (*fire + *water + *wood + *metal) / 4.0);
}

/* ── 五行四象限判断 ──────────────────────────────────── */
static const char *wuxing_quadrant(double fire, double water, double wood,
                                    double metal, double earth, double *score) {
    double vals[5] = {fire, water, wood, metal, earth};
    double sum = fire + water + wood + metal + earth;
    double avg = sum / 5.0;
    double max_v = vals[0], min_v = vals[0];
    for (int i = 1; i < 5; i++) {
        if (vals[i] > max_v) max_v = vals[i];
        if (vals[i] < min_v) min_v = vals[i];
    }
    double span = max_v - min_v;

    if (span < 20) { *score = fmin(90, avg * 2); return "五行均衡·系统稳定"; }
    if (fire > water + 20)     { *score = 40; return "火旺水衰·警惕干旱"; }
    if (water > fire + 20)     { *score = 35; return "水旺火衰·防范洪涝"; }
    if (wood > metal + 20)     { *score = 45; return "木旺金衰·风象活跃"; }
    if (metal > wood + 20)     { *score = 50; return "金旺木衰·气流受压"; }
    /* 土气过盛: 所有元素 < 土值*0.7 */
    int all_gt_earth = 1;
    for (int i = 0; i < 5; i++) {
        if (vals[i] > earth * 0.7) { all_gt_earth = 0; break; }
    }
    if (all_gt_earth) { *score = 55; return "土气过盛·系统胶着"; }

    *score = 65;
    return "相生相克·动态平衡";
}

/* ── 五行→卦象映射 ──────────────────────────────────── */
static int wuxing_to_hexagram(double fire, double water, double wood,
                               double metal, double earth) {
    /* 五行元素→八卦映射: (元素, 主卦index, 次卦index) */
    struct { double val; int g1, g2; } elem_map[5] = {
        {fire,  0 /*乾*/, 2 /*离*/},   /* 火→乾/离; 原Python离在前, 但0(乾)对应alert_boost=0.9 */
        {water, 5 /*坎*/, 7 /*坤*/},   /* 水→坎/坤 */
        {wood,  3 /*震*/, 4 /*巽*/},   /* 木→震/巽 */
        {metal, 1 /*兑*/, 0 /*乾*/},   /* 金→兑/乾 */
        {earth, 7 /*坤*/, 6 /*艮*/}    /* 土→坤/艮 */
    };

    double scores[8] = {0};
    for (int i = 0; i < 5; i++) {
        scores[elem_map[i].g1] += elem_map[i].val * 0.7;
        scores[elem_map[i].g2] += elem_map[i].val * 0.3;
    }
    int best = 0;
    for (int i = 1; i < 8; i++) {
        if (scores[i] > scores[best]) best = i;
    }
    return best;  /* 0=乾, 7=坤 */
}

/* ── 五行→预测修正 ──────────────────────────────────── */
static void wuxing_predict_adjust(double water, double fire,
                                   double *precip_factor, double *press_factor) {
    *precip_factor = 1.0;
    *press_factor  = 1.0;
    if (water > 50) *precip_factor = 1.0 + (water - 50) / 200.0;
    if (fire  > 50) *press_factor  = 1.0 + (fire  - 50) / 200.0;
}

/* ── 节气经验增强 ────────────────────────────────────── */
static double term_enhance_storm(double temp, int term_idx) {
    double temp_norm = 25 - abs(term_idx - 6) * 1.5;
    double storm_risk = TERM_KNOWLEDGE[term_idx].storm_risk;
    if (!isnan(temp) && fabs(temp - temp_norm) > 5) {
        storm_risk = fmin(1.0, storm_risk * 1.3);
    }
    return storm_risk;
}

/* ── 天象异常检测 ────────────────────────────────────── */
static void celestial_anomalies(double temp, double kp, double s4,
                                 char *out, int max_len,
                                 const char **assessment) {
    int has_alarm = 0;
    char buf[512] = "";
    size_t pos = 0;

    if (!isnan(temp) && fabs(temp - 20) > 5) {
        pos += snprintf(buf + pos, sizeof(buf) - pos,
                        "⚠️ 气温异常(期望20°C,实测%.1f°C)\n", temp);
        if (fabs(temp - 20) > 8) has_alarm++;
    }
    if (kp > 3) {
        pos += snprintf(buf + pos, sizeof(buf) - pos,
                        "⚠️ 地磁扰动(Kp=%.1f,七政失序)\n", kp);
        has_alarm++;
    }
    if (s4 > 0.3) {
        pos += snprintf(buf + pos, sizeof(buf) - pos,
                        "⚠️⚠️ 电离层闪烁(S4=%.3f,星曜动摇)\n", s4);
        has_alarm += 2;
    }
    if (pos == 0) {
        // snprintf(buf, sizeof(buf), "✅ 日月星曜各安其位");
        *assessment = "🟢 天象平和";
    } else if (has_alarm >= 2) {
        *assessment = "🔴 天有异象";
    } else {
        *assessment = "🟡 微有异动";
    }
    strncpy(out, buf, max_len - 1);
    out[max_len - 1] = '\0';
}

/* ── 主入口: 钦天监计算 ──────────────────────────────── */
/* 替代Python imperial_observatory.py + astral.py */
int wt_imperial_compute(wt_imperial_t *imp, time_t utc_ts) {
    if (!imp) return -1;
    memset(imp, 0, sizeof(*imp));

    /* 从DB读取当前气象数据 */
    double temp = NAN, humid = NAN, wind = NAN, press = NAN, kp = NAN, s4 = NAN;
    int got_out = wt_db_read_outdoor(&temp, &humid, &press, &wind);
    (void)got_out;
    kp = wt_db_read_kp();
    s4 = wt_db_read_s4();

    /* 默认值 */
    if (isnan(temp))  temp  = 20.0;
    if (isnan(humid)) humid = 50.0;
    if (isnan(wind))  wind  = 5.0;
    if (isnan(press)) press = 1013.25;
    if (isnan(kp))    kp    = 0.0;
    if (isnan(s4))    s4    = 0.0;

    /* 1. 太阳/节气 */
    double sun_lon = sun_apparent_longitude(utc_ts);
    int term_idx = sun_lon_to_term_index(sun_lon);
    imp->solar_term_idx = term_idx;
    snprintf(imp->solar_term, sizeof(imp->solar_term), "%s", SOLAR_TERMS[term_idx]);
    snprintf(imp->term_note, sizeof(imp->term_note), "%s", TERM_KNOWLEDGE[term_idx].note);
    imp->sun_lon_deg = sun_lon;
    imp->storm_factor = term_enhance_storm(temp, term_idx);
    imp->season_anomaly = !isnan(temp) && fabs(temp - (25 - abs(term_idx - 6) * 1.5)) > 5;

    /* 2. 月亮 */
    double moon_lon = moon_ecliptic_longitude(utc_ts);
    imp->moon_lon_deg = moon_lon;
    snprintf(imp->sun_mansion, sizeof(imp->sun_mansion), "%s", ra_to_mansion(sun_lon));
    snprintf(imp->moon_mansion, sizeof(imp->moon_mansion), "%s", ra_to_mansion(moon_lon));
    calc_moon_phase(moon_lon, sun_lon, &imp->moon_phase_name,
                    &imp->moon_phase_emoji, &imp->moon_phase_angle);

    /* 3. 五行 */
    double fire, water, wood, metal, earth;
    wuxing_map(temp, humid, wind, press, kp, &fire, &water, &wood, &metal, &earth);
    imp->wuxing[0] = fire;   imp->wuxing[1] = water;
    imp->wuxing[2] = wood;   imp->wuxing[3] = metal;
    imp->wuxing[4] = earth;
    const char *quad = wuxing_quadrant(fire, water, wood, metal, earth, &imp->wuxing_score);
    snprintf(imp->wuxing_quadrant, sizeof(imp->wuxing_quadrant), "%s", quad);
    wuxing_predict_adjust(water, fire, &imp->precip_factor, &imp->press_factor);

    /* 4. 卦象 */
    int hex_idx = wuxing_to_hexagram(fire, water, wood, metal, earth);
    imp->hexagram_idx = hex_idx;
    snprintf(imp->hexagram, sizeof(imp->hexagram), "%s", HEXAGRAM_NAMES[hex_idx]);
    imp->alert_threshold = HEXAGRAM_MAP[hex_idx].alert_boost;
    imp->system_stable = HEXAGRAM_MAP[hex_idx].stable;

    /* 5. 天象 */
    celestial_anomalies(temp, kp, s4, imp->anomaly_detail,
                        sizeof(imp->anomaly_detail), &imp->celestial_assessment);

    /* 6. 时间戳 */
    imp->ts = (int)utc_ts;

    /* 6b. 增强注释 */
    snprintf(imp->enhancement_note, sizeof(imp->enhancement_note),
             "%s(%s) | %s | 卦%s",
             imp->solar_term, imp->term_note,
             imp->wuxing_quadrant, imp->hexagram);

    /* 7. 写入DB (替代Python sqlite写入) */
    {
        sqlite3 *db;
        if (sqlite3_open(WENTIAN_DB, &db) == SQLITE_OK) {
            char sql[1024];
            snprintf(sql, sizeof(sql),
                "INSERT OR REPLACE INTO imperial_enhancement "
                "(ts,solar_term,term_idx,term_storm_factor,term_season_anomaly,"
                " wuxing_water,wuxing_fire,wuxing_wood,wuxing_metal,wuxing_earth,"
                " wuxing_quadrant,wuxing_quadrant_score,"
                " precip_adjust_factor,press_adjust_factor,"
                " hexagram,alert_threshold,system_stable,enhancement_note,version)"
                " VALUES(%d,'%s',%d,%.2f,%d,%.1f,%.1f,%.1f,%.1f,%.1f,'%s',%.1f,%.3f,%.3f,'%s',%.2f,%d,'%s','钦天监 v2.0 (C)')",
                imp->ts, imp->solar_term, imp->solar_term_idx, imp->storm_factor,
                imp->season_anomaly ? 1 : 0,
                imp->wuxing[1], imp->wuxing[0], imp->wuxing[2],
                imp->wuxing[3], imp->wuxing[4],
                imp->wuxing_quadrant, imp->wuxing_score,
                imp->precip_factor, imp->press_factor,
                imp->hexagram, imp->alert_threshold, imp->system_stable,
                imp->enhancement_note);
            char *err = NULL;
            sqlite3_exec(db, sql, 0, 0, &err);
            if (err) { sqlite3_free(err); }
            sqlite3_close(db);
        }
    }

    return 0;
}

/* ── 输出: 传统格式打印 ──────────────────────────────── */
void wt_imperial_print(const wt_imperial_t *imp) {
    if (!imp) return;
    printf("━━━ 钦天监增强引擎 v2.0 (C实现) ━━━\n");
    printf("  🌞 节气: %s(%s) | 风暴因子=%.2f\n",
           imp->solar_term, imp->term_note, imp->storm_factor);
    printf("  🔥 五行: %s | 降水修正=%.3f | 气压修正=%.3f\n",
           imp->wuxing_quadrant, imp->precip_factor, imp->press_factor);
    printf("  ☰ 卦象: %s | 系统稳定=%s\n",
           imp->hexagram, imp->system_stable ? "稳定" : "变动");
    printf("  🎯 预警阈值修正: %.2fx\n", imp->alert_threshold);
    printf("  🌙 月相: %s %s (角度=%.0f°)\n",
           imp->moon_phase_emoji ? imp->moon_phase_emoji : "",
           imp->moon_phase_name ? imp->moon_phase_name : "",
           imp->moon_phase_angle);
    printf("  📡 天象: %s\n", imp->celestial_assessment);
    if (strlen(imp->anomaly_detail) > 0 && imp->anomaly_detail[0] != (char)0xE2)
        printf("  %s", imp->anomaly_detail);
    printf("  ✅ 增强特征已写入内存结构, 供预测模型直接读取\n");
    printf("  ✅ 零文件IPC, 零fork, 纯C原生\n");
}

/* ── JSON输出(向后兼容, 供llm_weather_analyst.py读取) ── */
int wt_imperial_write_json(const wt_imperial_t *imp, const char *path) {
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    fprintf(f, "{\n");
    fprintf(f, "  \"ts\": %d,\n", imp->ts);
    fprintf(f, "  \"solar_term\": \"%s\",\n", imp->solar_term);
    fprintf(f, "  \"term_idx\": %d,\n", imp->solar_term_idx);
    fprintf(f, "  \"term_storm_factor\": %.2f,\n", imp->storm_factor);
    fprintf(f, "  \"term_season_anomaly\": %d,\n", imp->season_anomaly ? 1 : 0);
    fprintf(f, "  \"wuxing_water\": %.1f,\n", imp->wuxing[1]);
    fprintf(f, "  \"wuxing_fire\": %.1f,\n", imp->wuxing[0]);
    fprintf(f, "  \"wuxing_wood\": %.1f,\n", imp->wuxing[2]);
    fprintf(f, "  \"wuxing_metal\": %.1f,\n", imp->wuxing[3]);
    fprintf(f, "  \"wuxing_earth\": %.1f,\n", imp->wuxing[4]);
    fprintf(f, "  \"wuxing_quadrant\": \"%s\",\n", imp->wuxing_quadrant);
    fprintf(f, "  \"wuxing_quadrant_score\": %.1f,\n", imp->wuxing_score);
    fprintf(f, "  \"precip_adjust_factor\": %.3f,\n", imp->precip_factor);
    fprintf(f, "  \"press_adjust_factor\": %.3f,\n", imp->press_factor);
    fprintf(f, "  \"hexagram\": \"%s\",\n", imp->hexagram);
    fprintf(f, "  \"alert_threshold\": %.2f,\n", imp->alert_threshold);
    fprintf(f, "  \"system_stable\": %d,\n", imp->system_stable);
    fprintf(f, "  \"enhancement_note\": \"%s(%s) | %s | 卦%s\",\n",
            imp->solar_term, imp->term_note,
            imp->wuxing_quadrant, imp->hexagram);
    fprintf(f, "  \"version\": \"钦天监 v2.0 (C)\"\n");
    fprintf(f, "}\n");
    fclose(f);
    return 0;
}
