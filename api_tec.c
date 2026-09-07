/* ============================================================
 * api_tec.c - 等效 TEC 估算引擎 v2.0
 * ============================================================
 * 项目: 问天 v2.3 (WenTian Weather Station)
 * 所有者: 主人朱涛 BG8SBA, 昆明长水机场
 *
 * 诚实声明: 问天是单频GPS, 无法做双频差分TEC
 * 以下TEC数据均为经验估算, 非真实测量
 * 详见 WENTIAN-NOTICE.md "数据局限性"
 *
 * 估算方法:
 *   1. Kp → TEC: 经验公式 TEC = (Kp + 1) * 5 TECU
 *      物理依据: Kp反映地磁活动 → 影响电离层电子密度
 *      局限: 粗略线性, 未考虑纬度/季节/地方时
 *   2. S4 → TEC: 关联性存在(TEC高时闪烁强), 但非定量
 *      暂不启用, 仅作参考
 *   3. F10.7 → TEC: 太阳射电通量→EUV电离率
 *      TEC = F10.7 * 0.1 TECU (经验估算)
 *
 * 输出: /root/data/fusion/tec_multi.json
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
    printf("\n━━━ 26. 等效 TEC 经验估算 (非真实双频测量) ━━━\n");

    /* 1. 取 Kp + F10.7 */
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
        /* 取最新的S4 */
        if (sqlite3_prepare_v2(db, "SELECT s4_gps FROM local_ionosphere ORDER BY ts DESC LIMIT 1", -1, &st, NULL) == SQLITE_OK) {
            if (sqlite3_step(st) == SQLITE_ROW) {
                s4_t = sqlite3_column_double(st, 0);
                if (s4_t < 0) s4_t = 0;
            }
            sqlite3_finalize(st);
        }
        sqlite3_close(db);
    }

    /* 2. 三源估算 (均标记为经验/非实测) */
    double tec_kp = (kp >= 0) ? (kp + 1.0) * 5.0 : -1;
    double tec_f107 = (f107 >= 0) ? f107 * 0.1 : -1;
    /* S4→TEC关联性存在但无定量公式, 仅打印供参考 */
    double tec_s4 = (s4_t > 0.01) ? s4_t * 100.0 : -1;

    printf("  ── 三源经验估算(非真实测量) ──\n");
    printf("    Kp=%.1f → TEC≈%.0f TECU | F10.7=%.0f → TEC≈%.0f TECU",
           kp, tec_kp, f107, tec_f107);
    if (tec_s4 > 0) printf(" | S4=%.3f → TEC≈%.0f TECU", s4_t, tec_s4);
    printf("\n");

    /* 3. 融合 (加权) */
    double tec_fused = 0, total_w = 0;
    int n_src = 0;
    if (tec_kp > 0) { tec_fused += tec_kp * 0.5; total_w += 0.5; n_src++; }
    if (tec_f107 > 0) { tec_fused += tec_f107 * 0.3; total_w += 0.3; n_src++; }
    if (tec_s4 > 0) { tec_fused += tec_s4 * 0.2; total_w += 0.2; n_src++; }

    if (total_w == 0) {
        printf("  ── 无有效数据源, 无法估算TEC\n");
        return 0;
    }
    tec_fused /= total_w;

    printf("  ── 加权融合结果(经验) ──\n");
    printf("     等效 TEC ≈ %.1f TECU (源数:%d/3, 经验估算, 非双频测量)\n",
           tec_fused, n_src);

    /* 4. 存JSON */
    FILE *f = fopen(TEC_JSON, "w");
    if (f) {
        fprintf(f, "{\n");
        fprintf(f, "  \"ts\": %ld,\n", (long)time(NULL));
        fprintf(f, "  \"note\": \"经验估算(非双频测量), 仅供参考\",\n");
        fprintf(f, "  \"tec_kp_est\": %.1f,\n", tec_kp);
        fprintf(f, "  \"tec_f107_est\": %.1f,\n", tec_f107);
        fprintf(f, "  \"tec_fused_est\": %.1f,\n", tec_fused);
        fprintf(f, "  \"sources\": %d\n", n_src);
        fprintf(f, "}\n");
        fclose(f);
        printf("  ✅ 已存入 tec_multi.json\n");
    }
    return 0;
}
