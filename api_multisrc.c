/* ============================================================
 * api_multisrc.c - 多源融合 S4 引擎 v1.0 (C实现)
 * ============================================================
 * 项目: 问天 v2.3 (WenTian Weather Station)
 * 所有者: 主人朱涛 BG8SBA, 昆明长水机场楼顶
 * 锁定: 详见 WENTIAN-LOCK.md, 非授权LLM请勿擅改
 *
 * 主人2026-09-04指示:
 *   "不要拘泥于一个数据源,融合互补"
 *   "SDR补充北斗的天上数据,复合UNO及多模块数据"
 *   "主板WiFi/以太/蓝牙等模块载波数据也是可以聚合"
 *
 * 5 数据源融合 (C/N0/S4):
 *   源1 (S1): UNO本地气象  (气压突变→电离层间接指示)
 *   源2 (S2): ATGM336H GPS+北斗串口 SNR (室内, 主路)
 *   源3 (S3): SDR扫频 GPS-L1/BDS-B1I 信号功率 (室外视角)
 *   源4 (S4): Open-Meteo 卫星气象(电离层TEC间接)
 *   源5 (S5): ScintPi互联网S4公开数据 (朱涛 BG8SBA对标)
 *
 * 融合策略:
 *   - 权重按数据可信度分配 (SDR+Open-Meteo > 串口 > 间接)
 *   - 任何一源缺失则动态调整权重
 *   - 多源一致时高置信度, 矛盾时标记低置信度
 *
 * 输出:
 *   - fused_s4  (综合 S4)
 *   - fusion_confidence (0-1)
 *   - 5源各 S4 贡献度
 * ============================================================ */
#include "wentian.h"
#include <sqlite3.h>
#include <math.h>
#include <time.h>
#include <string.h>
#include <sys/stat.h>
#include <stdio.h>
#include <unistd.h>

/* ── SDR GNSS 频点扫频数据目录 ─────────────────────────── */
#define SDR_GNSS_DIR "/root/data/sdr/gnss_sweep_v2"
#define MULTISRC_JSON "/root/data/fusion/multisrc_fusion.json"

/* ── 数据源权重 (主人在ENSO大考版校准) ──────────────────── */
#define W_SDR         0.30  /* SDR扫频 (室外视角, 最重) */
#define W_GNSS_UART   0.25  /* ATGM336H串口SNR (室内, 第二) */
#define W_OPENMETEO   0.20  /* Open-Meteo TEC */
#define W_SCINTPI     0.15  /* ScintPi互联网公开S4 */
#define W_UNO         0.10  /* UNO气压突变 (间接, 仅辅助) */

/* ── S4阈值 (国际标准) ──────────────────────────────────── */
static const char *s4_level_class(double s4) {
    if (s4 < 0)        return "NO DATA";
    if (s4 < 0.1)      return "NONE";
    if (s4 < 0.2)      return "WEAK";
    if (s4 < 0.4)      return "MODERATE";
    if (s4 < 0.6)      return "STRONG";
    return "SEVERE";
}

/* ── 源3: SDR扫频 GPS-L1 + BDS-B1I S4 提取 ───────────────── */
/* 原理: RTL-SDR接收GPS L1 (1575.42MHz) / BDS B1I (1561.098MHz)
 *       扫频CSV峰值变化反映信号功率波动 → 推算 S4
 * 主人硬件限制: 扫频数据来自gnss_sweep_v2/, 每次扫频多bin
 * 取所有扫频的SNR>3dB峰, 算峰间峰内RSD作为S4_t */
static double multisrc_sdr_s4(double *out_peak_snr, double *out_peak_freq_mhz) {
    struct stat st;
    int found = 0;

    /* 优先读 GPS-L1 扫频 */
    FILE *fp = fopen("/root/data/sdr/gnss_sweep_v2/GPS-L1.csv", "r");
    if (!fp) fp = fopen("/root/data/sdr/gnss_sweep_v2/BDS-B1I.csv", "r");
    if (!fp) return -1.0;

    /* 收集所有扫频峰值 */
    double peak_snrs[20] = {0};
    double peak_freqs[20] = {0};
    int n_peaks = 0;

    char line[8192];
    while (fgets(line, sizeof(line), fp)) {
        char *parts[600];
        int n = 0;
        char *p = line;
        while (p && *p && n < 600) {
            parts[n++] = p;
            char *q = strchr(p, ',');
            if (!q) break;
            *q = '\0';
            p = q + 1;
        }
        if (n < 7) continue;

        double start_hz = 0, bin_hz = 0;
        int num_bins = 0;
        if (sscanf(parts[2], "%lf", &start_hz) != 1) continue;
        if (sscanf(parts[4], "%lf", &bin_hz) != 1) continue;
        if (sscanf(parts[5], "%d", &num_bins) != 1) continue;
        if (num_bins <= 0 || num_bins > 500 || bin_hz <= 0) continue;

        /* 解析dBm数组 */
        double bins[500] = {0};
        int valid = 0;
        for (int i = 6; i < n && valid < num_bins; i++) {
            if (sscanf(parts[i], "%lf", &bins[valid]) == 1) valid++;
        }
        if (valid < 10) continue;

        /* 找峰值 */
        int peak_idx = 0;
        double peak_dbm = -200;
        double sum_dbm = 0;
        for (int i = 0; i < valid; i++) {
            sum_dbm += bins[i];
            if (bins[i] > peak_dbm) { peak_dbm = bins[i]; peak_idx = i; }
        }
        double noise = sum_dbm / valid;
        double snr = peak_dbm - noise;
        double freq = (start_hz + peak_idx * bin_hz) / 1e6;

        if (snr > 1.5 && n_peaks < 20) {
            peak_snrs[n_peaks] = snr;
            peak_freqs[n_peaks] = freq;
            n_peaks++;
        }
    }
    fclose(fp);

    if (n_peaks < 2) {
        /* 只有一次扫频 → 直接输出那次SNR作为初步估计 */
        if (out_peak_snr) *out_peak_snr = n_peaks > 0 ? peak_snrs[0] : 0;
        if (out_peak_freq_mhz) *out_peak_freq_mhz = n_peaks > 0 ? peak_freqs[0] : 0;
        return n_peaks > 0 ? 0.3 * (peak_snrs[0] / 20.0) : -1.0;  /* 粗估 */
    }

    /* 多扫频: 峰SNR的RSD作为S4_t
     * S4 = stddev(peak_snrs) / mean(peak_snrs) */
    double sum = 0, sum_sq = 0;
    for (int i = 0; i < n_peaks; i++) {
        sum += peak_snrs[i];
        sum_sq += peak_snrs[i] * peak_snrs[i];
    }
    double mean = sum / n_peaks;
    double var = sum_sq / n_peaks - mean * mean;
    if (var < 0) var = 0;
    double s4 = sqrt(var) / mean;

    /* 取最强峰 */
    int best = 0;
    for (int i = 1; i < n_peaks; i++) {
        if (peak_snrs[i] > peak_snrs[best]) best = i;
    }

    if (out_peak_snr) *out_peak_snr = peak_snrs[best];
    if (out_peak_freq_mhz) *out_peak_freq_mhz = peak_freqs[best];
    return s4;
}

/* ── 源2: ATGM336H 串口 GPS/BDS SNR (从已有 local_iono) ─────── */
static double multisrc_gnss_uart_s4(double *out_avg_snr) {
    sqlite3 *db;
    if (sqlite3_open(WENTIAN_DB, &db) != SQLITE_OK) return -1.0;
    sqlite3_stmt *st;
    int rc = sqlite3_prepare_v2(db,
        "SELECT s4_gps, s4_bds, gps_snr_avg, bds_snr_avg "
        "FROM local_iono ORDER BY ts DESC LIMIT 1",
        -1, &st, NULL);
    if (rc != SQLITE_OK) { sqlite3_close(db); return -1.0; }

    double s4 = -1;
    double avg_snr = 0;
    if (sqlite3_step(st) == SQLITE_ROW) {
        double s4_g = sqlite3_column_double(st, 0);
        double s4_b = sqlite3_column_double(st, 1);
        double snr_g = sqlite3_column_double(st, 2);
        double snr_b = sqlite3_column_double(st, 3);
        s4 = (s4_g > s4_b) ? s4_g : s4_b;
        avg_snr = (snr_g + snr_b) / 2.0;
    }
    sqlite3_finalize(st);
    sqlite3_close(db);
    if (out_avg_snr) *out_avg_snr = avg_snr;
    return s4;
}

/* ── 源4: Open-Meteo TEC (用K-index间接反映电离层) ────────── */
/* Open-Meteo 没直接给TEC, 但提供Kp指数分布 → 间接推算 */
static double multisrc_openmeteo_kp(void) {
    sqlite3 *db;
    if (sqlite3_open(WENTIAN_DB, &db) != SQLITE_OK) return -1.0;
    sqlite3_stmt *st;
    int rc = sqlite3_prepare_v2(db,
        "SELECT kp FROM swpc_kp ORDER BY ts DESC LIMIT 1",
        -1, &st, NULL);
    if (rc != SQLITE_OK) { sqlite3_close(db); return -1.0; }

    double kp = -1;
    if (sqlite3_step(st) == SQLITE_ROW) {
        kp = sqlite3_column_double(st, 0);
    }
    sqlite3_finalize(st);
    sqlite3_close(db);
    return kp;
}

/* ── 源1: UNO 气压突变 (间接电离层指示) ────────────────────── */
/* 强电离层扰动会引起大气压力微扰, 30min气压骤变>1.5hPa */
static double multisrc_uno_pressure_delta(void) {
    sqlite3 *db;
    if (sqlite3_open("/root/data/ano_weather.db", &db) != SQLITE_OK) return -1.0;
    sqlite3_stmt *st;
    int rc = sqlite3_prepare_v2(db,
        "SELECT sea_level_pressure FROM ano_weather "
        "WHERE source='UNO_v2.0_bridge' AND sea_level_pressure > 0 "
        "ORDER BY ts DESC LIMIT 60",
        -1, &st, NULL);
    if (rc != SQLITE_OK) { sqlite3_close(db); return -1.0; }

    /* 取最近 60 vs 60分钟前 */
    double vals[60] = {0};
    int n = 0;
    while (sqlite3_step(st) == SQLITE_ROW && n < 60) {
        vals[n++] = sqlite3_column_double(st, 0);
    }
    sqlite3_finalize(st);
    sqlite3_close(db);
    if (n < 30) return -1.0;

    /* vals是倒序(最新在前), 最近=vals[0], 30分钟前=vals[29] */
    double recent = 0;
    for (int i = 0; i < 10; i++) recent += vals[i];
    recent /= 10;
    double old = 0;
    for (int i = 25; i < 35 && i < n; i++) old += vals[i];
    old /= 10;

    return recent - old;  /* hPa 正=气压升(冷锋), 负=气压降(雷暴) */
}

/* ── 主入口: 多源融合 S4 ─────────────────────────────────── */
int wt_multisrc_run(void) {
    printf("\n━━━ 23. 多源融合 S4 引擎 (5源加权) ━━━\n");

    /* 采集5源 S4/SNR/Kp/ΔP */
    double s4_sdr = multisrc_sdr_s4(NULL, NULL);
    double s4_uart = multisrc_gnss_uart_s4(NULL);
    double kp = multisrc_openmeteo_kp();
    double dp_30min = multisrc_uno_pressure_delta();

    /* SDR原始峰值 (用于诊断) */
    double sdr_peak_snr = 0, sdr_peak_freq = 0;
    multisrc_sdr_s4(&sdr_peak_snr, &sdr_peak_freq);
    double uart_avg_snr = 0;
    multisrc_gnss_uart_s4(&uart_avg_snr);

    /* Open-Meteo Kp → 等效S4 (经验: Kp 5 ≈ S4 0.3) */
    double s4_openmeteo = (kp >= 0 && kp <= 9) ? (kp * 0.06) : -1.0;

    /* UNO 气压30min变化 → 间接S4 (1.5hPa对应S4≈0.2) */
    double s4_uno = (fabs(dp_30min) > 0.5) ? (fabs(dp_30min) / 7.5) : -1.0;

    /* 源5 (ScintPi) 占位 - 当前DB无该数据, 设为-1不参与 */
    double s4_scintpi = -1.0;

    printf("  ── 5源数据采集 ──\n");
    printf("    [1] SDR扫频     S4=%.3f | 峰SNR=%.2fdB @ %.3fMHz\n",
           s4_sdr >= 0 ? s4_sdr : 0.0, sdr_peak_snr, sdr_peak_freq);
    printf("    [2] ATGM336H串口 S4=%.3f | 平均SNR=%.1fdB\n",
           s4_uart >= 0 ? s4_uart : 0.0, uart_avg_snr);
    printf("    [3] Open-Meteo Kp=%.1f → S4=%.3f\n", kp, s4_openmeteo);
    printf("    [4] UNO气压30min ΔP=%.2fhPa → S4=%.3f\n", dp_30min, s4_uno);
    printf("    [5] ScintPi    S4=%.3f (ScintPi数据源待接入)\n", s4_scintpi);

    /* ── 加权融合 (缺失源动态调整权重) ──────────────────── */
    double weights[5] = {W_SDR, W_GNSS_UART, W_OPENMETEO, W_UNO, W_SCINTPI};
    double values[5] = {s4_sdr, s4_uart, s4_openmeteo, s4_uno, s4_scintpi};
    const char *names[5] = {"SDR", "UART", "OpenMeteo", "UNO", "ScintPi"};

    /* 归一化权重 (跳过NO DATA) */
    double w_sum = 0;
    for (int i = 0; i < 5; i++) {
        if (values[i] >= 0) w_sum += weights[i];
    }
    if (w_sum < 0.01) {
        printf("  ⚠️ 所有数据源缺失, 无法融合\n");
        return -1;
    }

    double fused_s4 = 0;
    double used_n = 0;
    for (int i = 0; i < 5; i++) {
        if (values[i] >= 0) {
            fused_s4 += weights[i] * values[i];
            used_n++;
        }
    }
    fused_s4 /= w_sum;

    /* 置信度 = (有效源数 / 5) × (1 - 源间标准差/均值)
     * 多源一致 → 高置信度; 矛盾 → 低置信度 */
    double vals_active[5] = {0};
    double v_sum = 0, v_var = 0;
    int v_n = 0;
    for (int i = 0; i < 5; i++) {
        if (values[i] >= 0) {
            vals_active[v_n] = values[i];
            v_sum += values[i];
            v_n++;
        }
    }
    double v_mean = v_n > 0 ? v_sum / v_n : 0;
    for (int i = 0; i < v_n; i++) {
        v_var += (vals_active[i] - v_mean) * (vals_active[i] - v_mean);
    }
    v_var = v_n > 0 ? v_var / v_n : 0;
    double v_std = sqrt(v_var);

    double coverage = (double)used_n / 5.0;
    double consistency = (v_mean > 0.01) ? (1.0 - v_std / v_mean) : 1.0;
    if (consistency < 0) consistency = 0;
    if (consistency > 1) consistency = 1;
    double confidence = coverage * consistency;

    printf("  ── 融合结果 ──\n");
    printf("    综合 S4 = %.4f | 等级 = %s\n",
           fused_s4, s4_level_class(fused_s4));
    printf("    置信度 = %.2f (覆盖率=%.0f%% 一致性=%.2f)\n",
           confidence, coverage * 100, consistency);
    printf("    有效源: %d/5\n", (int)used_n);

    /* ── 写入 DB ─────────────────────────────────────────── */
    sqlite3 *db;
    if (sqlite3_open(WENTIAN_DB, &db) == SQLITE_OK) {
        const char *sql = "CREATE TABLE IF NOT EXISTS multisrc_s4 ("
            "ts INTEGER PRIMARY KEY, "
            "s4_sdr REAL DEFAULT -1, s4_uart REAL DEFAULT -1, "
            "s4_openmeteo REAL DEFAULT -1, s4_uno REAL DEFAULT -1, "
            "s4_scintpi REAL DEFAULT -1, "
            "fused_s4 REAL DEFAULT 0, confidence REAL DEFAULT 0, "
            "level TEXT DEFAULT '', kp REAL DEFAULT 0, "
            "dp_30min REAL DEFAULT 0, "
            "used_n INTEGER DEFAULT 0, note TEXT DEFAULT '')";
        sqlite3_exec(db, sql, NULL, NULL, NULL);

        sqlite3_stmt *st;
        int rc;
        rc = sqlite3_prepare_v2(db,
            "INSERT OR REPLACE INTO multisrc_s4 "
            "(ts,s4_sdr,s4_uart,s4_openmeteo,s4_uno,s4_scintpi,"
            " fused_s4,confidence,level,kp,dp_30min,used_n,note) "
            "VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?)", -1, &st, NULL);
        if (rc == SQLITE_OK) {
            sqlite3_bind_int64(st, 1, (sqlite3_int64)time(NULL));
            sqlite3_bind_double(st, 2, s4_sdr);
            sqlite3_bind_double(st, 3, s4_uart);
            sqlite3_bind_double(st, 4, s4_openmeteo);
            sqlite3_bind_double(st, 5, s4_uno);
            sqlite3_bind_double(st, 6, s4_scintpi);
            sqlite3_bind_double(st, 7, fused_s4);
            sqlite3_bind_double(st, 8, confidence);
            sqlite3_bind_text(st, 9, s4_level_class(fused_s4), -1, SQLITE_TRANSIENT);
            sqlite3_bind_double(st, 10, kp);
            sqlite3_bind_double(st, 11, dp_30min);
            sqlite3_bind_int(st, 12, (int)used_n);
            char note[64];
            snprintf(note, sizeof(note), "%d/5源融合 一致性=%.2f", (int)used_n, consistency);
            sqlite3_bind_text(st, 13, note, -1, SQLITE_TRANSIENT);
            sqlite3_step(st);
            sqlite3_finalize(st);
        }
        sqlite3_close(db);
    }

    /* ── 写 JSON ─────────────────────────────────────────── */
    mkdir("/root/data/fusion", 0755);
    FILE *jf = fopen(MULTISRC_JSON, "w");
    if (jf) {
        fprintf(jf, "{\n");
        fprintf(jf, "  \"ts\": %ld,\n", (long)time(NULL));
        fprintf(jf, "  \"sources\": {\n");
        fprintf(jf, "    \"sdr\": {\"s4\": %.4f, \"peak_snr_db\": %.2f, \"peak_freq_mhz\": %.3f},\n",
                s4_sdr, sdr_peak_snr, sdr_peak_freq);
        fprintf(jf, "    \"uart\": {\"s4\": %.4f, \"avg_snr_db\": %.1f},\n", s4_uart, uart_avg_snr);
        fprintf(jf, "    \"openmeteo\": {\"kp\": %.1f, \"s4_eq\": %.4f},\n", kp, s4_openmeteo);
        fprintf(jf, "    \"uno\": {\"dp_30min_hpa\": %.2f, \"s4_eq\": %.4f},\n", dp_30min, s4_uno);
        fprintf(jf, "    \"scintpi\": {\"s4\": %.4f}\n", s4_scintpi);
        fprintf(jf, "  },\n");
        fprintf(jf, "  \"fusion\": {\n");
        fprintf(jf, "    \"fused_s4\": %.4f,\n", fused_s4);
        fprintf(jf, "    \"level\": \"%s\",\n", s4_level_class(fused_s4));
        fprintf(jf, "    \"confidence\": %.4f,\n", confidence);
        fprintf(jf, "    \"coverage\": %.2f,\n", coverage);
        fprintf(jf, "    \"consistency\": %.4f,\n", consistency);
        fprintf(jf, "    \"used_n\": %d\n", (int)used_n);
        fprintf(jf, "  }\n");
        fprintf(jf, "}\n");
        fclose(jf);
    }

    printf("  ✅ 已存入 multisrc_s4 表 + multisrc_fusion.json\n");
    return 0;
}