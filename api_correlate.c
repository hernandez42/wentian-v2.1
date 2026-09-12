/* ============================================================
 * api_correlate.c - 问天软件雷达 · 三路相干引擎 v2.0
 * ============================================================
 * 项目: 问天 (WenTian Weather Station)
 * 所有者: 主人朱涛 BG8SBA, 昆明长水机场楼顶
 * 锁定: 详见 WENTIAN-LOCK.md, 非授权LLM请勿擅改
 *
 * v2.0 (2026-09-12) 重构 — 修复"软件雷达识别 UNKNOWN"泛滥链:
 *   1. SDR: 绝对SNR>15dB永久假触发(聚合SQL扫全表历史取到旧极值)
 *      → 6h滚动基线 + 5dB突变判定, 15min新鲜度门槛
 *   2. GNSS: PWV误读pwv_history.csv第2列(机箱温度~25.5°C)当PWV
 *      → 改读local_pwv表真实pwv_mm + 15min斜率
 *   3. UNO: 5行≈2min窗口把±0.1hPa量化噪声放大成假变化率
 *      → 16min窗口取首尾真实时间戳, 跨度≥8min才计算
 *   4. match_pattern: 决策表有空洞(sdr+uno触发组合全落UNKNOWN)
 *      → 完整决策表 + 方向判据(气压降+水汽升=雷暴, 气压升+水汽降=飑线)
 *   5. corr_*异构特征Pearson相关(无物理意义, 技能清单已知造假项)
 *      → 删除, 无任何下游消费者
 *   6. 置信度: STATIONARY=稳定一致率×水汽饱和度(可解释),
 *      其他型=异常源占比×模式因子
 *
 * 三路信号:
 *   SDR: GNSS L1频谱SNR突变 (射频层, 相对6h滚动基线)
 *   GNSS: PWV水位/急升 + S4电离层闪烁 (信号层)
 *   UNO: 气压/温度变化率 (地面层, ≥10min真实跨度)
 *
 * 阈值依据(2026-09-12实测校准):
 *   PWV异常: 昆明雨季14天local_pwv均值44.0mm/极值51.6mm → ≥48为异常高值
 *   PWV斜率: 单样本|Δ|均值0.057mm → 15min斜率≥1.5mm为急升
 *   UNO气压: BMP量化0.1hPa, ≥8min跨度下噪声<0.15hPa/10min,
 *            阈值0.5hPa/10min滤噪声且捕获锋面过境(1-3hPa/10min)
 *   SDR: GNSS L1常态SNR 9-14dB → 基线+5dB为真实信号增强
 *   静止锋确认: 三路全静默 + PWV≥45mm(14天均值之上) + 斜率平稳
 *
 * 特征向量布局(用于事后分析, 存struct):
 *   sdr[16]:  [0]噪底 [1]峰值MHz [2]峰值dBm [3]当前SNR
 *             [4]6h基线SNR [5]基线样本数 [6]SNR增量 [7-15]保留
 *   gnss[8]:  [0]PWV [1]PWV15min斜率 [2]S4_GPS [3]S4_BDS
 *             [4]GPS_SNR [5]BDS_SNR [6]总卫星数 [7]PDOP
 *   uno[6]:   [0]站压 [1]温度 [2]湿度 [3]海压
 *             [4]气压变化率hPa/10min [5]温度变化率°C/10min
 * ============================================================ */
#include "wentian.h"
#include <sqlite3.h>
#include <math.h>

#define CORREL_JSON          WENTIAN_FUSION_DIR "/radar_correlation.json"
#define CORREL_TABLE         "radar_correl"

/* ── 特征提取窗口(分钟) ──────────────────────────────────── */
#define CORR_WINDOW_MIN      15     /* 滑动窗长度(默认参数) */

/* ── 异常判定阈值(校准依据见文件头) ──────────────────────── */
#define CORR_PRESS_THR       0.5    /* hPa/10min, 气压变化显著 */
#define CORR_TEMP_THR        1.0    /* °C/10min, 温度变化显著 */
#define CORR_S4_THR          0.3    /* 电离层闪烁显著阈值 */
#define CORR_SDR_DELTA_THR   5.0    /* dB, 当前SNR超出6h基线的突变量 */
#define CORR_SDR_FRESH_SEC   900    /* s, 扫频行新鲜度门槛(15min) */
#define CORR_SDR_BASE_SEC    21600  /* s, 滚动基线窗口(6h) */
#define CORR_SDR_BASE_MIN_N  6      /* 基线最少样本数, 不足不判定 */
#define PWV_ANOM_THR         48.0   /* mm, PWV异常高值(14天均值44/极值51.6) */
#define PWV_SLOPE_THR        1.5    /* mm/15min, PWV急升 */
#define STATIONARY_PWV_THR   45.0   /* mm, 静止锋"持续高湿"下限 */
#define UNO_SPAN_MIN         8.0    /* min, 变化率最小可信跨度 */

/* ── 模式匹配函数声明(下面定义) ──────────────────────────── */
static int match_pattern(const wt_radar_correl_t *c);
static int load_sdr_features(time_t ts, int span_min, double *feat, int n);
static int load_gnss_features(time_t ts, int span_min, double *feat, int n);
static int load_uno_features(time_t ts, int span_min, double *feat, int n);

/* ── 加载SDR特征 ────────────────────────────────────────── */
/* v2.0: 不再用绝对SNR阈值和带聚合的错误SQL(旧查询LIMIT对聚合无效,
 * MAX扫全表历史取到9月2日旧极值27dB → sdr_active永久=1)。 */
static int load_sdr_features(time_t ts, int span_min, double *feat, int n) {
    (void)ts; (void)span_min; (void)n;
    sqlite3 *db;
    if (sqlite3_open(WENTIAN_DB, &db) != SQLITE_OK) return -1;

    /* 1. 最新一条新鲜扫频行(≤15min, 排除Inf/NaN污染行) */
    sqlite3_stmt *st;
    int have_cur = 0;
    if (sqlite3_prepare_v2(db,
        "SELECT noise_dbm, peak_mhz, peak_dbm, peak_snr FROM local_sdr "
        "WHERE ts > strftime('%s','now') - 900 "
        "AND peak_snr > -50 AND peak_snr < 1000000 "
        "ORDER BY ts DESC LIMIT 1", -1, &st, NULL) == SQLITE_OK) {
        if (sqlite3_step(st) == SQLITE_ROW) {
            feat[0] = sqlite3_column_double(st, 0);  /* noise floor */
            feat[1] = sqlite3_column_double(st, 1);  /* peak freq */
            feat[2] = sqlite3_column_double(st, 2);  /* peak dbm */
            feat[3] = sqlite3_column_double(st, 3);  /* current SNR */
            have_cur = 1;
        }
        sqlite3_finalize(st);
    }
    if (!have_cur) { sqlite3_close(db); return -1; }

    /* 2. 6小时滚动基线(排除最近15min防自污染) */
    int base_n = 0;
    double base_avg = 0.0;
    if (sqlite3_prepare_v2(db,
        "SELECT COUNT(*), AVG(peak_snr) FROM local_sdr "
        "WHERE ts BETWEEN strftime('%s','now') - 21600 "
        "AND strftime('%s','now') - 900 "
        "AND peak_snr > -50 AND peak_snr < 1000000",
        -1, &st, NULL) == SQLITE_OK) {
        if (sqlite3_step(st) == SQLITE_ROW) {
            base_n = sqlite3_column_int(st, 0);
            base_avg = sqlite3_column_double(st, 1);
        }
        sqlite3_finalize(st);
    }
    sqlite3_close(db);

    feat[4] = base_avg;                 /* 6h基线SNR */
    feat[5] = (double)base_n;           /* 基线样本数 */
    feat[6] = feat[3] - base_avg;       /* SNR增量 */
    return 0;
}

/* ── 加载GNSS特征 ───────────────────────────────────────── */
/* v2.0: PWV改读local_pwv表(旧读pwv_history.csv用strchr取到的是
 * 第2列T_c机箱温度~25.5°C, 真PWV在第9列 → 异常判据永不触发)。 */
static int load_gnss_features(time_t ts, int span_min, double *feat, int n) {
    (void)ts; (void)span_min; (void)n;
    sqlite3 *db;
    if (sqlite3_open(WENTIAN_DB, &db) != SQLITE_OK) return -1;

    /* 1. PWV: local_pwv表(1h新鲜), 最新值 + 15min归一斜率 */
    int pwv_ok = 0;
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(db,
        "SELECT pwv_mm, ts FROM local_pwv "
        "WHERE ts > strftime('%s','now') - 3600 "
        "ORDER BY ts DESC LIMIT 4", -1, &st, NULL) == SQLITE_OK) {
        double pwv0 = 0, ts0 = 0, pwvN = 0, tsN = 0;
        int cnt = 0;
        while (sqlite3_step(st) == SQLITE_ROW && cnt < 4) {
            double v = sqlite3_column_double(st, 0);
            double t = sqlite3_column_double(st, 1);
            if (cnt == 0) { pwv0 = v; ts0 = t; }
            pwvN = v; tsN = t;
            cnt++;
        }
        sqlite3_finalize(st);
        if (cnt >= 1 && pwv0 > 0) {
            feat[0] = pwv0;
            pwv_ok = 1;
            if (cnt >= 2 && ts0 > tsN) {
                double dt_min = (ts0 - tsN) / 60.0;
                if (dt_min >= 10.0)
                    feat[1] = (pwv0 - pwvN) / dt_min * 15.0;  /* mm/15min */
            }
        }
    }

    /* 2. S4/SNR/PDOP: local_ionosphere(2h新鲜过滤) */
    int iono_ok = 0;
    if (sqlite3_prepare_v2(db,
        "SELECT s4_gps, s4_bds, gps_snr_avg, bds_snr_avg, "
        "samples_gps, samples_bds, pdop_avg "
        "FROM local_ionosphere "
        "WHERE ts > strftime('%s','now') - 7200 "
        "ORDER BY ts DESC LIMIT 1", -1, &st, NULL) == SQLITE_OK) {
        if (sqlite3_step(st) == SQLITE_ROW) {
            feat[2] = sqlite3_column_double(st, 0);  /* s4_gps */
            feat[3] = sqlite3_column_double(st, 1);  /* s4_bds */
            feat[4] = sqlite3_column_double(st, 2);  /* gps_snr_avg */
            feat[5] = sqlite3_column_double(st, 3);  /* bds_snr_avg */
            feat[6] = sqlite3_column_double(st, 4) + sqlite3_column_double(st, 5);
            feat[7] = sqlite3_column_double(st, 6);  /* pdop_avg */
            iono_ok = 1;
        }
        sqlite3_finalize(st);
    }
    sqlite3_close(db);
    return (pwv_ok || iono_ok) ? 0 : -1;
}

/* ── 加载UNO特征 ────────────────────────────────────────── */
/* v2.0: 16min窗口取首尾(旧实现LIMIT 5仅跨~2min, ±0.1hPa量化噪声
 * 被10/dt放大成3hPa/10min假信号 → uno_pressure随机乱闪)。
 * ts为TEXT本地时间, 用replace统一格式后字符串比较过滤新鲜度;
 * 变化率用strftime('%s')差值(偏移在差分中抵消)。 */
static int load_uno_features(time_t ts, int span_min, double *feat, int n) {
    (void)ts; (void)span_min; (void)n;
    sqlite3 *db;
    /* ano_weather 表在主人硬件库, 不在 wentian.db */
    if (sqlite3_open(ANO_DB, &db) != SQLITE_OK) return -1;

    double cur_t = 0, cur_h = 0, cur_p = 0, cur_pa = 0;
    double old_p = 0, old_t = 0, old_ts = 0, cur_ts = 0;
    int have_cur = 0, have_old = 0;

    /* 1. 最新一条(30min内) */
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(db,
        "SELECT t, h, p, pa, strftime('%s', ts) FROM ano_weather "
        "WHERE source='UNO_v2.0_bridge' "
        "AND replace(ts,'T',' ') >= datetime('now','localtime','-30 minutes') "
        "ORDER BY ts DESC LIMIT 1", -1, &st, NULL) == SQLITE_OK) {
        if (sqlite3_step(st) == SQLITE_ROW) {
            cur_t  = sqlite3_column_double(st, 0);
            cur_h  = sqlite3_column_double(st, 1);
            cur_p  = sqlite3_column_double(st, 2);
            cur_pa = sqlite3_column_double(st, 3);
            cur_ts = sqlite3_column_double(st, 4);
            have_cur = 1;
        }
        sqlite3_finalize(st);
    }
    if (!have_cur) { sqlite3_close(db); return -1; }

    /* 2. 16min窗口的最早一条(变化率锚点) */
    if (sqlite3_prepare_v2(db,
        "SELECT t, p, strftime('%s', ts) FROM ano_weather "
        "WHERE source='UNO_v2.0_bridge' "
        "AND replace(ts,'T',' ') >= datetime('now','localtime','-16 minutes') "
        "ORDER BY ts ASC LIMIT 1", -1, &st, NULL) == SQLITE_OK) {
        if (sqlite3_step(st) == SQLITE_ROW) {
            old_t  = sqlite3_column_double(st, 0);
            old_p  = sqlite3_column_double(st, 1);
            old_ts = sqlite3_column_double(st, 2);
            have_old = 1;
        }
        sqlite3_finalize(st);
    }
    sqlite3_close(db);

    feat[0] = cur_p;    /* 当前站压 */
    feat[1] = cur_t;    /* 当前温度 */
    feat[2] = cur_h;    /* 当前湿度 */
    feat[3] = cur_pa;   /* 当前海平面气压 */

    /* 3. 变化率: 首尾真实时间戳, 跨度≥8min才可信 */
    if (have_old && cur_ts > old_ts) {
        double dt_min = (cur_ts - old_ts) / 60.0;
        if (dt_min >= UNO_SPAN_MIN) {
            feat[4] = (cur_p - old_p) * 10.0 / dt_min;  /* hPa/10min */
            feat[5] = (cur_t - old_t) * 10.0 / dt_min;  /* °C/10min */
        }
    }
    return 0;
}

/* ── 模式匹配 ───────────────────────────────────────────── */
/* v2.0: 完整决策表(旧表sdr+uno等触发组合无分支全落UNKNOWN)。
 * 方向判据: 雷暴=气压降+水汽升, 飑线=气压升+水汽降。 */
static int match_pattern(const wt_radar_correl_t *c) {
    const double p_rate    = c->uno_features[4];   /* hPa/10min */
    const double pwv       = c->gnss_features[0];  /* mm */
    const double pwv_slope = c->gnss_features[1];  /* mm/15min */

    /* 三路异常同向 → 按变化方向分辨雷暴/飑线 */
    if (c->sdr_active && c->gnss_anomaly && c->uno_pressure) {
        if (p_rate < -CORR_PRESS_THR && pwv_slope > 0) return WT_PATTERN_THUNDER;
        if (p_rate >  CORR_PRESS_THR && pwv_slope < 0) return WT_PATTERN_SQUALL;
        return WT_PATTERN_UNKNOWN;  /* 方向矛盾, 不硬猜 */
    }
    /* SDR静默+GNSS异常+地面气压变化 → 温度骤降=假冷锋, 否则不认定 */
    if (!c->sdr_active && c->gnss_anomaly && c->uno_pressure) {
        if (c->uno_temp) return WT_PATTERN_FALSE_COLD;
        return WT_PATTERN_UNKNOWN;
    }
    /* 仅地面层变化: 伴温度骤降=假冷锋, 气压单独突变=局地风场 */
    if (!c->sdr_active && !c->gnss_anomaly && c->uno_pressure) {
        if (c->uno_temp) return WT_PATTERN_FALSE_COLD;
        return WT_PATTERN_WIND_SHEAR;
    }
    /* 三路全静默: 唯一可确认的是"稳定+持续高湿"的准静止锋 */
    if (!c->sdr_active && !c->gnss_anomaly && !c->uno_pressure && !c->uno_temp) {
        if (pwv >= STATIONARY_PWV_THR && fabs(pwv_slope) < 1.0)
            return WT_PATTERN_STATIONARY;
        return WT_PATTERN_UNKNOWN;
    }
    /* 其余组合(SDR单独响/SDR+GNSS无地面印证): 证据不足, 诚实UNKNOWN */
    return WT_PATTERN_UNKNOWN;
}

/* ── 主相干引擎 ─────────────────────────────────────────── */
int wt_radar_correlate(wt_radar_correl_t *out, time_t ts, int window_min) {
    memset(out, 0, sizeof(*out));
    out->ts = ts ? ts : time(NULL);
    out->lead_time_min = 15;  /* 默认提前15分钟 */

    /* 1. 加载三路特征 (返回-1=该路无新鲜数据, 特征保持0) */
    double sdr_feat[16] = {0};
    double gnss_feat[8] = {0};
    double uno_feat[6] = {0};

    int sdr_ok = (load_sdr_features(out->ts, window_min, sdr_feat, 16) == 0);
    int gnss_ok = (load_gnss_features(out->ts, window_min, gnss_feat, 8) == 0);
    int uno_ok = (load_uno_features(out->ts, window_min, uno_feat, 6) == 0);

    if (!sdr_ok && !gnss_ok && !uno_ok) {
        /* 所有源都失败, 无法做相干分析 — 诚实输出零证据 */
        out->matched_pattern = WT_PATTERN_UNKNOWN;
        out->confidence = 0.0;
        out->coherence = 0.0;
        strncpy(out->pattern_name, "UNKNOWN", sizeof(out->pattern_name) - 1);
        return 0;
    }

    memcpy(out->sdr_features, sdr_feat, sizeof(sdr_feat));
    memcpy(out->gnss_features, gnss_feat, sizeof(gnss_feat));
    memcpy(out->uno_features, uno_feat, sizeof(uno_feat));

    /* 2. 异常检测(每路独立判断, 仅当有新鲜数据; 阈值见文件头校准) */
    out->sdr_active = sdr_ok && (sdr_feat[5] >= CORR_SDR_BASE_MIN_N) &&
                      (sdr_feat[3] > sdr_feat[4] + CORR_SDR_DELTA_THR);
    out->gnss_anomaly = gnss_ok && (gnss_feat[0] >= PWV_ANOM_THR ||
                        gnss_feat[1] >= PWV_SLOPE_THR ||
                        gnss_feat[2] > CORR_S4_THR || gnss_feat[3] > CORR_S4_THR);
    out->uno_pressure = uno_ok && (fabs(uno_feat[4]) > CORR_PRESS_THR);
    out->uno_temp    = uno_ok && (fabs(uno_feat[5]) > CORR_TEMP_THR);

    /* 3. 模式匹配 (基于"谁触发"+变化方向, 不再用无意义的相关系数) */
    out->matched_pattern = match_pattern(out);

    /* 4. 一致率与置信度(真实计算, 每个数字可解释):
     *   agreement = 报异常的独立源数 / 有新鲜数据的源数
     *   STATIONARY: 相干性=各路"一致稳定"占比, 置信度=×水汽饱和度
     *   其他型: 相干性=异常一致率, 置信度=×模式因子(UNKNOWN减半) */
    int n_avail = (sdr_ok ? 1 : 0) + (gnss_ok ? 1 : 0) + (uno_ok ? 1 : 0);
    int n_anom  = (out->sdr_active ? 1 : 0) + (out->gnss_anomaly ? 1 : 0) +
                  ((out->uno_pressure || out->uno_temp) ? 1 : 0);
    double agreement = (n_avail > 0) ? (double)n_anom / (double)n_avail : 0.0;

    if (out->matched_pattern == WT_PATTERN_STATIONARY) {
        double quiet = 1.0 - agreement;         /* 稳定一致率 */
        if (n_avail < 2) quiet *= 0.5;          /* 单源不足以印证 */
        double sat = (gnss_feat[0] - 40.0) / 10.0;  /* 水汽饱和度 */
        if (sat < 0.2) sat = 0.2;
        if (sat > 1.0) sat = 1.0;
        out->coherence  = quiet;
        out->confidence = quiet * sat;
    } else {
        double factor = (out->matched_pattern != WT_PATTERN_UNKNOWN) ? 1.0 : 0.5;
        out->coherence  = agreement;
        out->confidence = agreement * factor;
    }

    /* 命名 */
    const char *names[] = { "UNKNOWN", "THUNDER", "SQUALL", "FALSE_COLD", "STATIONARY", "WIND_SHEAR" };
    strncpy(out->pattern_name, names[out->matched_pattern], sizeof(out->pattern_name) - 1);

    /* 提前量: 置信度越高→提前量越大(预警可靠) */
    if (out->confidence > 0.6) out->lead_time_min = 30;
    else if (out->confidence > 0.3) out->lead_time_min = 20;
    else out->lead_time_min = 10;

    return 0;
}

/* ── Daemon入口 ──────────────────────────────────────────── */
int wt_radar_correlate_run(void) {
    wt_radar_correl_t c;
    if (wt_radar_correlate(&c, time(NULL), CORR_WINDOW_MIN) != 0) {
        printf("  ⚠ 相干雷达计算失败(数据不足)\n");
        return -1;
    }
    printf("  ✅ 三路相干: %s 置信度=%.2f 提前=%dmin\n",
           c.pattern_name, c.confidence, c.lead_time_min);

    /* 保存到DB */
    wt_db_save_correl(&c);

    /* 输出JSON (v2.0: 删除corr_sg/su/gu — 异构特征Pearson无物理意义) */
    FILE *f = fopen(CORREL_JSON, "w");
    if (f) {
        fprintf(f, "{\n");
        fprintf(f, "  \"ts\": %ld,\n", (long)c.ts);
        fprintf(f, "  \"coherence\": %.3f,\n", c.coherence);
        fprintf(f, "  \"sdr_active\": %d,\n", c.sdr_active);
        fprintf(f, "  \"gnss_anomaly\": %d,\n", c.gnss_anomaly);
        fprintf(f, "  \"uno_pressure_change\": %d,\n", c.uno_pressure);
        fprintf(f, "  \"uno_temp_change\": %d,\n", c.uno_temp);
        fprintf(f, "  \"matched_pattern\": \"%s\",\n", c.pattern_name);
        fprintf(f, "  \"confidence\": %.3f,\n", c.confidence);
        fprintf(f, "  \"lead_time_min\": %d,\n", c.lead_time_min);
        fprintf(f, "  \"pwv_mm\": %.2f,\n", c.gnss_features[0]);
        fprintf(f, "  \"pwv_slope_15min\": %.3f,\n", c.gnss_features[1]);
        fprintf(f, "  \"uno_press_rate\": %.3f,\n", c.uno_features[4]);
        fprintf(f, "  \"sdr_snr_delta\": %.2f\n", c.sdr_features[6]);
        fprintf(f, "}\n");
        fclose(f);
    }

    return 0;
}

/* ── DB初始化 ───────────────────────────────────────────── */
int wt_radar_db_init(const char *path) {
    sqlite3 *db;
    if (sqlite3_open(path, &db) != SQLITE_OK) return -1;

    const char *sql =
        "CREATE TABLE IF NOT EXISTS radar_correl ("
        "ts INTEGER PRIMARY KEY, "
        "coherence REAL, "
        "sdr_active INTEGER, "
        "gnss_anomaly INTEGER, "
        "uno_pressure INTEGER, "
        "uno_temp INTEGER, "
        "matched_pattern INTEGER, "
        "pattern_name TEXT, "
        "confidence REAL, "
        "lead_time_min INTEGER);";
    int rc = sqlite3_exec(db, sql, NULL, NULL, NULL);
    sqlite3_close(db);
    return (rc == SQLITE_OK) ? 0 : -1;
}

/* ── DB保存 ──────────────────────────────────────────────── */
int wt_db_save_correl(const wt_radar_correl_t *c) {
    sqlite3 *db;
    if (sqlite3_open(WENTIAN_DB, &db) != SQLITE_OK) return -1;

    char sql[512];
    snprintf(sql, sizeof(sql),
        "INSERT OR REPLACE INTO radar_correl "
        "(ts,coherence,sdr_active,gnss_anomaly,uno_pressure,uno_temp,"
        "matched_pattern,pattern_name,confidence,lead_time_min) "
        "VALUES (%ld,%.4f,%d,%d,%d,%d,%d,'%s',%.4f,%d)",
        (long)c->ts, c->coherence, c->sdr_active, c->gnss_anomaly,
        c->uno_pressure, c->uno_temp, c->matched_pattern, c->pattern_name,
        c->confidence, c->lead_time_min);

    int rc = sqlite3_exec(db, sql, NULL, NULL, NULL);
    sqlite3_close(db);
    return (rc == SQLITE_OK) ? 0 : -1;
}
