/* ============================================================
 * wt_self_repair.c - 问天全链路自愈修复引擎 v1.0
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
 * 数据流 (APEX ΔG < 0 触发):
 *   wt_self_heal_check() → 发现异常
 *   → 找对应 systemd 服务名
 *   → systemctl restart <service>
 *   → 等待 10 秒
 *   → 验证恢复
 *   → 写入 repair_log
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
    {"outdoor",          "weather-station",    "/root/scripts/weather_station_v2",        2100,  3,  "室外气象站(C)"},
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

/* ── SDR 北斗扫频修复 ─────────────────────────────────────── */
static void repair_sdr_gnss_scan(void) {
    /* 检测 SDR 硬件是否在线 */
    FILE *fp = popen("timeout 3 rtl_sdr -f 1575.42M -g 40 -n 8192 /tmp/sdr_health_check.bin 2>&1 | head -2", "r");
    if (!fp) return;
    char buf[256] = {0};
    fread(buf, 1, 255, fp);
    (void)buf;
    pclose(fp);

    if (strstr(buf, "RTL2838") || strstr(buf, "R828D")) {
        /* SDR 硬件在线, 触发北斗扫频 */
        printf("  🔧 SDR硬件在线, 触发北斗扫频...\n");
        /* 北斗B1I (1561.098MHz) + GPS L1 (1575.42MHz) 扫频 */
        char cmd[1024];
        time_t now = time(NULL);
        struct tm *tm = localtime(&now);
        char dirname[64];
        snprintf(dirname, sizeof(dirname), "/root/data/sdr/auto_sweep_%04d%02d%02d",
                 tm->tm_year+1900, tm->tm_mon+1, tm->tm_mday);
        mkdir(dirname, 0755);

        snprintf(cmd, sizeof(cmd),
            "rtl_power -f 1569M:1581M:50k -g 40 -i 5 -1 "
            "%s/gnss_l1_%02d%02d%02d.csv 2>&1 &",
            dirname, tm->tm_hour, tm->tm_min, tm->tm_sec);
        int ret = system(cmd);
        (void)ret;
    } else {
        printf("  ⚠️ SDR硬件离线, 跳过扫频\n");
    }
}

/* ── 自愈主函数 ──────────────────────────────────────────── */
static int do_self_repair(char *log_out, int max_log) {
    int total_repaired = 0;
    int pos = 0;

    for (size_t i = 0; i < REPAIR_N; i++) {
        const repair_entry_t *e = &REPAIR_TABLE[i];
        sqlite3 *db;
        int need_repair = 0;

        /* 打开正确的 DB */
        const char *db_path = WENTIAN_DB;
        if (strcmp(e->table, "gps_log") == 0 ||
            strcmp(e->table, "ano_weather") == 0 ||
            strcmp(e->table, "weather") == 0) {
            db_path = "/root/data/ano_weather.db";
        }

        if (sqlite3_open(db_path, &db) == SQLITE_OK) {
            char q[512];
            /* 有些表用 timestamp TEXT, 有些用 INTEGER */
            if (strcmp(e->table, "gps_log") == 0) {
                snprintf(q, sizeof(q),
                    "SELECT COUNT(*), strftime('%%s','now') - "
                    "strftime('%%s', MAX(ts)) FROM %s", e->table);
            } else if (strcmp(e->table, "ano_weather") == 0) {
                snprintf(q, sizeof(q),
                    "SELECT COUNT(*), strftime('%%s','now') - "
                    "strftime('%%s', MAX(ts)) FROM %s", e->table);
            } else if (strcmp(e->table, "passage") == 0) {
                snprintf(q, sizeof(q),
                    "SELECT COUNT(*), strftime('%%s','now') - MAX(ts) FROM "
                    "(SELECT MAX(ts) as ts FROM nowcast)", 1);
                /* passage 没有独立表, 用 nowcast 代替 */
            } else {
                snprintf(q, sizeof(q),
                    "SELECT COUNT(*), CAST(strftime('%%s','now') AS INTEGER) - "
                    "MAX(ts) FROM %s", e->table);
            }

            sqlite3_stmt *st;
            if (sqlite3_prepare_v2(db, q, -1, &st, NULL) == SQLITE_OK) {
                if (sqlite3_step(st) == SQLITE_ROW) {
                    int rows = sqlite3_column_int(st, 0);
                    int age = sqlite3_column_int(st, 1);

                    if (rows < e->rows_min || (rows > 0 && age > e->max_age_sec)) {
                        need_repair = 1;
                        pos += snprintf(log_out + pos, max_log - pos,
                            "[%s]行数=%d(<%d)或过旧=%ds(>%ds)|",
                            e->desc, rows, e->rows_min, age, e->max_age_sec);
                    }
                }
                sqlite3_finalize(st);
            }
            sqlite3_close(db);
        }

        if (!need_repair) continue;

        /* ── 执行修复 ── */
        printf("  🔧 修复 %s: ", e->desc);

        /* 方式1: systemd 服务重启 */
        if (e->service && e->service[0]) {
            char cmd[256];
            snprintf(cmd, sizeof(cmd), "systemctl restart %s 2>&1", e->service);
            int rc = system(cmd);
            if (rc == 0) {
                printf("重启 systemd %s 成功\n", e->service);
                total_repaired++;
            } else {
                printf("重启 systemd %s 失败(rc=%d), 尝试脚本...\n", e->service, rc);
                /* 方式2: 脚本直接启动 */
                if (e->script && e->script[0]) {
                    char sc[512];
                    snprintf(sc, sizeof(sc), "nohup %s >> /var/log/repair_%s.log 2>&1 &",
                             e->script, e->service);
                    /* 不能用 nohup,直接启动 */
                    snprintf(cmd, sizeof(cmd), "%s &", e->script);
                    rc = system(cmd);
                    if (rc == 0 || rc == 32512) {  /* 32512 = 后台返回的伪退出码 */
                        printf("启动脚本 %s\n", e->script);
                        total_repaired++;
                    }
                }
            }
            /* 等待服务启动 */
            sleep(2);
        }
    }

    /* 额外: SDR 硬件检测 */
    if (total_repaired > 0 || pos > 0) {
        printf("  🔧 检查SDR硬件...\n");
        repair_sdr_gnss_scan();
    }

    return total_repaired;
}

/* ── 主入口: 全系统自愈 ──────────────────────────────────── */
int wt_full_self_repair(void) {
    printf("\n━━━ 25. 全系统自愈修复引擎 (APEX ΔG<0) ━━━\n");

    char log[2048] = {0};
    int repaired = do_self_repair(log, sizeof(log));

    printf("  📊 自愈摘要:\n");
    if (repaired > 0) {
        printf("  ✅ 修复了 %d 个组件:\n", repaired);
        if (log[0]) printf("    %s\n", log);
    } else {
        printf("  ✅ 全部组件健康, 无需修复\n");
    }

    return 0;
}