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
/* 把任意DB文本安全写进JSON字符串:
 *  - 转义引号/反斜杠/制表/换行
 *  - 丢掉控制字符
 *  - 丢弃残缺UTF-8字节序列 (DB里note字段被C端按字节截断过, 直接输出会让
 *    Python json.load 抛 UnicodeDecodeError, 整条推送挂掉)
 * out 需至少 strlen(val)*2+1 字节 */
static void json_escape(const char *val, char *out, size_t outsz) {
    size_t o = 0;
#define PUTCH(c) do { if (o + 1 < outsz) out[o++] = (char)(c); } while (0)
#define PUTS(s)  do { for (const char *q_ = (s); *q_; q_++) PUTCH(*q_); } while (0)
    const unsigned char *p = (const unsigned char *)(val ? val : "");
    while (*p) {
        unsigned char c = *p;
        if (c == '"')  { PUTS("\\\"");  p++; continue; }
        if (c == '\\') { PUTS("\\\\");  p++; continue; }
        if (c == '\n') { PUTS("\\n");   p++; continue; }
        if (c == '\r') { p++;           continue; }
        if (c == '\t') { PUTS("\\t");   p++; continue; }
        if (c < 0x20)  { p++;           continue; }   /* 控制字符丢弃 */

        if (c < 0x80)  { PUTCH(c); p++; continue; }

        /* 多字节UTF-8: 先数前导字节, 再确认后续 continuation 全齐 */
        int need = 0;
        if ((c & 0xE0) == 0xC0) need = 1;
        else if ((c & 0xF0) == 0xE0) need = 2;
        else if ((c & 0xF8) == 0xF0) need = 3;
        else { p++; continue; }                        /* 非法前导字节, 丢 */

        int ok = 1;
        for (int i = 1; i <= need; i++)
            if ((p[i] & 0xC0) != 0x80) { ok = 0; break; }
        if (!ok) { p++; continue; }                    /* 被截断的字符, 整串丢 */

        for (int i = 0; i <= need; i++) PUTCH(p[i]);
        p += need + 1;
    }
    out[o] = '\0';
#undef PUTCH
#undef PUTS
}

static void write_kv_esc(FILE *f, const char *key, const char *val, int last) {
    char buf[2048];
    json_escape(val, buf, sizeof(buf));
    fprintf(f, "    \"%s\": \"%s\"%s\n", key, buf, last ? "" : ",");
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
    fprintf(out, "    },\n");

    /* ── v2.0 新增: 问天v2.3模块结论导出 ── */

    /* 17. 短临Nowcasting (模块17/18) */
    fprintf(out, "    \"nowcast\": {\n");
    if (sqlite3_prepare_v2(db, "SELECT ts,warning_level,forecast,score,thunder_score,squall_score,stationary_score,wind_shear_score,pwv_current,precip_1h_mm,alert_msg FROM nowcast ORDER BY ts DESC LIMIT 1", -1, &st, NULL) == SQLITE_OK
        && sqlite3_step(st) == SQLITE_ROW) {
        write_kv_int(out, "ts", sqlite3_column_int64(st, 0), 0);
        const unsigned char *wl = sqlite3_column_text(st, 1);
        const unsigned char *fc = sqlite3_column_text(st, 2);
        write_kv_esc(out, "warning_level", (const char*)wl, 0);
        write_kv_esc(out, "forecast", (const char*)fc, 0);
        write_kv_int(out, "score", sqlite3_column_int(st, 3), 0);
        write_kv_int(out, "thunder_score", sqlite3_column_int(st, 4), 0);
        write_kv_int(out, "squall_score", sqlite3_column_int(st, 5), 0);
        write_kv_int(out, "stationary_score", sqlite3_column_int(st, 6), 0);
        write_kv_int(out, "wind_shear_score", sqlite3_column_int(st, 7), 0);
        write_kv_num(out, "pwv_current", sqlite3_column_double(st, 8), 0);
        write_kv_num(out, "precip_1h_mm", sqlite3_column_double(st, 9), 0);
        const unsigned char *am = sqlite3_column_text(st, 10);
        write_kv_esc(out, "alert_msg", (const char*)am, 1);
    }
    sqlite3_finalize(st);
    fprintf(out, "    },\n");

    /* 18. 多源融合预测结论 (模块21) */
    fprintf(out, "    \"multi_source\": {\n");
    if (sqlite3_prepare_v2(db, "SELECT ts,T_current,P_current,T_1h,P_1h,final_weather,zambretti,openmeteo_3h,metar_now,storm_score,level,alerts,s4_max FROM multi_source_forecast ORDER BY ts DESC LIMIT 1", -1, &st, NULL) == SQLITE_OK
        && sqlite3_step(st) == SQLITE_ROW) {
        write_kv_int(out, "ts", sqlite3_column_int64(st, 0), 0);
        write_kv_num(out, "t_current", sqlite3_column_double(st, 1), 0);
        write_kv_num(out, "p_current", sqlite3_column_double(st, 2), 0);
        write_kv_num(out, "t_1h", sqlite3_column_double(st, 3), 0);
        write_kv_num(out, "p_1h", sqlite3_column_double(st, 4), 0);
        const unsigned char *fw = sqlite3_column_text(st, 5);
        const unsigned char *zb = sqlite3_column_text(st, 6);
        const unsigned char *om3 = sqlite3_column_text(st, 7);
        const unsigned char *mn = sqlite3_column_text(st, 8);
        write_kv_esc(out, "final_weather", (const char*)fw, 0);
        write_kv_esc(out, "zambretti", (const char*)zb, 0);
        write_kv_esc(out, "openmeteo_3h", (const char*)om3, 0);
        write_kv_esc(out, "metar_now", (const char*)mn, 0);
        write_kv_int(out, "storm_score", sqlite3_column_int(st, 9), 0);
        const unsigned char *lv = sqlite3_column_text(st, 10);
        write_kv_esc(out, "level", (const char*)lv, 0);
        const unsigned char *al = sqlite3_column_text(st, 11);
        write_kv_esc(out, "alerts", (const char*)al, 0);
        write_kv_num(out, "s4_max", sqlite3_column_double(st, 12), 1);
    }
    sqlite3_finalize(st);
    fprintf(out, "    },\n");

    /* 19. 自进化评分 (模块22) — 每个predictor最新一行 */
    fprintf(out, "    \"evolution\": [\n");
    if (sqlite3_prepare_v2(db, "SELECT ts,predictor,mae_temp,mae_press,total_score,sample_n FROM evolution WHERE rowid IN (SELECT MAX(rowid) FROM evolution GROUP BY predictor) ORDER BY predictor", -1, &st, NULL) == SQLITE_OK) {
        int first = 1;
        while (sqlite3_step(st) == SQLITE_ROW) {
            if (!first) fprintf(out, ",\n");
            first = 0;
            char esc_note[2048];
            json_escape((const char*)sqlite3_column_text(st, 6), esc_note, sizeof(esc_note));
            const unsigned char *pd = sqlite3_column_text(st, 1);
            fprintf(out, "      {\"predictor\": \"%s\", \"mae_temp\": %.2f, \"mae_press\": %.2f, \"score\": %d, \"samples\": %d, \"note\": \"%s\"}",
                    pd ? (const char*)pd : "?",
                    sqlite3_column_double(st, 2), sqlite3_column_double(st, 3),
                    sqlite3_column_int(st, 4), sqlite3_column_int(st, 5),
                    esc_note);
        }
        fprintf(out, "\n    ],\n");
    }
    sqlite3_finalize(st);

    /* 20. 多源融合S4引擎 (模块23) */
    fprintf(out, "    \"multisrc_s4\": {\n");
    if (sqlite3_prepare_v2(db, "SELECT ts,s4_sdr,s4_openmeteo,s4_uno,fused_s4,confidence,level,used_n FROM multisrc_s4 ORDER BY ts DESC LIMIT 1", -1, &st, NULL) == SQLITE_OK
        && sqlite3_step(st) == SQLITE_ROW) {
        write_kv_int(out, "ts", sqlite3_column_int64(st, 0), 0);
        write_kv_num(out, "s4_sdr", sqlite3_column_double(st, 1), 0);
        write_kv_num(out, "s4_openmeteo", sqlite3_column_double(st, 2), 0);
        write_kv_num(out, "s4_uno", sqlite3_column_double(st, 3), 0);
        write_kv_num(out, "fused_s4", sqlite3_column_double(st, 4), 0);
        write_kv_num(out, "confidence", sqlite3_column_double(st, 5), 0);
        const unsigned char *lv = sqlite3_column_text(st, 6);
        write_kv_esc(out, "level", (const char*)lv, 0);
        write_kv_int(out, "used_n", sqlite3_column_int(st, 7), 1);
    }
    sqlite3_finalize(st);
    fprintf(out, "    },\n");

    /* 21. 开源专业数据 (模块24) */
    fprintf(out, "    \"external\": {\n");
    if (sqlite3_prepare_v2(db, "SELECT ts,noaa_kp,noaa_f107,metno_temp,metno_pressure,wttr_temp,sources_n FROM external_data ORDER BY ts DESC LIMIT 1", -1, &st, NULL) == SQLITE_OK
        && sqlite3_step(st) == SQLITE_ROW) {
        write_kv_int(out, "ts", sqlite3_column_int64(st, 0), 0);
        write_kv_num(out, "noaa_kp", sqlite3_column_double(st, 1), 0);
        write_kv_num(out, "noaa_f107", sqlite3_column_double(st, 2), 0);
        write_kv_num(out, "metno_temp", sqlite3_column_double(st, 3), 0);
        write_kv_num(out, "metno_pressure", sqlite3_column_double(st, 4), 0);
        write_kv_num(out, "wttr_temp", sqlite3_column_double(st, 5), 0);
        write_kv_int(out, "sources_n", sqlite3_column_int(st, 6), 1);
    }
    sqlite3_finalize(st);
    fprintf(out, "    },\n");

    /* 22. PWV反演 (模块17) */
    fprintf(out, "    \"pwv\": {\n");
    if (sqlite3_prepare_v2(db, "SELECT ts,pwv_mm,delta_pwv,storm_score FROM local_pwv ORDER BY ts DESC LIMIT 1", -1, &st, NULL) == SQLITE_OK
        && sqlite3_step(st) == SQLITE_ROW) {
        write_kv_int(out, "ts", sqlite3_column_int64(st, 0), 0);
        write_kv_num(out, "pwv_mm", sqlite3_column_double(st, 1), 0);
        write_kv_num(out, "delta_pwv", sqlite3_column_double(st, 2), 0);
        write_kv_int(out, "storm_score", sqlite3_column_int(st, 3), 1);
    }
    sqlite3_finalize(st);
    fprintf(out, "    },\n");

    /* 23. 钦天监增强 (imperial_enhancement 表) */
    fprintf(out, "    \"imperial_enhancement\": {\n");
    if (sqlite3_prepare_v2(db, "SELECT ts,solar_term,wuxing_quadrant,hexagram,precip_adjust_factor,press_adjust_factor,alert_threshold,system_stable FROM imperial_enhancement ORDER BY ts DESC LIMIT 1", -1, &st, NULL) == SQLITE_OK
        && sqlite3_step(st) == SQLITE_ROW) {
        write_kv_int(out, "ts", sqlite3_column_int64(st, 0), 0);
        write_kv_esc(out, "solar_term", (const char*)sqlite3_column_text(st, 1), 0);
        write_kv_esc(out, "wuxing_quadrant", (const char*)sqlite3_column_text(st, 2), 0);
        write_kv_esc(out, "hexagram", (const char*)sqlite3_column_text(st, 3), 0);
        write_kv_num(out, "precip_adjust", sqlite3_column_double(st, 4), 0);
        write_kv_num(out, "press_adjust", sqlite3_column_double(st, 5), 0);
        write_kv_num(out, "alert_threshold", sqlite3_column_double(st, 6), 0);
        write_kv_int(out, "system_stable", sqlite3_column_int(st, 7), 1);
    }
    sqlite3_finalize(st);
    fprintf(out, "    },\n");

    /* 24. ROTI (读取 JSON 文件) */
    fprintf(out, "    \"roti\": {\n");
    FILE *rf = fopen("/root/data/fusion/roti.json", "r");
    if (rf) {
        char rbuf[1024];
        size_t rn = fread(rbuf, 1, sizeof(rbuf)-1, rf);
        fclose(rf);
        if (rn > 0) {
            rbuf[rn] = '\0';
            /* 提取 roti 和 status 字段 */
            const char *r_r = strstr(rbuf, "\"roti\":");
            const char *r_s = strstr(rbuf, "\"status\":");
            if (r_r) {
                double roti_val = 0;
                sscanf(r_r + 8, "%lf", &roti_val);
                write_kv_num(out, "roti", roti_val, 0);
            }
            if (r_s) {
                char status[32] = {0};
                sscanf(r_s + 9, "\"%31[^\"]\"", status);
                write_kv_esc(out, "status", status, 0);
            }
            write_kv_int(out, "samples", 0, 1);  /* last=true */
        }
    } else {
        write_kv_num(out, "roti", 0, 0);
        write_kv_esc(out, "status", "数据不足", 1);
    }
    /* roti不是最后一块 */
    fprintf(out, "    },\n");

    /* 25. 星象 (astral.json) */
    fprintf(out, "    \"astral\": {\n");
    FILE *af = fopen("/root/data/fusion/astral.json", "r");
    char anote[256] = {0};
    strcpy(anote, "日月星曜各安其位");
    if (af) {
        char abuf[4096];
        size_t an = fread(abuf, 1, sizeof(abuf)-1, af);
        fclose(af);
        if (an > 0) {
            abuf[an] = '\0';
            const char *cs = strstr(abuf, "\"celestial_assessment\":");
            if (cs) {
                char ca[64] = {0};
                sscanf(cs, "\"celestial_assessment\": \"%63[^\"]\"", ca);
                write_kv_esc(out, "celestial_assessment", ca, 0);
            }
            const char *as = strstr(abuf, "\"anomalies\":");
            if (as) {
                const char *dt = strstr(as, "\"detail\":\"");
                if (dt) {
                    sscanf(dt, "\"detail\": \"%255[^\"]\"", anote);
                }
            }
        }
    }
    /* 确保至少有一个detail写入 */
    write_kv_esc(out, "detail", anote, 0);
    write_kv_esc(out, "version", "星象 v1.0", 1);
    fprintf(out, "    },\n");  /* astral后面还有weathernext */

    /* 26. WeatherNext 2 (weathernext_forecast.json) */
    fprintf(out, "    \"weathernext\": {\n");
    FILE *wf = fopen("/root/data/fusion/weathernext_forecast.json", "r");
    if (wf) {
        char wbuf[2048];
        size_t wn = fread(wbuf, 1, sizeof(wbuf)-1, wf);
        fclose(wf);
        if (wn > 0) {
            wbuf[wn] = '\0';
            /* 摘要daily温度范围 */
            const char *sd = strstr(wbuf, "\"summary\":");
            if (sd) {
                char sum_buf[1024] = {0};
                /* 取前3天摘要 */
                const char *d1 = strstr(sd, "2026");
                if (d1) {
                    snprintf(sum_buf, sizeof(sum_buf), "%.60s...", d1);
                    write_kv_esc(out, "summary_preview", sum_buf, 0);
                }
            }
            write_kv_int(out, "hours", wn > 100 ? 360 : 0, 0);
        }
    }
    write_kv_esc(out, "model", "google_weathernext2_ensemble", 1);
    fprintf(out, "    }\n");  /* weathernext最后一块, 无逗号 */

    fprintf(out, "  },\n");  /* data 块结束 */
    fprintf(out, "  \"meta\": {\n");
    fprintf(out, "    \"data_source_count\": \"17 API + 4 硬件 + 1 Kalman + 1 PWV + 1 电离层 + 1 相干 + 1 预测 + 1 自进化 + 1 多源S4 + 4 开源 + 1 自愈 + 1 TEC + 1 钦天监 + 1 ROTI + 1 星象 + 1 WeatherNext = 38 维度\",\n");
    fprintf(out, "    \"db_path\": \"/root/data/wentian.db\",\n");
    fprintf(out, "    \"schema\": \"18 tables + imperial_enhancement + roti.json\"\n");
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
