/* ============================================================
 * api_sdr_live.c - SDR实时频谱采集引擎 v1.0
 * ============================================================
 * 使用 rtl_power 作为数据源, 实时解析CSV输出:
 *   1560M-1580M (GNSS L1: 北斗B1I+GPS L1), 步进10kHz
 *   每3秒积分, 单次扫描
 *
 * 噪底 = 第5百分位, 峰值 = 最大值, SNR = 峰值 - 噪底
 * 结果写入 local_sdr 表
 *
 * 编译: gcc -O2 -Wall -Wextra ... -lm
 * ============================================================ */

#include "wentian.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <unistd.h>

/* 百分比比较函数: 升序, 用于百分位计算 */
static int cmp_double(const void *a, const void *b) {
    double da = *(const double *)a;
    double db = *(const double *)b;
    return (da > db) - (da < db);
}

/* ── 初始化 ──────────────────────────────────────────────── */
void wt_sdr_live_init(wt_sdr_live_t *sdr) {
    if (!sdr) return;
    memset(sdr, 0, sizeof(*sdr));
    sdr->active      = 0;
    sdr->center_freq = 1570e6;          /* 北斗B1I(1561.098MHz) + GPS L1(1575.42MHz) 中间 */
    sdr->bw_hz       = 20000000;        /* 20MHz, 覆盖 1560-1580MHz */
    sdr->gain        = 20;              /* RTL-SDR Blog V4 已验证增益 */
    sdr->noise_floor = 0.0;
    sdr->peak_freq   = 0.0;
    sdr->peak_power  = -200.0;
    sdr->avg_snr     = 0.0;
    sdr->last_update = 0;
}

/* ── 扫描 ────────────────────────────────────────────────── */
int wt_sdr_live_scan(wt_sdr_live_t *sdr) {
    if (!sdr) return -1;
    sdr->active = 1;

    /* 从结构体参数计算实际扫频范围 (默认 1560M-1580M) */
    double start_mhz = (sdr->center_freq - sdr->bw_hz / 2.0) / 1e6;
    double end_mhz   = (sdr->center_freq + sdr->bw_hz / 2.0) / 1e6;

    char cmd[512];
    snprintf(cmd, sizeof(cmd),
             "rtl_power -f %.0fM:%.0fM:10k -g %d -i 3 -1 2>/dev/null",
             start_mhz, end_mhz, sdr->gain);

    FILE *fp = popen(cmd, "r");
    if (!fp) {
        sdr->active = 0;
        return -1;
    }

    /* 动态数组收集所有频点dBm值和对应频率 */
    double *values = NULL;
    double *freqs  = NULL;
    int n_values = 0, cap = 0;
    char line[8192];

    while (fgets(line, sizeof(line), fp)) {
        /* rtl_power CSV: date, time, Hz_low, Hz_high, Hz_step, samples, dBm, dBm, ... */
        double start_hz, end_hz, bin_hz;
        int num_bins;
        char date[32], time_str[32];

        int n = sscanf(line, "%31[^,],%31[^,],%lf,%lf,%lf,%d",
                       date, time_str, &start_hz, &end_hz, &bin_hz, &num_bins);
        if (n < 6 || num_bins <= 0 || num_bins > 50000 || bin_hz <= 0)
            continue;

        /* 跳过前6个逗号分隔字段 */
        char *p = line;
        for (int c = 0; c < 6; c++) {
            p = strchr(p, ',');
            if (!p) break;
            p++;
        }
        if (!p) continue;

        /* 解析每个频点的dBm值 */
        for (int b = 0; b < num_bins && *p; b++) {
            double dbm;
            if (sscanf(p, "%lf", &dbm) != 1) break;

            if (n_values >= cap) {
                cap = cap ? cap * 2 : 8192;
                double *old_v = values;
                double *old_f = freqs;
                values = realloc(old_v, cap * sizeof(double));
                freqs  = realloc(old_f, cap * sizeof(double));
                if (!values || !freqs) {
                    free(values ? values : old_v);
                    free(freqs  ? freqs  : old_f);
                    pclose(fp);
                    sdr->active = 0;
                    return -1;
                }
            }

            values[n_values] = dbm;
            freqs[n_values]  = start_hz + (double)b * bin_hz;
            n_values++;

            char *q = strchr(p, ',');
            if (!q) break;
            p = q + 1;
        }
    }

    int status = pclose(fp);
    (void)status;

    if (n_values == 0) {
        free(values);
        free(freqs);
        sdr->active = 0;
        return -1;
    }

    /* ── 找峰值 (最大值及其对应频率) ── */
    double peak_val = -200.0;
    double peak_f   = 0.0;
    for (int i = 0; i < n_values; i++) {
        if (values[i] > peak_val) {
            peak_val = values[i];
            peak_f   = freqs[i];
        }
    }
    sdr->peak_power = peak_val;
    sdr->peak_freq  = peak_f;

    /* ── 噪底 = 第5百分位 ── */
    qsort(values, n_values, sizeof(double), cmp_double);

    int p5_idx = (int)(n_values * 0.05);
    if (p5_idx < 0) p5_idx = 0;
    if (p5_idx >= n_values) p5_idx = n_values - 1;
    sdr->noise_floor = values[p5_idx];

    /* ── SNR = 峰值 - 噪底 ── */
    sdr->avg_snr = sdr->peak_power - sdr->noise_floor;

    /* ── 写入 local_sdr 表 (用现有 wt_local_save_sdr 接口) ── */
    wt_sdr_t s;
    memset(&s, 0, sizeof(s));
    s.ts = time(NULL);
    snprintf(s.file, sizeof(s.file), "live_sdr_%ld.csv", s.ts);
    snprintf(s.band, sizeof(s.band),
             "GNSS L1 Live (%.0f-%.0fMHz)",
             start_mhz, end_mhz);
    s.noise_floor_dbm = sdr->noise_floor;
    s.peak_freq_mhz   = sdr->peak_freq / 1e6;
    s.peak_dbm        = sdr->peak_power;
    s.peak_snr        = sdr->avg_snr;

    wt_local_save_sdr(&s);

    sdr->last_update = s.ts;

    free(values);
    free(freqs);
    sdr->active = 0;
    return 0;
}

/* ── 停止 (清理状态) ────────────────────────────────────── */
void wt_sdr_live_stop(wt_sdr_live_t *sdr) {
    if (!sdr) return;
    sdr->active      = 0;
    sdr->noise_floor = 0.0;
    sdr->peak_freq   = 0.0;
    sdr->peak_power  = 0.0;
    sdr->avg_snr     = 0.0;
}
