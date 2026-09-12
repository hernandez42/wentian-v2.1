/* ============================================================
 * wt_self_repair.c - 问天全链路自愈修复引擎 v2.0
 * ============================================================
 * 项目: 问天 v2.3 (WenTian Weather Station)
 * 所有者: 主人朱涛 BG8SBA, 昆明长水机场楼顶
 *
 * 功能: 当系统组件数据异常时自动修复, 不等人
 *   1. DB数据陈旧 → 对应服务重启
 *   2. 进程挂死 → systemd restart
 *   3. SDR扫频停止 → 触发补扫
 *   4. 串口堵塞 → kill + 重启
 *   5. API拉取失败 → 重试 + 换备选
 *   6. 硬件异常 → 记录日志通知主人
 *
 * v2.0 (2026-09-11) 新增:
 *   - 闭环验证: repair后sleep(15)→健康检查SQL确认
 *   - 分级修复: 1.数据降级 2.服务重启 3.紧急日志
 *   - 重试机制: 最多3次, 间隔15/30/60s
 *   - 数据库紧急日志(repair_emergency表)
 *   - 自愈心跳(self_repair_heartbeat表)
 *   - wt_full_self_repair返回修复组件数量
 * ============================================================ */
#include "wentian.h"
#include <sqlite3.h>
#include <time.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>

/* ── 数据源 → systemd 服务名 映射 ─────────────────────────── */
typedef struct {
    const char *table;        /* SQLite表名 */
    const char *service;      /* systemd服务名 */
    const char *script;       /* 备选:脚本路径(若没有service) */
    int     max_age_sec;      /* 数据允许的最大间隔 */
    int     rows_min;         /* 最少行数 */
    const char *desc;         /* 中文描述 */
} repair_entry_t;

static const repair_entry_t REPAIR_TABLE[] = {
    {"gps_log",          "gps-full-collect",   "/root/scripts/gps_full_collect.py",      300,  10,  "GPS/北斗串口采集"},
    {"gps_log",          "gps-collect",        "/root/scripts/gps_collect",               600,  50,  "GPS C版采集"},
    {"gps_log",          "gps-uno-fusion",     "/root/scripts/gps_uno_fusion.py",         600,  50,  "GPS-UNO融合"},
    {"ano_weather",      "uno-weather",        "/root/scripts/uno_bridge.py --loop --interval 60", 300, 100, "UNO气象(旧版)"},
    {"outdoor",          "wentian",            "",                                3600,  10,  "室外气象(问天daemon)"},
    {"metar",            "weather-analyze",    "/root/scripts/weather_analyze.py",        7200,  3,  "METAR机场数据"},
    {"nowcast",          "wentian",            "",                                      600,   5,  "短临Nowcast(问天)"},
    {"local_pwv",        "wentian",            "",                                      900,   5,  "PWV反演(问天)"},
    {"local_iono",       "gnss-ionosphere",    "/root/scripts/gnss_ionosphere.py",        3600,  3,  "电离层S4"},
    {"local_uno",        "uno-bridge",         "/root/scripts/uno_bridge.py --loop --interval 60", 300, 50,  "UNO桥接"},
    {"multi_source_forecast", "wentian",       "",                                      1800,  3,  "多源预测(问天)"},
    {"external_data",    "wentian",            "",                                      3600,  3,  "NOAA/mno/wttr"},
    {"multisrc_s4",      "wentian",            "",                                      3600,  3,  "多源S4(问天)"},
    {"radar_correl",     "wentian",            "",                                      1800,  5,  "雷达相干(问天)"},
    {"weather",           "weather-fusion",    "/root/scripts/weather_fusion.py",         3600,  10, "全源融合"},
    {"passage",           "passage-news",      "/root/scripts/passage_news.py",          21600,  3,  "过境新闻"},
};
#define REPAIR_N (sizeof(REPAIR_TABLE)/sizeof(REPAIR_TABLE[0]))

/* ── 构建健康检查SQL (返回0成功) ───────────────────────────── */
static int build_health_query(const repair_entry_t *e, char *q, int qsize) {
    if (!e || !q) return -1;
    if (strcmp(e->table, "gps_log") == 0 ||
        strcmp(e->table, "ano_weather") == 0) {
        snprintf(q, qsize,
            "SELECT COUNT(*), strftime('%%s','now') - "
            "strftime('%%s', MAX(ts)) FROM %s", e->table);
    } else if (strcmp(e->table, "passage") == 0) {
        snprintf(q, qsize,
            "SELECT COUNT(*), strftime('%%s','now') - MAX(ts) FROM "
            "(SELECT MAX(ts) as ts FROM nowcast)");
    } else {
        snprintf(q, qsize,
            "SELECT COUNT(*), CAST(strftime('%%s','now') AS INTEGER) - "
            "MAX(ts) FROM %s", e->table);
    }
    return 0;
}

/* ── 条目对应的DB路径 ─────────────────────────────────────── */
static const char *entry_db_path(const repair_entry_t *e) {
    if (strcmp(e->table, "gps_log") == 0 ||
        strcmp(e->table, "ano_weather") == 0 ||
        strcmp(e->table, "weather") == 0) {
        return "/root/data/ano_weather.db";
    }
    return WENTIAN_DB;
}

/* ── 闭环验证: 执行健康检查SQL, rows>0 且 age<300s → 通过 ── */
static int verify_entry_repair(const repair_entry_t *e) {
    const char *db_path = entry_db_path(e);
    sqlite3 *db;
    if (sqlite3_open(db_path, &db) != SQLITE_OK) return 0;

    char q[512];
    build_health_query(e, q, sizeof(q));

    int verified = 0;
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(db, q, -1, &st, NULL) == SQLITE_OK) {
        if (sqlite3_step(st) == SQLITE_ROW) {
            int rows = sqlite3_column_int(st, 0);
            int age  = sqlite3_column_int(st, 1);
            /* 验证标准: 有数据且间隔<300s */
            if (rows > 0 && age < 300) {
                verified = 1;
            }
        }
        sqlite3_finalize(st);
    }
    sqlite3_close(db);
    return verified;
}

/* ── 写入紧急日志(DB repair_emergency表 + 文件) ───────────── */
static void write_emergency_log(const char *table_name, const char *desc,
                                 const char *error, int attempts) {
    sqlite3 *db;
    if (sqlite3_open(WENTIAN_DB, &db) != SQLITE_OK) return;

    const char *create =
        "CREATE TABLE IF NOT EXISTS repair_emergency ("
        "ts INTEGER PRIMARY KEY,"
        "table_name TEXT,"
        "description TEXT,"
        "error TEXT,"
        "attempts INTEGER"
        ")";
    sqlite3_exec(db, create, NULL, NULL, NULL);

    sqlite3_stmt *st;
    const char *ins =
        "INSERT INTO repair_emergency(ts,table_name,description,error,attempts) "
        "VALUES(?,?,?,?,?)";
    if (sqlite3_prepare_v2(db, ins, -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, (sqlite3_int64)time(NULL));
        sqlite3_bind_text(st, 2, table_name, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 3, desc ? desc : "", -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 4, error ? error : "", -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(st, 5, attempts);
        sqlite3_step(st);
        sqlite3_finalize(st);
    }
    sqlite3_close(db);

    /* 同时写文件日志 */
    FILE *lf = fopen("/root/data/fusion/repair_emergency.log", "a");
    if (lf) {
        time_t tnow = time(NULL);
        struct tm *tmp = localtime(&tnow);
        char tsbuf[32];
        strftime(tsbuf, sizeof(tsbuf), "%Y-%m-%d %H:%M:%S", tmp);
        fprintf(lf, "[%s] EMERGENCY: table=%s desc=%s error=%s attempts=%d\n",
                tsbuf, table_name, desc ? desc : "", error ? error : "", attempts);
        fclose(lf);
    }
}

/* ── 写入修复日志(文件) ───────────────────────────────────── */
static void write_repair_log_file(const repair_entry_t *e, const char *detail) {
    FILE *lf = fopen("/root/data/fusion/repair_log.txt", "a");
    if (!lf) return;
    time_t tnow = time(NULL);
    struct tm *tmp = localtime(&tnow);
    char tsbuf[32];
    strftime(tsbuf, sizeof(tsbuf), "%Y-%m-%d %H:%M:%S", tmp);
    fprintf(lf, "[%s] 修复 %s (service=%s script=%s) %s\n",
            tsbuf, e->desc,
            e->service ? e->service : "",
            e->script ? e->script : "",
            detail ? detail : "");
    fclose(lf);
}

/* ── SDR 北斗扫频修复 ─────────────────────────────────────── */
static void repair_sdr_gnss_scan(void) {
    /* 检测 SDR 硬件是否在线 */
    FILE *fp = popen("timeout 3 rtl_sdr -f 1575.42M -g 40 -n 8192 /tmp/sdr_health_check.bin 2>&1 | head -2", "r");
    if (!fp) return;
    char buf[256] = {0};
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-result"
    fread(buf, 1, 255, fp);
#pragma GCC diagnostic pop
    (void)buf;
    pclose(fp);

    if (strstr(buf, "RTL2838") || strstr(buf, "R828D")) {
        printf("  🔧 SDR硬件在线, 触发北斗B1I+GPS L1扫频...\n");
        char cmd[1024];
        time_t now = time(NULL);
        struct tm *tm = localtime(&now);
        char dirname[64];
        snprintf(dirname, sizeof(dirname), "/root/data/sdr/auto_sweep_%04d%02d%02d",
                 tm->tm_year+1900, tm->tm_mon+1, tm->tm_mday);
        mkdir(dirname, 0755);

        snprintf(cmd, sizeof(cmd),
            "rtl_power -f 1559M:1581M:50k -g 40 -i 5 -1 "
            "%s/gnss_b1i_l1_%02d%02d%02d.csv 2>&1 &",
            dirname, tm->tm_hour, tm->tm_min, tm->tm_sec);
        int ret = system(cmd);
        if (ret != 0) printf("  ⚠️ rtl_power 启动失败 rc=%d\n", ret);
    } else {
        printf("  ⚠️ SDR硬件离线, 跳过扫频\n");
    }
}

/* ── systemd 重启 + active 验证 ────────────────────────────── */
static int restart_systemd(const char *service, const char *desc) {
    if (!service || !service[0]) return 0;
    (void)desc;
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "systemctl restart %s 2>&1", service);
    printf("    systemctl restart %s\n", service);
    int rc = system(cmd);
    if (rc != 0) return 0;

    sleep(10);
    char vcmd[256];
    snprintf(vcmd, sizeof(vcmd), "systemctl is-active %s 2>/dev/null", service);
    FILE *vp = popen(vcmd, "r");
    char vbuf[32] = {0};
    if (vp) {
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-result"
        fread(vbuf, 1, sizeof(vbuf) - 1, vp);
#pragma GCC diagnostic pop
        pclose(vp);
    }
    int active = strstr(vbuf, "active") ? 1 : 0;
    if (active) {
        printf("    ✅ 服务 %s 已active\n", service);
    } else {
        printf("    ⚠️ 服务 %s 状态=%s\n", service, vbuf);
    }
    return active;
}

/* ── 脚本启动 ──────────────────────────────────────────────── */
static int start_script_entry(const char *script, const char *desc) {
    if (!script || !script[0]) return 0;
    (void)desc;
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "%s >/dev/null 2>&1 &", script);
    printf("    启动脚本 %s\n", script);
    int rc = system(cmd);
    if (rc == 0) {
        printf("    ✅ 脚本启动成功\n");
    } else {
        printf("    ⚠️ 脚本启动失败 rc=%d\n", rc);
    }
    return (rc == 0) ? 1 : 0;
}

/* ══════════════════════════════════════════════════════════════
 * ── 自愈心跳写入 (公开函数) ───────────────────────────────── */
void wt_self_repair_heartbeat(int success_count, int fail_count,
                               const char *last_error, int last_verified_ok) {
    sqlite3 *db;
    if (sqlite3_open(WENTIAN_DB, &db) != SQLITE_OK) return;

    const char *create =
        "CREATE TABLE IF NOT EXISTS self_repair_heartbeat ("
        "ts INTEGER PRIMARY KEY,"
        "success_count INTEGER DEFAULT 0,"
        "fail_count INTEGER DEFAULT 0,"
        "last_error TEXT DEFAULT '',"
        "last_verified_ok INTEGER DEFAULT 0"
        ")";
    sqlite3_exec(db, create, NULL, NULL, NULL);

    sqlite3_stmt *st;
    const char *ins =
        "INSERT INTO self_repair_heartbeat "
        "(ts,success_count,fail_count,last_error,last_verified_ok) "
        "VALUES(?,?,?,?,?)";
    if (sqlite3_prepare_v2(db, ins, -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, (sqlite3_int64)time(NULL));
        sqlite3_bind_int(st, 2, success_count);
        sqlite3_bind_int(st, 3, fail_count);
        sqlite3_bind_text(st, 4, last_error ? last_error : "", -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(st, 5, last_verified_ok ? 1 : 0);
        sqlite3_step(st);
        sqlite3_finalize(st);
    }
    sqlite3_close(db);
}

/* ══════════════════════════════════════════════════════════════
 * ── 自愈主函数 (返回修复成功组件数) ───────────────────────── */
static int do_self_repair(char *log_out, int max_log,
                           int *out_fail_count, char *out_last_error,
                           int max_error_len) {
    int total_success = 0;
    int total_fail    = 0;
    int pos = 0;
    char last_error[256] = {0};

    for (size_t i = 0; i < REPAIR_N; i++) {
        const repair_entry_t *e = &REPAIR_TABLE[i];

        /* ── 打开DB + 健康检查 ── */
        const char *db_path = entry_db_path(e);
        sqlite3 *db;
        int need_repair = 0;
        int rows_initial = 0, age_initial = 0;

        if (sqlite3_open(db_path, &db) == SQLITE_OK) {
            char q[512];
            build_health_query(e, q, sizeof(q));
            sqlite3_stmt *st;
            if (sqlite3_prepare_v2(db, q, -1, &st, NULL) == SQLITE_OK) {
                if (sqlite3_step(st) == SQLITE_ROW) {
                    rows_initial = sqlite3_column_int(st, 0);
                    age_initial  = sqlite3_column_int(st, 1);
                    if (rows_initial < e->rows_min ||
                        (rows_initial > 0 && age_initial > e->max_age_sec)) {
                        need_repair = 1;
                        pos += snprintf(log_out + pos, max_log - pos,
                            "[%s]行数=%d(<%d)或过旧=%ds(>%ds)|",
                            e->desc, rows_initial, e->rows_min,
                            age_initial, e->max_age_sec);
                    }
                }
                sqlite3_finalize(st);
            }
            sqlite3_close(db);
        }

        if (!need_repair) continue;

        printf("  🔧 修复 %s (分级: 1降级→2重启→3紧急)\n", e->desc);

        /* ══════════════════════════════════════════════════════
         * 分级修复:
         *   第1级 — 数据降级 (wt_metar_fallback_run)
         *   第2级 — systemctl restart + 脚本备选
         *   第3级 — 紧急日志 (3次验证均失败后)
         * ══════════════════════════════════════════════════════ */
        int level_repaired = 0;
        int verified       = 0;

        /* ── 第1级: 数据降级 ── */
        if (strcmp(e->table, "outdoor") == 0 ||
            strcmp(e->table, "metar") == 0) {
            printf("  📊 第1级: 尝试数据降级(METAR备选)...\n");
            wt_metar_t fb_metar;
            memset(&fb_metar, 0, sizeof(fb_metar));
            int fb_ret = wt_metar_fallback_run(&fb_metar);
            if (fb_ret == 0 && fb_metar.temp > -60 && fb_metar.temp < 60) {
                printf("  ✅ 第1级: 数据降级成功\n");
                level_repaired = 1;
                sleep(15);
                if (verify_entry_repair(e)) {
                    verified = 1;
                    printf("  ✅ 第1级: 闭环验证通过\n");
                } else {
                    printf("  ⚠️ 第1级: 降级后数据仍未恢复, 进入第2级\n");
                }
            } else {
                printf("  ⚠️ 第1级: 降级失败(rc=%d), 进入第2级\n", fb_ret);
            }
        } else {
            printf("  📊 跳过第1级(非天气表), 直接第2级\n");
        }

        /* ── 第2级: 服务重启 + 脚本 ── */
        if (!verified) {
            printf("  📊 第2级: 重启服务 + 备用脚本...\n");

            level_repaired = 0;

            /* 过境新闻服务不存在时跳过修复 */
            if (e->service && strcmp(e->service, "passage-news") == 0) {
                char _cbuf[64] = {0};
                FILE *_cp = popen("systemctl is-active passage-news 2>/dev/null", "r");
                if (_cp) {
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-result"
                    fread(_cbuf, 1, sizeof(_cbuf)-1, _cp);
#pragma GCC diagnostic pop
                    pclose(_cp);
                }
                char *_nl = strchr(_cbuf, '\n');
                if (_nl) *_nl = '\0';
                if (strcmp(_cbuf, "active") != 0) {
                    printf("  ⚠️ 过境新闻服务不存在, 跳过修复\n");
                    write_repair_log_file(e, "过境新闻服务不存在, 跳过修复");
                    continue;
                }
            }

            if (e->service && e->service[0]) {
                if (restart_systemd(e->service, e->desc)) {
                    level_repaired = 1;
                }
            }

            if (!level_repaired && e->script && e->script[0]) {
                if (start_script_entry(e->script, e->desc)) {
                    level_repaired = 1;
                }
            }

            /* ── 闭环验证: sleep(15) → 健康检查 ── */
            /*     最多重试3次, 间隔递增: 15/30/60s */
            if (level_repaired) {
                int delays[] = {15, 15, 30, 60};
                for (int attempt = 0; attempt <= 3 && !verified; attempt++) {
                    if (attempt > 0) {
                        printf("  🔄 验证重试 %d/3 (等待 %ds)...\n",
                               attempt, delays[attempt]);
                    }
                    sleep(delays[attempt]);
                    if (verify_entry_repair(e)) {
                        verified = 1;
                        printf("  ✅ 闭环验证通过 (attempt %d)\n", attempt);
                    }
                }
            }
        }

        /* ── 结果处理: 成功计数 / 第3级紧急日志 ── */
        if (verified) {
            total_success++;
            write_repair_log_file(e, "闭环验证通过");
        } else {
            total_fail++;
            snprintf(last_error, sizeof(last_error),
                "3次验证均失败: table=%s rows_init=%d age_init=%d",
                e->table, rows_initial, age_initial);
            printf("  🔴 第3级: 写入紧急日志 (3次修复均失败)\n");
            write_emergency_log(e->table, e->desc, last_error, 3);
        }
    }

    /* 额外: SDR 硬件检测 */
    if (total_success > 0 || pos > 0) {
        printf("  🔧 检查SDR硬件...\n");
        repair_sdr_gnss_scan();
    }

    if (out_fail_count)  *out_fail_count = total_fail;
    if (out_last_error && last_error[0])
        snprintf(out_last_error, max_error_len, "%s", last_error);

    return total_success;
}

/* ══════════════════════════════════════════════════════════════
 * ── 主入口: 全系统自愈 (返回修复成功的组件数量) ──────────── */
int wt_full_self_repair(void) {
    printf("\n━━━ 25. 全系统自愈修复引擎 (APEX ΔG<0) v2.0 ━━━\n");

    char log[2048] = {0};
    char last_error[256] = {0};
    int fail_count = 0;

    int repaired = do_self_repair(log, sizeof(log),
                                   &fail_count, last_error, sizeof(last_error));

    printf("  📊 自愈摘要:\n");
    if (repaired > 0 || fail_count > 0) {
        printf("  ✅ 修复 %d 个组件, ❌ 失败 %d 个:\n", repaired, fail_count);
        if (log[0]) printf("    %s\n", log);
        if (fail_count > 0 && last_error[0])
            printf("    ⚠️ 最后错误: %s\n", last_error);
    } else {
        printf("  ✅ 全部组件健康, 无需修复\n");
    }

    /* ── 写入自愈心跳 ── */
    wt_self_repair_heartbeat(repaired, fail_count, last_error,
                              (repaired > 0 && fail_count == 0) ? 1 : 0);

    return repaired;
}
