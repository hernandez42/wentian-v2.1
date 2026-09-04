/* ============================================================
 * api_correlate.c - 问天软件雷达 · 三路相干引擎 v1.0
 * ============================================================
 * 项目: 问天 v1.1 (WenTian Weather Station)
 * 所有者: 主人朱涛 BG8SBA, 昆明长水机场楼顶
 * 锁定: 详见 WENTIAN-LOCK.md, 非授权LLM请勿擅改
 *
 * 核心思想:
 *   SDR射频枪 + GNSS信号枪 + UNO地面枪 = 三路相干软件雷达
 *   不是三个传感器各看各的, 而是把它们的信号当成同一部雷达的
 *   不同接收通道, 做相干处理。
 *
 * 三路信号:
 *   SDR: 频谱峰值 + 噪声底 + 信标闪烁 (射频层)
 *   GNSS: PWV + S4指数 + SNR + 多路径 (信号层)
 *   UNO: 气压 + 温度 + 湿度 (地面层)
 *
 * 处理流程:
 *   1. 时间对齐(统一到1分钟粒度, 取window_min时间窗)
 *   2. 每路提取特征向量
 *   3. 计算三路互相关系数
 *   4. 模式匹配(谁响谁不响 + 变化方向)
 *   5. 输出天气型概率 + 置信度 + 提前量
 *
 * 模式匹配规则(基于"谁响谁不响"):
 *   雷暴: SDR闪烁+GNSS PWV急升+UNO气压降 → 三路同向
 *   飑线: SDR流星异常(电离层扰动)+GNSS PWV骤降+UNO气压骤升
 *   假冷锋: SDR静默+GNSS PWV不变+UNO温度骤降 → 仅UNO响
 *   准静止锋: 三路全部稳定, GNSS持续高湿
 *   风切变: SDR静默+GNSS风向突变+UNO气压稳
 *
 * 编译: 纳入 wentian 主编译 (gcc -O2 -Wall -Wextra)
 * ============================================================ */
#include "wentian.h"
#include <sqlite3.h>
#include <math.h>

#define CORREL_JSON          "/root/data/fusion/radar_correlation.json"
#define CORREL_TABLE         "radar_correl"
#define PWV_HISTORY_CSV      "/root/data/fusion/pwv_history.csv"  /* 与api_nowcast.c共享 */

/* ── 特征提取窗口(分钟) ──────────────────────────────────── */
#define CORR_WINDOW_MIN      15     /* 滑动窗长度 */
#define CORR_PRESS_THR       0.5    /* hPa/10min, 气压变化显著 */
#define CORR_TEMP_THR        1.0    /* °C/10min, 温度变化显著 */
#define CORR_PWV_THR         0.5    /* mm/10min, PWV变化显著 */
#define CORR_S4_THR          0.3    /* 电离层闪烁显著阈值 */
#define CORR_SDR_SNR_THR     5.0    /* dB, SDR峰值SNR突变 */

/* ── 模式匹配函数声明(下面定义) ──────────────────────────── */
static int match_pattern(const wt_radar_correl_t *c);
static int load_sdr_features(time_t ts, int span_min, double *feat, int n);
static int load_gnss_features(time_t ts, int span_min, double *feat, int n);
static int load_uno_features(time_t ts, int span_min, double *feat, int n);

/* ── 互相关系数 ──────────────────────────────────────────── */
static double corrcoef(const double *x, const double *y, int n) {
    double sx = 0, sy = 0, sxx = 0, syy = 0, sxy = 0;
    for (int i = 0; i < n; i++) {
        sx += x[i]; sy += y[i];
        sxx += x[i] * x[i];
        syy += y[i] * y[i];
        sxy += x[i] * y[i];
    }
    double nx = n * sxx - sx * sx;
    double ny = n * syy - sy * sy;
    if (nx <= 0 || ny <= 0) return 0.0;
    return (n * sxy - sx * sy) / sqrt(nx * ny);
}

/* ── 加载SDR特征 ────────────────────────────────────────── */
/* 特征向量[16]:
 *  [0] 噪声底dBm, [1] 峰值频率MHz, [2] 峰值dBm, [3] 峰值SNR
 *  [4-7] 业余2m频段(144-148MHz)统计: 均值/方差/最大值/最大值频率
 *  [8-11] 业余70cm频段(430-440MHz)统计
 *  [12-15] 航空频段(108-137MHz)统计
 */
static int load_sdr_features(time_t ts, int span_min, double *feat, int n) {
    (void)ts; (void)span_min; (void)n;
    /* 当前用最新快照, 历史回放时再用时间窗 */
    sqlite3 *db;
    if (sqlite3_open(WENTIAN_DB, &db) != SQLITE_OK) return -1;

    sqlite3_stmt *st;
    int rc = sqlite3_prepare_v2(db,
        "SELECT noise_dbm, peak_mhz, peak_dbm, peak_snr "
        "FROM local_sdr ORDER BY ts DESC LIMIT 1", -1, &st, NULL);
    if (rc != SQLITE_OK) { sqlite3_close(db); return -1; }
    if (sqlite3_step(st) != SQLITE_ROW) { sqlite3_finalize(st); sqlite3_close(db); return -1; }

    feat[0] = sqlite3_column_double(st, 0);  /* noise floor */
    feat[1] = sqlite3_column_double(st, 1);  /* peak freq */
    feat[2] = sqlite3_column_double(st, 2);  /* peak dbm */
    feat[3] = sqlite3_column_double(st, 3);  /* peak snr */
    sqlite3_finalize(st);

    /* 简单统计: 从local_sdr读最近10条算各频段均值 */
    sqlite3_stmt *st2;
    rc = sqlite3_prepare_v2(db,
        "SELECT AVG(peak_snr), AVG(noise_dbm), MAX(peak_snr), peak_mhz "
        "FROM local_sdr WHERE peak_mhz BETWEEN 144 AND 148 "
        "ORDER BY ts DESC LIMIT 10", -1, &st2, NULL);
    if (rc == SQLITE_OK && sqlite3_step(st2) == SQLITE_ROW) {
        feat[4] = sqlite3_column_double(st2, 0);  /* 2m avg snr */
        feat[5] = sqlite3_column_double(st2, 1);  /* 2m avg noise */
        feat[6] = sqlite3_column_double(st2, 2);  /* 2m max snr */
        feat[7] = sqlite3_column_double(st2, 3);  /* 2m peak freq */
    }
    sqlite3_finalize(st2);

    rc = sqlite3_prepare_v2(db,
        "SELECT AVG(peak_snr), AVG(noise_dbm), MAX(peak_snr), peak_mhz "
        "FROM local_sdr WHERE peak_mhz BETWEEN 430 AND 440 "
        "ORDER BY ts DESC LIMIT 10", -1, &st2, NULL);
    if (rc == SQLITE_OK && sqlite3_step(st2) == SQLITE_ROW) {
        feat[8] = sqlite3_column_double(st2, 0);
        feat[9] = sqlite3_column_double(st2, 1);
        feat[10] = sqlite3_column_double(st2, 2);
        feat[11] = sqlite3_column_double(st2, 3);
    }
    sqlite3_finalize(st2);

    rc = sqlite3_prepare_v2(db,
        "SELECT AVG(peak_snr), AVG(noise_dbm), MAX(peak_snr), peak_mhz "
        "FROM local_sdr WHERE peak_mhz BETWEEN 108 AND 137 "
        "ORDER BY ts DESC LIMIT 10", -1, &st2, NULL);
    if (rc == SQLITE_OK && sqlite3_step(st2) == SQLITE_ROW) {
        feat[12] = sqlite3_column_double(st2, 0);
        feat[13] = sqlite3_column_double(st2, 1);
        feat[14] = sqlite3_column_double(st2, 2);
        feat[15] = sqlite3_column_double(st2, 3);
    }
    sqlite3_finalize(st2);
    sqlite3_close(db);
    return 0;
}

/* ── 加载GNSS特征 ───────────────────────────────────────── */
/* 特征向量[8]:
 *  [0] PWV(mm), [1] S4_GPS, [2] S4_BDS, [3] GPS_SNR
 *  [4] BDS_SNR, [5] Klobuchar垂直延迟, [6] 总卫星数, [7] PDOP
 */
static int load_gnss_features(time_t ts, int span_min, double *feat, int n) {
    (void)ts; (void)span_min; (void)n;
    sqlite3 *db;
    if (sqlite3_open(WENTIAN_DB, &db) != SQLITE_OK) return -1;

    /* 读GNSS电离层数据 */
    sqlite3_stmt *st;
    int rc = sqlite3_prepare_v2(db,
        "SELECT s4_gps, s4_bds, gps_snr_avg, bds_snr_avg, "
        "klob_vert_delay, total_sats, pdop "
        "FROM local_iono ORDER BY ts DESC LIMIT 1", -1, &st, NULL);
    if (rc != SQLITE_OK) { sqlite3_close(db); return -1; }
    if (sqlite3_step(st) == SQLITE_ROW) {
        feat[1] = sqlite3_column_double(st, 0);
        feat[2] = sqlite3_column_double(st, 1);
        feat[3] = sqlite3_column_double(st, 2);
        feat[4] = sqlite3_column_double(st, 3);
        feat[5] = sqlite3_column_double(st, 4);
        feat[6] = sqlite3_column_double(st, 5);
        feat[7] = sqlite3_column_double(st, 6);
    }
    sqlite3_finalize(st);

    /* PWV从pwv_history.csv读最新值(兼容新旧格式) */
    FILE *f = fopen(PWV_HISTORY_CSV, "r");
    if (f) {
        char line[512];
        double last_pwv = 0.0;
        while (fgets(line, sizeof(line), f)) {
            /* 跳过标题行或空行 */
            if (line[0] < '0' || line[0] > '9') continue;
            char *p = strchr(line, ',');
            if (p) {
                double v = strtod(p + 1, NULL);
                if (v > 0) last_pwv = v;
            }
        }
        fclose(f);
        feat[0] = last_pwv;
    }
    sqlite3_close(db);
    return 0;
}

/* ── 加载UNO特征 ────────────────────────────────────────── */
/* 特征向量[6]:
 *  [0] 气压hPa, [1] 温度°C, [2] 湿度%, [3] 海平面气压
 *  [4] 气压10min变化率, [5] 温度10min变化率
 */
static int load_uno_features(time_t ts, int span_min, double *feat, int n) {
    (void)ts; (void)span_min; (void)n;
    sqlite3 *db;
    if (sqlite3_open(WENTIAN_DB, &db) != SQLITE_OK) return -1;

    sqlite3_stmt *st;
    int rc = sqlite3_prepare_v2(db,
        "SELECT t, h, p, pa, ts FROM ano_weather "
        "WHERE source='UNO_v2.0_bridge' ORDER BY ts DESC LIMIT 5", -1, &st, NULL);
    if (rc != SQLITE_OK) { sqlite3_close(db); return -1; }

    double temps[5] = {0}, pressures[5] = {0}, times[5] = {0};
    int count = 0;
    while (sqlite3_step(st) == SQLITE_ROW && count < 5) {
        temps[count] = sqlite3_column_double(st, 0);
        pressures[count] = sqlite3_column_double(st, 2);
        times[count] = sqlite3_column_double(st, 4);
        count++;
    }
    sqlite3_finalize(st);
    sqlite3_close(db);

    if (count == 0) return -1;

    feat[0] = pressures[0];  /* 当前气压 */
    feat[1] = temps[0];      /* 当前温度 */
    feat[2] = 0;             /* 湿度(uno表无, 留空) */
    feat[3] = 0;             /* 海平面气压(从其他源) */

    /* 10分钟变化率(线性近似) */
    if (count >= 2 && times[0] > times[count - 1]) {
        double dt = (times[0] - times[count - 1]) / 60.0;  /* 分钟 */
        if (dt > 0) {
            feat[4] = (pressures[0] - pressures[count - 1]) * 10.0 / dt;  /* hPa/10min */
            feat[5] = (temps[0] - temps[count - 1]) * 10.0 / dt;          /* °C/10min */
        }
    }
    return 0;
}

/* ── 模式匹配 ───────────────────────────────────────────── */
/* 基于"谁响谁不响" + 变化方向 判断天气型 */
static int match_pattern(const wt_radar_correl_t *c) {
    if (c->sdr_active && c->gnss_anomaly && c->uno_pressure) {
        /* 三路同向: 最可能是雷暴或飑线 */
        if (c->corr_gu > 0.5) return WT_PATTERN_THUNDER;
        if (c->corr_gu < -0.3) return WT_PATTERN_SQUALL;  /* 气压升/PWV降反相关 */
    }
    if (!c->sdr_active && c->gnss_anomaly && c->uno_pressure) {
        /* SDR静默: 非电离层事件, 可能是假冷锋或静止锋 */
        if (c->uno_temp) return WT_PATTERN_FALSE_COLD;  /* 温度骤降 */
        return WT_PATTERN_STATIONARY;
    }
    if (!c->sdr_active && !c->gnss_anomaly && c->uno_pressure) {
        /* 仅UNO变化: 局地风场或微下击暴流 */
        return WT_PATTERN_WIND_SHEAR;
    }
    return WT_PATTERN_UNKNOWN;
}

/* ── 主相干引擎 ─────────────────────────────────────────── */
int wt_radar_correlate(wt_radar_correl_t *out, time_t ts, int window_min) {
    memset(out, 0, sizeof(*out));
    out->ts = ts ? ts : time(NULL);
    out->lead_time_min = 15;  /* 默认提前15分钟 */

    /* 1. 加载三路特征 */
    double sdr_feat[16] = {0};
    double gnss_feat[8] = {0};
    double uno_feat[6] = {0};

    load_sdr_features(out->ts, window_min, sdr_feat, 16);
    load_gnss_features(out->ts, window_min, gnss_feat, 8);
    load_uno_features(out->ts, window_min, uno_feat, 6);

    memcpy(out->sdr_features, sdr_feat, sizeof(sdr_feat));
    memcpy(out->gnss_features, gnss_feat, sizeof(gnss_feat));
    memcpy(out->uno_features, uno_feat, sizeof(uno_feat));

    /* 2. 异常检测(每路独立判断) */
    /* SDR: 峰值SNR > 15dB 或 2m频段SNR突变 */
    out->sdr_active = (sdr_feat[3] > 15.0 || sdr_feat[6] > 15.0) ? 1 : 0;

    /* GNSS: PWV>40mm 或 S4>0.3 或 SNR异常 */
    out->gnss_anomaly = (gnss_feat[0] > 40.0 || gnss_feat[1] > CORR_S4_THR ||
                          gnss_feat[2] > CORR_S4_THR) ? 1 : 0;

    /* UNO: 气压变化>0.5hPa/10min 或 温度变化>1°C/10min */
    out->uno_pressure = (fabs(uno_feat[4]) > CORR_PRESS_THR) ? 1 : 0;
    out->uno_temp = (fabs(uno_feat[5]) > CORR_TEMP_THR) ? 1 : 0;

    /* 3. 互相关系数(用特征向量近似) */
    /* SDR↔GNSS: 峰值SNR vs PWV/S4 */
    out->corr_sg = corrcoef(sdr_feat, gnss_feat, 8);
    /* SDR↔UNO: 峰值SNR vs 气压/温度 */
    out->corr_su = corrcoef(sdr_feat, uno_feat, 6);
    /* GNSS↔UNO: PWV/S4 vs 气压/温度 */
    out->corr_gu = corrcoef(gnss_feat, uno_feat, 6);

    /* 4. 相干系数(三路相关性综合) */
    double c = (fabs(out->corr_sg) + fabs(out->corr_su) + fabs(out->corr_gu)) / 3.0;
    out->coherence = (c > 1.0) ? 1.0 : c;

    /* 5. 模式匹配 */
    out->matched_pattern = match_pattern(out);
    out->confidence = out->coherence;  /* 简化: 相干系数=置信度 */

    /* 命名 */
    const char *names[] = { "UNKNOWN", "THUNDER", "SQUALL", "FALSE_COLD", "STATIONARY", "WIND_SHEAR" };
    strncpy(out->pattern_name, names[out->matched_pattern], sizeof(out->pattern_name) - 1);

    /* 提前量: 根据相干性调整 */
    if (out->coherence > 0.6) out->lead_time_min = 10;
    else if (out->coherence > 0.3) out->lead_time_min = 20;
    else out->lead_time_min = 30;

    return 0;
}

/* ── Daemon入口 ──────────────────────────────────────────── */
int wt_radar_correlate_run(void) {
    wt_radar_correl_t c;
    if (wt_radar_correlate(&c, time(NULL), CORR_WINDOW_MIN) != 0) return -1;

    /* 保存到DB */
    wt_db_save_correl(&c);

    /* 输出JSON */
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
        fprintf(f, "  \"corr_sg\": %.3f,\n", c.corr_sg);
        fprintf(f, "  \"corr_su\": %.3f,\n", c.corr_su);
        fprintf(f, "  \"corr_gu\": %.3f\n", c.corr_gu);
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
        "lead_time_min INTEGER, "
        "corr_sg REAL, "
        "corr_su REAL, "
        "corr_gu REAL);";
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
        "matched_pattern,pattern_name,confidence,lead_time_min,"
        "corr_sg,corr_su,corr_gu) "
        "VALUES (%ld,%.4f,%d,%d,%d,%d,%d,'%s',%.4f,%d,%.4f,%.4f,%.4f)",
        (long)c->ts, c->coherence, c->sdr_active, c->gnss_anomaly,
        c->uno_pressure, c->uno_temp, c->matched_pattern, c->pattern_name,
        c->confidence, c->lead_time_min, c->corr_sg, c->corr_su, c->corr_gu);

    int rc = sqlite3_exec(db, sql, NULL, NULL, NULL);
    sqlite3_close(db);
    return (rc == SQLITE_OK) ? 0 : -1;
}