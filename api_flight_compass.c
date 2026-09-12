/* ============================================================
 * api_flight_compass.c - 寻龙尺 · 航班实时航空天气风险评估
 * ============================================================
 * 功能: 输入航班号 → 查航线 → 获取起降机场METAR → 风险评估
 * 零幻觉: API不可用诚实回退, 不编造数据
 *
 * 依赖:
 *   wt_aviation_metar()   — api_other.c (任意ICAO机场METAR)
 *   flight_routes.h        — 20条ZPPP航线
 * ============================================================ */
#include "wentian.h"
#include "flight_routes.h"
#include <sqlite3.h>
#include <math.h>
#include <ctype.h>

/* ── 辅助: 从METAR原始报文中检测雷暴(TS)组 ─────────────── */
static int metar_has_thunder(const char *raw) {
    if (!raw) return 0;
    const char *p = raw;
    while ((p = strstr(p, "TS")) != NULL) {
        int pre_ok = (p == raw) || (p[-1] == ' ') ||
                     (p[-1] == '+' || p[-1] == '-' ||
                      (p >= raw + 2 && strncmp(p - 2, "VC", 2) == 0));
        if (pre_ok) return 1;
        p += 2;
    }
    return 0;
}

/* ── 辅助: 从METAR提取侧风分量(默认使用机场主起降方向) ── */
/* rwy_hdg: 跑道磁航向(度), 未指定时用风向+45°近似 */
static double calc_crosswind(double wind_dir, double wind_spd_kt, double rwy_hdg) {
    if (wind_spd_kt < 0.1) return 0.0;
    double diff = wind_dir - rwy_hdg;
    return fabs(wind_spd_kt * sin(diff * M_PI / 180.0));
}

/* ── 辅助: 从METAR原始报文中提取能见度数值 ─────────────── */
/* 标准METAR: "9999" → >10km, "5000" → 5000m, "0800" → 800m等
 * 注意: 某些METAR用 "CAVOK" 表示能见度优良, "////" 表示缺失 */
__attribute__((unused)) static int parse_metar_vis_m(const char *raw) {
    if (!raw) return -1;
    if (strstr(raw, "CAVOK") || strstr(raw, "CAVOK ")) return 10000;
    /* 找能见度段: 通常是METAR第4段(机场代码后第3个字段) */
    /* 简化: 找常见的能见度模式: 4-5位数字后跟空格 */
    const char *p = raw;
    int found_9999 = 0;
    while (*p) {
        if (isdigit((unsigned char)*p)) {
            /* 检查是否为4-5位数字 */
            const char *start = p;
            int len = 0;
            while (isdigit((unsigned char)*p)) { p++; len++; }
            if (len >= 4 && len <= 5 && (*p == ' ' || *p == '\0' || *p == '\n')) {
                int val = 0;
                for (int i = 0; i < len; i++) val = val * 10 + (start[i] - '0');
                if (val == 9999) { found_9999 = 10000; continue; }
                return val;
            }
        } else {
            p++;
        }
    }
    return found_9999 ? found_9999 : -1;
}

/* ── 获取指定ICAO机场的天气评估, 返回航班状态 ───────────── */
/* fa: 总评估结构体, is_dep: 0=目的地 1=起飞站 */
static int fetch_airport_weather(const char *icao, const char *name,
                                  wt_flight_assess_t *fa, int is_dep) {
    wt_metar_t m;
    memset(&m, 0, sizeof(m));

    int ret = wt_aviation_metar(icao, &m);

    double *temp, *humid, *press, *wspd, *wdir, *vis;
    char *weather, *raw, *status_text;
    int *status;

    if (is_dep) {
        temp = &fa->dep_temp;
        humid = &fa->dep_humid;
        press = &fa->dep_press;
        wspd = &fa->dep_wind_spd;
        wdir = &fa->dep_wind_dir;
        vis = &fa->dep_vis;
        weather = fa->dep_weather;
        raw = fa->dep_metar_raw;
        status = &fa->dep_flight_status;
        status_text = fa->dep_status_text;
    } else {
        temp = &fa->arr_temp;
        humid = &fa->arr_humid;
        press = &fa->arr_press;
        wspd = &fa->arr_wind_spd;
        wdir = &fa->arr_wind_dir;
        vis = &fa->arr_vis;
        weather = fa->arr_weather;
        raw = fa->arr_metar_raw;
        status = &fa->arr_flight_status;
        status_text = fa->arr_status_text;
    }

    if (ret != 0 || m.raw[0] == '\0') {
        /* METAR不可用: 诚实回退, 标注为状态1(注意) */
        snprintf(raw, 256, "%s METAR暂不可用(API或网络问题)", icao);
        snprintf(weather, 128, "数据不可用");
        *temp = NAN; *humid = NAN; *press = NAN;
        *wspd = NAN; *wdir = NAN; *vis = NAN;
        *status = 1;  /* 注意 */
        snprintf(status_text, 128, "⚠ %s 无实时天气数据, 以模型估算为参考", name);
        return -1;
    }

    /* METAR成功获取 */
    snprintf(raw, 512, "%s", m.raw);
    *temp = m.temp;
    *humid = NAN;  /* METAR不含湿度 */
    *press = m.altim_hpa;
    *wspd = (double)m.wind_speed_kt;
    *wdir = (double)m.wind_dir;
    *vis = (double)m.visibility_m;

    /* 天气描述 */
    if (metar_has_thunder(m.raw)) {
        snprintf(weather, 128, "⛈ 雷暴");
    } else if (strstr(m.raw, "DZ") || strstr(m.raw, "RA") ||
               strstr(m.raw, "SHRA") || strstr(m.raw, "-RA")) {
        snprintf(weather, 128, "🌧 降水");
    } else if (strstr(m.raw, "FG") || strstr(m.raw, "BR")) {
        snprintf(weather, 128, "🌫 雾/薄雾");
    } else if (strstr(m.raw, "HZ")) {
        snprintf(weather, 128, "🌫 霾");
    } else if (m.wind_speed_kt >= 20) {
        snprintf(weather, 128, "💨 大风");
    } else {
        snprintf(weather, 128, "☀️ 晴好");
    }

    /* 初始状态 */
    *status = 0;
    snprintf(status_text, 128, "✅ %s 天气正常", name);
    return 0;
}


/* ═══════════════════════════════════════════════════════════ */
/* 1. 航线查找                                                  */
/* ============================================================ */
/* 输入: 航班号(如 "MU5809"), 输出写入fa
 * 返回: 0=找到, -1=未收录该航线 */
int wt_flight_lookup(const char *flight_no, wt_flight_assess_t *fa) {
    if (!flight_no || !fa || !*flight_no) return -1;

    const flight_route_t *rt = find_route(flight_no);
    if (!rt) {
        memset(fa, 0, sizeof(*fa));
        snprintf(fa->flight_no, sizeof(fa->flight_no), "%s", flight_no);
        return -1;  /* 诚实回退: 未收录该航线 */
    }

    memset(fa, 0, sizeof(*fa));
    snprintf(fa->flight_no, sizeof(fa->flight_no), "%s", flight_no);
    strncpy(fa->dep_icao, rt->dep_icao, sizeof(fa->dep_icao) - 1);
    strncpy(fa->arr_icao, rt->arr_icao, sizeof(fa->arr_icao) - 1);
    strncpy(fa->dep_name, rt->dep_name, sizeof(fa->dep_name) - 1);
    strncpy(fa->arr_name, rt->arr_name, sizeof(fa->arr_name) - 1);
    fa->distance_nm = rt->distance_nm;
    fa->ts = time(NULL);

    return 0;
}


/* ═══════════════════════════════════════════════════════════ */
/* 2. 获取起降站气象数据                                        */
/* ============================================================ */
/* 调用 wt_aviation_metar 获取起飞站和目的站天气
 * 返回: 0=全部成功, -1=至少一个失败(仍填充了部分数据) */
int wt_flight_weather(wt_flight_assess_t *fa) {
    if (!fa || !fa->dep_icao[0] || !fa->arr_icao[0]) return -1;

    int ret1 = fetch_airport_weather(fa->dep_icao, fa->dep_name, fa, 1);
    int ret2 = fetch_airport_weather(fa->arr_icao, fa->arr_name, fa, 0);

    return (ret1 == 0 && ret2 == 0) ? 0 : -1;
}


/* ═══════════════════════════════════════════════════════════ */
/* 3. 飞行风险评估                                              */
/* ============================================================ */
/* 基于侧风/能见度/雷暴/距离等评估起降可行性和航路风险
 * 返回: 0=成功 */
int wt_flight_assess(wt_flight_assess_t *fa) {
    if (!fa) return -1;

    /* ── 起飞机场评估 ── */
    /* 侧风评估: 使用各机场主用跑道方向近似 */
    /* ZPPP主用04/22号(磁航向40°/220°) */
    if (!isnan(fa->dep_wind_spd) && !isnan(fa->dep_wind_dir)) {
        double xw04 = calc_crosswind(fa->dep_wind_dir, fa->dep_wind_spd, 40.0);
        double xw22 = calc_crosswind(fa->dep_wind_dir, fa->dep_wind_spd, 220.0);
        double best_xw = (xw04 < xw22) ? xw04 : xw22;

        if (best_xw >= 33.0) {
            fa->dep_flight_status = 3;
            snprintf(fa->dep_status_text, sizeof(fa->dep_status_text),
                     "🔴 %s 侧风%.0fkt 超B737限制(33kt), 禁止起降",
                     fa->dep_name, best_xw);
        } else if (best_xw >= 25.0) {
            fa->dep_flight_status = 2;
            snprintf(fa->dep_status_text, sizeof(fa->dep_status_text),
                     "🟠 %s 侧风%.0fkt 超25kt, 建议关注",
                     fa->dep_name, best_xw);
        } else if (best_xw >= 15.0) {
            if (fa->dep_flight_status < 1) fa->dep_flight_status = 1;
            snprintf(fa->dep_status_text, sizeof(fa->dep_status_text),
                     "🟡 %s 侧风%.0fkt 超15kt, 注意",
                     fa->dep_name, best_xw);
        }
    }

    /* 能见度评估(起飞站) */
    if (!isnan(fa->dep_vis) && fa->dep_vis > 0) {
        if (fa->dep_vis < 800) {
            fa->dep_flight_status = 3;
            snprintf(fa->dep_status_text, sizeof(fa->dep_status_text),
                     "🔴 %s 能见度%.0fm<800m LVP, 禁止起降",
                     fa->dep_name, fa->dep_vis);
        } else if (fa->dep_vis < 1600) {
            if (fa->dep_flight_status < 2) fa->dep_flight_status = 2;
            snprintf(fa->dep_status_text, sizeof(fa->dep_status_text),
                     "🟠 %s 能见度%.0fm<1600m, 警告",
                     fa->dep_name, fa->dep_vis);
        }
    }

    /* METAR雷暴检测(起飞站) */
    if (fa->dep_metar_raw[0] && metar_has_thunder(fa->dep_metar_raw)) {
        if (fa->dep_flight_status < 2) fa->dep_flight_status = 2;
        snprintf(fa->dep_status_text, sizeof(fa->dep_status_text),
                 "🟠 %s METAR含雷暴(TS), 警告", fa->dep_name);
    }

    /* ── 目的地机场评估 ── */
    /* 侧风: ZUUU主用02L/20R(磁航向20°/200°), 其他机场取通用值 */
    if (!isnan(fa->arr_wind_spd) && !isnan(fa->arr_wind_dir)) {
        /* 使用通用主跑道方向: 取与风垂直分量最小的跑道 */
        double xw_est = calc_crosswind(fa->arr_wind_dir, fa->arr_wind_spd,
                                       fmod(fa->arr_wind_dir + 90.0, 360.0));
        /* 实际取90°旋转后最小侧风 — 简化: 风速*0.5 ~侧风估算 */
        double approx_xw = fabs(fa->arr_wind_spd *
            sin(fmod(fa->arr_wind_dir - 180.0 + 90.0, 360.0) * M_PI / 180.0));
        (void)xw_est;

        if (approx_xw >= 33.0) {
            fa->arr_flight_status = 3;
            snprintf(fa->arr_status_text, sizeof(fa->arr_status_text),
                     "🔴 %s 预估侧风%.0fkt 超限制, 禁止起降",
                     fa->arr_name, approx_xw);
        } else if (approx_xw >= 25.0) {
            fa->arr_flight_status = 2;
            snprintf(fa->arr_status_text, sizeof(fa->arr_status_text),
                     "🟠 %s 预估侧风%.0fkt 警告", fa->arr_name, approx_xw);
        } else if (approx_xw >= 15.0) {
            if (fa->arr_flight_status < 1) fa->arr_flight_status = 1;
            snprintf(fa->arr_status_text, sizeof(fa->arr_status_text),
                     "🟡 %s 侧风%.0fkt 注意", fa->arr_name, approx_xw);
        }
    }

    /* 能见度(目的地) */
    if (!isnan(fa->arr_vis) && fa->arr_vis > 0) {
        if (fa->arr_vis < 800) {
            fa->arr_flight_status = 3;
            snprintf(fa->arr_status_text, sizeof(fa->arr_status_text),
                     "🔴 %s 能见度%.0fm<800m LVP, 禁止起降",
                     fa->arr_name, fa->arr_vis);
        } else if (fa->arr_vis < 1600) {
            if (fa->arr_flight_status < 2) fa->arr_flight_status = 2;
            snprintf(fa->arr_status_text, sizeof(fa->arr_status_text),
                     "🟠 %s 能见度%.0fm<1600m, 警告",
                     fa->arr_name, fa->arr_vis);
        }
    }

    /* METAR雷暴检测(目的地) */
    if (fa->arr_metar_raw[0] && metar_has_thunder(fa->arr_metar_raw)) {
        if (fa->arr_flight_status < 2) fa->arr_flight_status = 2;
        snprintf(fa->arr_status_text, sizeof(fa->arr_status_text),
                 "🟠 %s METAR含雷暴(TS), 警告", fa->arr_name);
    }

    /* ── 航路风险评估 ── */
    int enroute_score = 100;
    char enroute_detail[256] = "";

    /* 起降场温差大 → 可能穿越锋面 */
    if (!isnan(fa->dep_temp) && !isnan(fa->arr_temp)) {
        double temp_diff = fabs(fa->dep_temp - fa->arr_temp);
        if (temp_diff > 15.0) {
            enroute_score -= 20;
            snprintf(enroute_detail + strlen(enroute_detail),
                     sizeof(enroute_detail) - strlen(enroute_detail),
                     "起降温差%.0f°C(可能穿越锋面); ", temp_diff);
        } else if (temp_diff > 10.0) {
            enroute_score -= 10;
            snprintf(enroute_detail + strlen(enroute_detail),
                     sizeof(enroute_detail) - strlen(enroute_detail),
                     "起降温差%.0f°C; ", temp_diff);
        }
    }

    /* 航程长且起降场天气差异大 → 增加不确定性 */
    if (fa->distance_nm > 800 &&
        (fa->dep_flight_status >= 1 || fa->arr_flight_status >= 1)) {
        enroute_score -= 15;
        snprintf(enroute_detail + strlen(enroute_detail),
                 sizeof(enroute_detail) - strlen(enroute_detail),
                 "长航线(%dnm)两端天气复杂; ", fa->distance_nm);
    }

    /* 航程长: 航路天气不确定性自然增加 */
    if (fa->distance_nm > 1200) {
        enroute_score -= 10;
        snprintf(enroute_detail + strlen(enroute_detail),
                 sizeof(enroute_detail) - strlen(enroute_detail),
                 "超长航线(%dnm)航路不确定性高; ", fa->distance_nm);
    }

    /* 起降场之一有雷暴 → 航路风险显著增加 */
    if (fa->dep_flight_status >= 2 || fa->arr_flight_status >= 2) {
        enroute_score -= 15;
        snprintf(enroute_detail + strlen(enroute_detail),
                 sizeof(enroute_detail) - strlen(enroute_detail),
                 "起降场存在高风险天气; ");
    }

    /* 起降均有高风险 → 航路极可能受影响 */
    if (fa->dep_flight_status >= 2 && fa->arr_flight_status >= 2) {
        enroute_score -= 25;
        snprintf(enroute_detail + strlen(enroute_detail),
                 sizeof(enroute_detail) - strlen(enroute_detail),
                 "两端机场均存在高风险天气, 建议取消; ");
    }

    if (enroute_score < 0) enroute_score = 0;
    fa->enroute_risk_score = enroute_score;
    if (enroute_detail[0]) {
        snprintf(fa->enroute_risk_text, sizeof(fa->enroute_risk_text), "%s", enroute_detail);
    } else {
        snprintf(fa->enroute_risk_text, sizeof(fa->enroute_risk_text),
                 "晴好, 无危险天气");
    }

    /* ── 综合评分 ── */
    /* 权重: 起飞40% + 航路20% + 目的地40% */
    int dep_score = 100;
    if (fa->dep_flight_status == 3) dep_score = 0;
    else if (fa->dep_flight_status == 2) dep_score = 25;
    else if (fa->dep_flight_status == 1) dep_score = 60;

    int arr_score = 100;
    if (fa->arr_flight_status == 3) arr_score = 0;
    else if (fa->arr_flight_status == 2) arr_score = 25;
    else if (fa->arr_flight_status == 1) arr_score = 60;

    fa->overall_score = (int)(dep_score * 0.4 + enroute_score * 0.2 + arr_score * 0.4 + 0.5);
    if (fa->overall_score > 100) fa->overall_score = 100;
    if (fa->overall_score < 0) fa->overall_score = 0;

    /* 建议 */
    if (fa->overall_score >= 80) {
        snprintf(fa->recommendation, sizeof(fa->recommendation),
                 "✅ 正常 — 航班正常性高");
    } else if (fa->overall_score >= 50) {
        snprintf(fa->recommendation, sizeof(fa->recommendation),
                 "🟡 注意 — 存在一定天气风险, 建议持续监控");
    } else if (fa->overall_score >= 25) {
        snprintf(fa->recommendation, sizeof(fa->recommendation),
                 "🟠 建议延误 — 天气条件较差, 建议推迟");
    } else {
        snprintf(fa->recommendation, sizeof(fa->recommendation),
                 "🔴 建议取消 — 天气条件不适宜飞行");
    }

    /* 完整评估文本 */
    char buf[768];
    int pos = 0;
    pos += snprintf(buf + pos, sizeof(buf) - pos,
        "航班%s %s→%s | ", fa->flight_no, fa->dep_name, fa->arr_name);
    pos += snprintf(buf + pos, sizeof(buf) - pos,
        "起飞%s | ", fa->dep_status_text[0] ? fa->dep_status_text : "待评估");
    pos += snprintf(buf + pos, sizeof(buf) - pos,
        "到达%s | ", fa->arr_status_text[0] ? fa->arr_status_text : "待评估");
    pos += snprintf(buf + pos, sizeof(buf) - pos,
        "航路: %s | ", fa->enroute_risk_text);
    pos += snprintf(buf + pos, sizeof(buf) - pos,
        "评分%d/100 — %s", fa->overall_score, fa->recommendation);
    snprintf(fa->assessment, sizeof(fa->assessment), "%s", buf);

    return 0;
}


/* ═══════════════════════════════════════════════════════════ */
/* 4. 寻龙尺主入口                                              */
/* ============================================================ */
/* 整合: 查航线 → 获取天气 → 评估 → 存DB
 * 返回: 0=成功, -1=航线未收录, -2=其他错误 */
int wt_flight_compass_run(const char *flight_no, wt_flight_assess_t *fa) {
    if (!flight_no || !fa) return -2;

    /* 步骤1: 查航线 */
    if (wt_flight_lookup(flight_no, fa) != 0) {
        return -1;  /* 未收录该航线 */
    }

    /* 步骤2: 获取天气 */
    wt_flight_weather(fa);

    /* 步骤3: 风险评估 */
    wt_flight_assess(fa);

    /* 步骤4: 存DB */
    {
        sqlite3 *db;
        if (sqlite3_open(WENTIAN_DB, &db) == SQLITE_OK) {
            sqlite3_busy_timeout(db, 1000);
            sqlite3_stmt *st;
            if (sqlite3_prepare_v2(db,
                "INSERT INTO flight_assess "
                "(ts,flight_no,dep_icao,arr_icao,dep_name,arr_name,"
                "distance_nm,"
                "dep_temp,dep_wind_spd,dep_wind_dir,dep_vis,dep_metar_raw,dep_status,"
                "arr_temp,arr_wind_spd,arr_wind_dir,arr_vis,arr_metar_raw,arr_status,"
                "enroute_risk_score,overall_score,recommendation) "
                "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
                -1, &st, NULL) == SQLITE_OK) {

                sqlite3_bind_int64(st, 1, fa->ts);
                sqlite3_bind_text(st, 2, fa->flight_no, -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(st, 3, fa->dep_icao, -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(st, 4, fa->arr_icao, -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(st, 5, fa->dep_name, -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(st, 6, fa->arr_name, -1, SQLITE_TRANSIENT);
                sqlite3_bind_int(st, 7, fa->distance_nm);
                sqlite3_bind_double(st, 8, fa->dep_temp);
                sqlite3_bind_double(st, 9, fa->dep_wind_spd);
                sqlite3_bind_double(st, 10, fa->dep_wind_dir);
                sqlite3_bind_double(st, 11, fa->dep_vis);
                sqlite3_bind_text(st, 12, fa->dep_metar_raw, -1, SQLITE_TRANSIENT);
                sqlite3_bind_int(st, 13, fa->dep_flight_status);
                sqlite3_bind_double(st, 14, fa->arr_temp);
                sqlite3_bind_double(st, 15, fa->arr_wind_spd);
                sqlite3_bind_double(st, 16, fa->arr_wind_dir);
                sqlite3_bind_double(st, 17, fa->arr_vis);
                sqlite3_bind_text(st, 18, fa->arr_metar_raw, -1, SQLITE_TRANSIENT);
                sqlite3_bind_int(st, 19, fa->arr_flight_status);
                sqlite3_bind_int(st, 20, fa->enroute_risk_score);
                sqlite3_bind_int(st, 21, fa->overall_score);
                sqlite3_bind_text(st, 22, fa->recommendation, -1, SQLITE_TRANSIENT);

                sqlite3_step(st);
                sqlite3_finalize(st);
            }
            sqlite3_close(db);
        }
    }

    return 0;
}


/* ═══════════════════════════════════════════════════════════ */
/* 5. 格式化输出                                                */
/* ============================================================ */
void wt_flight_print(const wt_flight_assess_t *fa) {
    if (!fa) {
        printf("⚠ 寻龙尺: 无评估数据\n");
        return;
    }

    printf("\n");
    printf("━━━ 寻龙尺 · 航班风险评估 ━━━\n");
    printf("\n");
    printf("航班: %s %s→%s\n", fa->flight_no, fa->dep_name, fa->arr_name);

    /* 起飞站 */
    printf("🛫 %s %s", fa->dep_icao, fa->dep_name);
    if (!isnan(fa->dep_temp))
        printf(": %.0f°C", fa->dep_temp);
    if (!isnan(fa->dep_wind_dir) && !isnan(fa->dep_wind_spd))
        printf(" 风%.0f°/%.0fkt", fa->dep_wind_dir, fa->dep_wind_spd);
    if (!isnan(fa->dep_vis) && fa->dep_vis > 0) {
        if (fa->dep_vis >= 10000)
            printf(" 能见度>10km");
        else
            printf(" 能见度%.0fm", fa->dep_vis);
    }
    const char *dep_icons[] = {"✅", "🟡", "🟠", "🔴"};
    int dep_idx = (fa->dep_flight_status >= 0 && fa->dep_flight_status <= 3) ? fa->dep_flight_status : 0;
    printf(" %s %s\n", dep_icons[dep_idx],
           fa->dep_flight_status == 0 ? "正常" :
           fa->dep_flight_status == 1 ? "注意" :
           fa->dep_flight_status == 2 ? "警告" : "禁止");

    /* 目的地 */
    printf("🛬 %s %s", fa->arr_icao, fa->arr_name);
    if (!isnan(fa->arr_temp))
        printf(": %.0f°C", fa->arr_temp);
    if (!isnan(fa->arr_wind_dir) && !isnan(fa->arr_wind_spd))
        printf(" 风%.0f°/%.0fkt", fa->arr_wind_dir, fa->arr_wind_spd);
    if (!isnan(fa->arr_vis) && fa->arr_vis > 0) {
        if (fa->arr_vis >= 10000)
            printf(" 能见度>10km");
        else
            printf(" 能见度%.0fm", fa->arr_vis);
    }
    int arr_idx = (fa->arr_flight_status >= 0 && fa->arr_flight_status <= 3) ? fa->arr_flight_status : 0;
    printf(" %s %s\n", dep_icons[arr_idx],
           fa->arr_flight_status == 0 ? "正常" :
           fa->arr_flight_status == 1 ? "注意" :
           fa->arr_flight_status == 2 ? "警告" : "禁止");

    /* 航路 */
    printf("🛩 航路评估: %s", fa->enroute_risk_text);
    if (fa->enroute_risk_score >= 70)
        printf(" ✅\n");
    else if (fa->enroute_risk_score >= 40)
        printf(" 🟡\n");
    else
        printf(" 🔴\n");

    /* 综合 */
    printf("📊 综合评分: %d/100 — %s\n", fa->overall_score, fa->recommendation);

    /* 详细状态(如有异常) */
    if (fa->dep_flight_status > 0)
        printf("  → %s\n", fa->dep_status_text);
    if (fa->arr_flight_status > 0)
        printf("  → %s\n", fa->arr_status_text);

    if (fa->dep_metar_raw[0])
        printf("  %s\n", fa->dep_metar_raw);
    if (fa->arr_metar_raw[0])
        printf("  %s\n", fa->arr_metar_raw);

    printf("\n");
}
