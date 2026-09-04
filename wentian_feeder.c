/* ============================================================
 * wentian_feeder.c - 问天数据导出器 (供Python推送系统读取) v1.1
 * ============================================================
 * 项目: 问天 v1.1 (WenTian Weather Station)
 * 锁定: 详见 WENTIAN-LOCK.md
 *
 * 职责: 从 /root/data/wentian.db 14张表读最新一行,
 *       汇总成 JSON 写到 /root/data/fusion/wentian_latest.json
 *       供 feishu_ultimate_push.py 等Python推送脚本读取
 *
 * 设计原则:
 *   - C 为权威数据源 (主人硬性)
 *   - Python 仅胶水 (推送层)
 *   - JSON 桥接: C写, Python只读
 *
 * 调用: ./wentian_feeder once        (单次)
 *       ./wentian_feeder daemon 60   (60秒循环)
 * ============================================================ */
#include "wentian.h"
#include <sqlite3.h>
#include <sys/stat.h>
#include <unistd.h>

#define OUT_PATH "/root/data/fusion/wentian_latest.json"

static void write_kv(FILE *f, const char *key, const char *val, int last) {
    fprintf(f, "    \"%s\": \"%s\"%s\n", key, val, last ? "" : ",");
}
static void write_kv_num(FILE *f, const char *key, double val, int last) {
    fprintf(f, "    \"%s\": %.4f%s\n", key, val, last ? "" : ",");
}
static void write_kv_int(FILE *f, const char *key, long val, int last) {
    fprintf(f, "    \"%s\": %ld%s\n", key, val, last ? "" : ",");
}

static int export_one(sqlite3 *db, FILE *out) {
    sqlite3_stmt *st;

    fprintf(out, "{\n");
    fprintf(out, "  \"version\": \"1.1\",\n");
    fprintf(out, "  \"generated_at\": %ld,\n", (long)time(NULL));
    fprintf(out, "  \"source\": \"问天 v1.1 (WenTian Weather Station)\",\n");
    fprintf(out, "  \"lat\": 25.0820, \"lon\": 102.9097, \"alt\": 2115,\n");
    fprintf(out, "  \"data\": {\n");

    /* 1. outdoor */
    fprintf(out, "    \"outdoor\": {\n");
    if (sqlite3_prepare_v2(db, "SELECT ts,temp,humid,pressure,wind_s,wind_d,weather,precip,cloud,uv FROM outdoor ORDER BY ts DESC LIMIT 1", -1, &st, NULL) == SQLITE_OK
        && sqlite3_step(st) == SQLITE_ROW) {
        write_kv_int(out, "ts", sqlite3_column_int64(st, 0), 0);
        write_kv_num(out, "temperature", sqlite3_column_double(st, 1), 0);
        write_kv_num(out, "humidity", sqlite3_column_double(st, 2), 0);
        write_kv_num(out, "pressure_msl", sqlite3_column_double(st, 3), 0);
        write_kv_num(out, "wind_speed", sqlite3_column_double(st, 4), 0);
        write_kv_num(out, "wind_dir", sqlite3_column_double(st, 5), 0);
        const unsigned char *wx = sqlite3_column_text(st, 6);
        write_kv(out, "weather", wx ? (const char*)wx : "", 0);
        write_kv_num(out, "precip", sqlite3_column_double(st, 7), 0);
        write_kv_num(out, "cloud_cover", sqlite3_column_double(st, 8), 0);
        write_kv_num(out, "uv", sqlite3_column_double(st, 9), 1);
    }
    sqlite3_finalize(st);
    fprintf(out, "    },\n");

    /* 2. air */
    fprintf(out, "    \"air_quality\": {\n");
    if (sqlite3_prepare_v2(db, "SELECT ts,pm25,pm10,aqi FROM air ORDER BY ts DESC LIMIT 1", -1, &st, NULL) == SQLITE_OK
        && sqlite3_step(st) == SQLITE_ROW) {
        write_kv_int(out, "ts", sqlite3_column_int64(st, 0), 0);
        write_kv_num(out, "pm25", sqlite3_column_double(st, 1), 0);
        write_kv_num(out, "pm10", sqlite3_column_double(st, 2), 0);
        write_kv_int(out, "aqi", sqlite3_column_int(st, 3), 1);
    }
    sqlite3_finalize(st);
    fprintf(out, "    },\n");

    /* 3. marine */
    fprintf(out, "    \"marine\": {\n");
    if (sqlite3_prepare_v2(db, "SELECT ts,wave_height,wave_period FROM marine ORDER BY ts DESC LIMIT 1", -1, &st, NULL) == SQLITE_OK
        && sqlite3_step(st) == SQLITE_ROW) {
        write_kv_int(out, "ts", sqlite3_column_int64(st, 0), 0);
        write_kv_num(out, "wave_height", sqlite3_column_double(st, 1), 0);
        write_kv_num(out, "wave_period", sqlite3_column_double(st, 2), 1);
    }
    sqlite3_finalize(st);
    fprintf(out, "    },\n");

    /* 4. flood */
    fprintf(out, "    \"flood\": {\n");
    if (sqlite3_prepare_v2(db, "SELECT ts,discharge FROM flood ORDER BY ts DESC LIMIT 1", -1, &st, NULL) == SQLITE_OK
        && sqlite3_step(st) == SQLITE_ROW) {
        write_kv_int(out, "ts", sqlite3_column_int64(st, 0), 0);
        write_kv_num(out, "discharge", sqlite3_column_double(st, 1), 1);
    }
    sqlite3_finalize(st);
    fprintf(out, "    },\n");

    /* 5. metar (取最新ZPPP主) */
    fprintf(out, "    \"metar_zppp\": {\n");
    if (sqlite3_prepare_v2(db, "SELECT ts,icao,temp,dewpoint,wind_dir,wind_speed,visib,altim FROM metar WHERE icao='ZPPP' ORDER BY ts DESC LIMIT 1", -1, &st, NULL) == SQLITE_OK
        && sqlite3_step(st) == SQLITE_ROW) {
        write_kv_int(out, "ts", sqlite3_column_int64(st, 0), 0);
        const unsigned char *icao = sqlite3_column_text(st, 1);
        write_kv(out, "icao", icao ? (const char*)icao : "ZPPP", 0);
        write_kv_num(out, "temp", sqlite3_column_double(st, 2), 0);
        write_kv_num(out, "dewpoint", sqlite3_column_double(st, 3), 0);
        write_kv_int(out, "wind_dir", sqlite3_column_int(st, 4), 0);
        write_kv_int(out, "wind_speed_kt", sqlite3_column_int(st, 5), 0);
        write_kv_int(out, "visib_m", sqlite3_column_int(st, 6), 0);
        write_kv_num(out, "altim_hpa", sqlite3_column_double(st, 7), 1);
    }
    sqlite3_finalize(st);
    fprintf(out, "    },\n");

    /* 6. NOAA SWPC Kp/F10.7/G-scale (问天独有!) */
    fprintf(out, "    \"swpc\": {\n");
    if (sqlite3_prepare_v2(db, "SELECT ts,kp,kp_index,kp_text FROM swpc_kp ORDER BY ts DESC LIMIT 1", -1, &st, NULL) == SQLITE_OK
        && sqlite3_step(st) == SQLITE_ROW) {
        write_kv_int(out, "ts", sqlite3_column_int64(st, 0), 0);
        write_kv_num(out, "kp", sqlite3_column_double(st, 1), 0);
        write_kv_int(out, "kp_index", sqlite3_column_int(st, 2), 0);
        const unsigned char *t = sqlite3_column_text(st, 3);
        write_kv(out, "kp_text", t ? (const char*)t : "", 1);
    }
    sqlite3_finalize(st);
    fprintf(out, "    },\n");
    fprintf(out, "    \"swpc_f107\": {\n");
    if (sqlite3_prepare_v2(db, "SELECT ts,flux_sfu,ninety_day_mean FROM swpc_f107 ORDER BY ts DESC LIMIT 1", -1, &st, NULL) == SQLITE_OK
        && sqlite3_step(st) == SQLITE_ROW) {
        write_kv_int(out, "ts", sqlite3_column_int64(st, 0), 0);
        write_kv_num(out, "flux_sfu", sqlite3_column_double(st, 1), 0);
        write_kv_num(out, "ninety_day_mean", sqlite3_column_double(st, 2), 1);
    }
    sqlite3_finalize(st);
    fprintf(out, "    },\n");
    fprintf(out, "    \"swpc_scale\": {\n");
    if (sqlite3_prepare_v2(db, "SELECT ts,g_scale,s_scale,r_scale FROM swpc_scale ORDER BY ts DESC LIMIT 1", -1, &st, NULL) == SQLITE_OK
        && sqlite3_step(st) == SQLITE_ROW) {
        write_kv_int(out, "ts", sqlite3_column_int64(st, 0), 0);
        write_kv_int(out, "g_scale", sqlite3_column_int(st, 1), 0);
        write_kv_int(out, "s_scale", sqlite3_column_int(st, 2), 0);
        write_kv_int(out, "r_scale", sqlite3_column_int(st, 3), 1);
    }
    sqlite3_finalize(st);
    fprintf(out, "    },\n");

    /* 7. sun */
    fprintf(out, "    \"sun\": {\n");
    if (sqlite3_prepare_v2(db, "SELECT ts,sunrise,sunset,day_length FROM sun ORDER BY ts DESC LIMIT 1", -1, &st, NULL) == SQLITE_OK
        && sqlite3_step(st) == SQLITE_ROW) {
        write_kv_int(out, "ts", sqlite3_column_int64(st, 0), 0);
        write_kv_int(out, "sunrise", sqlite3_column_int64(st, 1), 0);
        write_kv_int(out, "sunset", sqlite3_column_int64(st, 2), 0);
        write_kv_num(out, "day_length_sec", sqlite3_column_double(st, 3), 1);
    }
    sqlite3_finalize(st);
    fprintf(out, "    },\n");

    /* 8. iss */
    fprintf(out, "    \"iss\": {\n");
    if (sqlite3_prepare_v2(db, "SELECT ts,lat,lon FROM iss ORDER BY ts DESC LIMIT 1", -1, &st, NULL) == SQLITE_OK
        && sqlite3_step(st) == SQLITE_ROW) {
        write_kv_int(out, "ts", sqlite3_column_int64(st, 0), 0);
        write_kv_num(out, "lat", sqlite3_column_double(st, 1), 0);
        write_kv_num(out, "lon", sqlite3_column_double(st, 2), 1);
    }
    sqlite3_finalize(st);
    fprintf(out, "    },\n");

    /* 9. quake */
    fprintf(out, "    \"quake\": {\n");
    fprintf(out, "      \"note\": \"见 wentian.c 第10段输出\"\n");
    fprintf(out, "    },\n");

    /* 10. apod */
    fprintf(out, "    \"apod\": {\n");
    if (sqlite3_prepare_v2(db, "SELECT ts,date,title,url FROM apod ORDER BY ts DESC LIMIT 1", -1, &st, NULL) == SQLITE_OK
        && sqlite3_step(st) == SQLITE_ROW) {
        write_kv_int(out, "ts", sqlite3_column_int64(st, 0), 0);
        const unsigned char *d = sqlite3_column_text(st, 1);
        const unsigned char *t = sqlite3_column_text(st, 2);
        const unsigned char *u = sqlite3_column_text(st, 3);
        write_kv(out, "date", d ? (const char*)d : "", 0);
        write_kv(out, "title", t ? (const char*)t : "", 0);
        write_kv(out, "url", u ? (const char*)u : "", 1);
    }
    sqlite3_finalize(st);
    fprintf(out, "    },\n");

    /* 11. donki */
    fprintf(out, "    \"donki\": {\n");
    if (sqlite3_prepare_v2(db, "SELECT type,class,id,note,ts FROM donki ORDER BY ts DESC LIMIT 1", -1, &st, NULL) == SQLITE_OK
        && sqlite3_step(st) == SQLITE_ROW) {
        const unsigned char *t = sqlite3_column_text(st, 0);
        const unsigned char *c = sqlite3_column_text(st, 1);
        const unsigned char *i = sqlite3_column_text(st, 2);
        write_kv(out, "type", t ? (const char*)t : "", 0);
        write_kv(out, "class", c ? (const char*)c : "", 0);
        write_kv(out, "id", i ? (const char*)i : "", 0);
        write_kv_int(out, "ts", sqlite3_column_int64(st, 4), 1);
    }
    sqlite3_finalize(st);
    fprintf(out, "    },\n");

    /* 12. local_uno (主人机柜) */
    fprintf(out, "    \"local_uno\": {\n");
    if (sqlite3_prepare_v2(db, "SELECT ts,cabinet_t,cabinet_h,cabinet_p,sea_p,altitude,weather FROM local_uno ORDER BY ts DESC LIMIT 1", -1, &st, NULL) == SQLITE_OK
        && sqlite3_step(st) == SQLITE_ROW) {
        write_kv_int(out, "ts", sqlite3_column_int64(st, 0), 0);
        write_kv_num(out, "cabinet_temp", sqlite3_column_double(st, 1), 0);
        write_kv_num(out, "cabinet_humid", sqlite3_column_double(st, 2), 0);
        write_kv_num(out, "cabinet_pressure", sqlite3_column_double(st, 3), 0);
        write_kv_num(out, "sea_level_pressure", sqlite3_column_double(st, 4), 0);
        write_kv_num(out, "altitude", sqlite3_column_double(st, 5), 0);
        const unsigned char *wx = sqlite3_column_text(st, 6);
        write_kv(out, "weather", wx ? (const char*)wx : "", 1);
    }
    sqlite3_finalize(st);
    fprintf(out, "    },\n");

    /* 13. local_gnss */
    fprintf(out, "    \"local_gnss\": {\n");
    if (sqlite3_prepare_v2(db, "SELECT ts,lat,lon,alt,fix,gps_sats,bds_sats,pdop,hdop,vdop FROM local_gnss ORDER BY ts DESC LIMIT 1", -1, &st, NULL) == SQLITE_OK
        && sqlite3_step(st) == SQLITE_ROW) {
        write_kv_int(out, "ts", sqlite3_column_int64(st, 0), 0);
        write_kv_num(out, "lat", sqlite3_column_double(st, 1), 0);
        write_kv_num(out, "lon", sqlite3_column_double(st, 2), 0);
        write_kv_num(out, "alt", sqlite3_column_double(st, 3), 0);
        write_kv_int(out, "fix", sqlite3_column_int(st, 4), 0);
        write_kv_int(out, "gps_sats", sqlite3_column_int(st, 5), 0);
        write_kv_int(out, "bds_sats", sqlite3_column_int(st, 6), 0);
        write_kv_num(out, "pdop", sqlite3_column_double(st, 7), 0);
        write_kv_num(out, "hdop", sqlite3_column_double(st, 8), 0);
        write_kv_num(out, "vdop", sqlite3_column_double(st, 9), 1);
    }
    sqlite3_finalize(st);
    fprintf(out, "    },\n");

    /* 14. local_iono */
    fprintf(out, "    \"local_iono\": {\n");
    if (sqlite3_prepare_v2(db, "SELECT ts,s4_gps,s4_bds,gps_snr,bds_snr,pdop_avg,activity FROM local_iono ORDER BY ts DESC LIMIT 1", -1, &st, NULL) == SQLITE_OK
        && sqlite3_step(st) == SQLITE_ROW) {
        write_kv_int(out, "ts", sqlite3_column_int64(st, 0), 0);
        write_kv_num(out, "s4_gps", sqlite3_column_double(st, 1), 0);
        write_kv_num(out, "s4_bds", sqlite3_column_double(st, 2), 0);
        write_kv_num(out, "gps_snr", sqlite3_column_double(st, 3), 0);
        write_kv_num(out, "bds_snr", sqlite3_column_double(st, 4), 0);
        write_kv_num(out, "pdop_avg", sqlite3_column_double(st, 5), 0);
        const unsigned char *a = sqlite3_column_text(st, 6);
        write_kv(out, "activity", a ? (const char*)a : "", 1);
    }
    sqlite3_finalize(st);
    fprintf(out, "    },\n");

    /* 15. local_sdr */
    fprintf(out, "    \"local_sdr\": {\n");
    fprintf(out, "      \"note\": \"见 wentian.c 第15段输出 (3频段峰值)\"\n");
    fprintf(out, "    },\n");

    /* 16. 融合统计 */
    fprintf(out, "    \"fusion\": {\n");
    if (sqlite3_prepare_v2(db, "SELECT ts,fused,p_uno,p_om,p_metar,sigma FROM kf_pressure ORDER BY ts DESC LIMIT 1", -1, &st, NULL) == SQLITE_OK
        && sqlite3_step(st) == SQLITE_ROW) {
        write_kv_int(out, "ts", sqlite3_column_int64(st, 0), 0);
        write_kv_num(out, "fused_pressure", sqlite3_column_double(st, 1), 0);
        write_kv_num(out, "p_uno", sqlite3_column_double(st, 2), 0);
        write_kv_num(out, "p_om", sqlite3_column_double(st, 3), 0);
        write_kv_num(out, "p_metar", sqlite3_column_double(st, 4), 0);
        write_kv_num(out, "sigma", sqlite3_column_double(st, 5), 1);
    }
    sqlite3_finalize(st);
    fprintf(out, "    }\n");

    fprintf(out, "  },\n");
    fprintf(out, "  \"meta\": {\n");
    fprintf(out, "    \"data_source_count\": \"17 API + 4 主人硬件 + 1 Kalman = 22 维度\",\n");
    fprintf(out, "    \"db_path\": \"/root/data/wentian.db\",\n");
    fprintf(out, "    \"schema\": \"18 tables: outdoor/metar/air/marine/flood/apod/donki/sun/iss/quake/local_uno/local_gnss/local_iono/local_sdr + swpc_kp/swpc_f107/swpc_scale + kf_pressure\"\n");
    fprintf(out, "  }\n");
    fprintf(out, "}\n");
    return 0;
}

static int feeder_once(void) {
    /* 确保 fusion dir 存在 */
    mkdir("/root/data/fusion", 0755);

    sqlite3 *db;
    if (sqlite3_open("/root/data/wentian.db", &db) != SQLITE_OK) {
        fprintf(stderr, "[feeder] 无法打开 DB: %s\n", sqlite3_errmsg(db));
        return -1;
    }

    /* 写到临时文件再 rename, 避免读半截 */
    char tmp_path[256];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", OUT_PATH);
    FILE *out = fopen(tmp_path, "w");
    if (!out) {
        fprintf(stderr, "[feeder] 无法写 %s\n", tmp_path);
        sqlite3_close(db);
        return -1;
    }
    export_one(db, out);
    fclose(out);
    sqlite3_close(db);
    rename(tmp_path, OUT_PATH);
    printf("[feeder] wrote %s\n", OUT_PATH);
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        printf("用法: %s {once|daemon N}\n", argv[0]);
        printf("  once     - 导出一次 JSON\n");
        printf("  daemon N - 每N秒循环导出 (默认300)\n");
        return 1;
    }
    if (strcmp(argv[1], "once") == 0) return feeder_once();
    if (strcmp(argv[1], "daemon") == 0) {
        int sec = argc >= 3 ? atoi(argv[2]) : 300;
        printf("[feeder] daemon mode, period=%ds\n", sec);
        while (1) {
            feeder_once();
            sleep(sec);
        }
    }
    fprintf(stderr, "[feeder] 未知命令: %s\n", argv[1]);
    return 1;
}
