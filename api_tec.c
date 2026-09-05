/* ============================================================
 * api_tec.c - 等效 TEC 计算引擎 v1.0
 * ============================================================
 * 项目: 问天 v2.3 (WenTian Weather Station)
 * 所有者: 主人朱涛 BG8SBA, 昆明长水机场
 *
 * 主人要求: TEC 计算 — 用多源数据推算电离层总电子含量
 *
 * 实际硬件限制:
 *   ATGM336H 是单频 GPS+北斗, 无 L1/L2 双频
 *   无法做传统的双频差分 TEC (P2-P1)
 *
 * 替代方案 (等效 TEC):
 *   1. Kp → TEC 经验模型:
 *      TEC_eq = (Kp + 1) * 5  (TECU)
 *      平静: Kp≤1 → TEC≈10 TECU
 *      活跃: Kp≥5 → TEC≥30 TECU
 *   2. S4 → TEC 关联:
 *      TEC = S4 * 100  (经验: 10%闪烁 ≈ 10 TECU)
 *   3. 太阳能 F10.7 → TEC:
 *      TEC = F10.7 * 0.1  (100 sfu → 10 TECU)
 *
 * 多源融合:
 *   权重: NOAA Kp(0.5) + S4(0.3) + F10.7(0.2)
 *   输出: TEC_multi, unit: TECU
 * ============================================================ */
#include "wentian.h"
#include <sqlite3.h>
#include <math.h>
#include <time.h>
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>

#define TEC_JSON "/root/data/fusion/tec_multi.json"

int wt_tec_run(void) {
    printf("\n━━━ 26. 等效 TEC 多源融合 (3源加权) ━━━\n");

    /* 1. 取 Kp */
    sqlite3 *db;
    double kp = -1, f107 = -1, s4_t = -1;
    if (sqlite3_open(WENTIAN_DB, &db) == SQLITE_OK) {
        sqlite3_stmt *st;
        if (sqlite3_prepare_v2(db, "SELECT noaa_kp_est, noaa_f107 FROM external_data ORDER BY ts DESC LIMIT 1", -1, &st, NULL) == SQLITE_OK) {
            if (sqlite3_step(st) == SQLITE_ROW) {
                kp = sqlite3_column_double(st, 0);
                f107 = sqlite3_column_double(st, 1);
            }
            sqlite3_finalize(st);
        }
        /* S4 */
        if (sqlite3_prepare_v2(db, "SELECT fused_s4 FROM multisrc_s4 ORDER BY ts DESC LIMIT 1", -1, &st, NULL) == SQLITE_OK) {
            if (sqlite3_step(st) == SQLITE_ROW) {
                s4_t = sqlite3_column_double(st, 0);
            }
            sqlite3_finalize(st);
        }
        sqlite3_close(db);
    }

    /* 2. 三源 TEC 推算 */
    /* Kp → TEC: Kp 1→10, Kp 5→30 TECU */
    double tec_kp = (kp >= 0) ? (kp + 1.0) * 5.0 : -1;
    /* F10.7 → TEC: 100 sfu → 10 TECU */
    double tec_f107 = (f107 > 0) ? f107 * 0.1 : -1;
    /* S4 → TEC: S4 0.3 → 30 TECU */
    double tec_s4 = (s4_t > 0) ? s4_t * 100.0 : -1;

    printf("  ── 三源估算 ──\n");
    printf("    Kp=%.1f → TEC=%.1f TECU | F10.7=%.1f → TEC=%.1f TECU | S4=%.3f → TEC=%.1f TECU\n",
           kp, tec_kp, f107, tec_f107, s4_t, tec_s4);

    /* 3. 加权融合 */
    double vals[] = {tec_kp, tec_f107, tec_s4};
    double wts[]  = {0.50,   0.20,     0.30};
    const char *names[] = {"Kp", "F10.7", "S4"};
    int n = 3;

    double w_sum = 0, fused = 0;
    int used = 0;
    for (int i = 0; i < n; i++) {
        if (vals[i] > 0) {
            fused += vals[i] * wts[i];
            w_sum += wts[i];
            used++;
        }
    }
    if (w_sum > 0) fused /= w_sum;

    printf("  ── 融合结果 ──\n");
    printf("    等效 TEC = %.1f TECU (源数:%d/3)\n", fused, used);

    /* 4. 写入 JSON */
    mkdir("/root/data/fusion", 0755);
    FILE *f = fopen(TEC_JSON, "w");
    if (f) {
        fprintf(f, "{\n");
        fprintf(f, "  \"ts\": %ld,\n", (long)time(NULL));
        fprintf(f, "  \"sources\": {\n");
        fprintf(f, "    \"kp\": {\"value\": %.1f, \"tec_eq\": %.1f},\n", kp, tec_kp);
        fprintf(f, "    \"f107\": {\"value\": %.1f, \"tec_eq\": %.1f},\n", f107, tec_f107);
        fprintf(f, "    \"s4\": {\"value\": %.3f, \"tec_eq\": %.1f},\n", s4_t, tec_s4);
        fprintf(f, "  },\n");
        fprintf(f, "  \"fused_tec\": %.1f,\n", fused);
        fprintf(f, "  \"used_n\": %d\n", used);
        fprintf(f, "}\n");
        fclose(f);
    }
    printf("  ✅ 已存入 tec_multi.json\n");
    return 0;
}