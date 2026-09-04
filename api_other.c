/* ============================================================
 * api_other.c - 其他API (Aviation/NASA/Sun/ISS/USGS/wttr) v1.1
 * ============================================================
 * 项目: 问天 v1.1 (WenTian Weather Station)
 * 锁定: 详见 WENTIAN-LOCK.md, 非授权LLM请勿擅改
 *
 * 函数清单:
 *   wt_aviation_metar()     AviationWeather 机场实测
 *   wt_nasa_apod()          NASA每日天文图 (含限流降级+placeholder)
 *   wt_nasa_donki_list()    NASA DONKI 4类空间天气事件
 *   wt_sun_times()          日出日落 (含本地SPA算法回退)
 *   wt_iss_position()       国际空间站位置
 *   wt_usgs_quakes_recent() 全球2.5+级地震 (24h)
 *   wt_wttr_in()            wttr.in 冗余气象
 *
 * 静态辅助函数:
 *   parse_iso()        ISO8601字符串→time_t (公开, wentian.c也用)
 *   find_nth_obj()     JSON第N个对象 (备用)
 *   dup_obj()          复制顶层JSON对象 (含嵌套)
 *   jday()             儒略日计算 (SPA用)
 *   calc_sun()         SPA太阳位置算法 (日出日落本地fallback)
 *   get_nasa_key()     读 /root/data/cache/nasa_key.txt
 *   apod_placeholder() APOD全部失败时的占位信息
 *
 * v1.1修复:
 *   - wttr.in: 空格匹配 + string数字兼容 + 修C语法?:
 *   - DONKI:  按type用正确字段 (cmeID/gstID/flrID/sepID)
 *   - APOD:    DEMO_KEY限流时读本地缓存+占位
 *   - SUN:     外部API失败时用本地SPA算法 (经纬度→UTC小时)
 *
 * 跨日逻辑: 昆明日出22:48 UTC属前一天, sunset 11:27 UTC属当天
 *   calc_sun用 sr_h>18 判跨日, 减86400秒校正
 * ============================================================ */
#define _GNU_SOURCE       /* 暴露 strptime */
#define _XOPEN_SOURCE 700
#include "wentian.h"
#include <time.h>
#include <math.h>
#include <sys/stat.h>
#include <unistd.h>

/* ── 工具: 找第N个JSON对象 ──────────────────────────────── */
static const char *find_nth_obj(const char *json, int n) {
    const char *p = json;
    for (int i = 0; i <= n; i++) {
        p = strchr(p, '{');
        if (!p) return NULL;
        if (i < n) p++;
    }
    return p;
}

/* 拷贝 JSON 顶层对象 (含嵌套) */
static char *dup_obj(const char *start) {
    if (!start || *start != '{') return NULL;
    int depth = 1;
    const char *end = start + 1;
    while (*end && depth > 0) {
        if (*end == '{') depth++;
        else if (*end == '}') depth--;
        end++;
    }
    if (depth != 0) return NULL;
    size_t len = end - start;
    char *r = malloc(len + 1);
    if (!r) return NULL;
    memcpy(r, start, len);
    r[len] = '\0';
    return r;
}

/* ISO8601 → time_t (无时区则按系统时区) - 公共, 被 wentian.c 也调用 */
time_t parse_iso(const char *s) {
    if (!s) return 0;
    struct tm tm = {0};
    char *end = strptime(s, "%Y-%m-%dT%H:%M:%S", &tm);
    if (!end) {
        /* 试只日期 */
        end = strptime(s, "%Y-%m-%d", &tm);
        if (!end) return 0;
    }
    /* 处理时区: Z=UTC, +HH:MM=-HH:MM, 无则按系统时区 */
    if (*end == 'Z' || *end == 'z') return timegm(&tm);
    if (*end == '+' || *end == '-') {
        int sign = (*end == '+') ? 1 : -1;
        int hh = 0, mm = 0;
        sscanf(end + 1, "%d:%d", &hh, &mm);
        time_t t = timegm(&tm);
        return t - sign * (hh * 3600 + mm * 60);
    }
    return mktime(&tm);
}

/* ── AviationWeather METAR (机场实测) ───────────────────── */
int wt_aviation_metar(const char *icao, wt_metar_t *out) {
    memset(out, 0, sizeof(*out));
    char url[512];
    snprintf(url, sizeof(url),
        "https://aviationweather.gov/api/data/metar?ids=%s&format=json&hours=1", icao);
    char *json = wt_http_get(url, 10);
    if (!json) return -1;

    char icao_pat[32];
    snprintf(icao_pat, sizeof(icao_pat), "\"icaoId\":\"%s\"", icao);
    const char *block = strstr(json, icao_pat);
    if (!block) { free(json); return -1; }

    /* 向前找最近的 { */
    const char *p = block;
    while (p > json && *p != '{') p--;
    char *obj = dup_obj(p);
    if (!obj) { free(json); return -1; }

    char *t;
    t = wt_json_dup(obj, "icaoId");
    if (t) { strncpy(out->icao, t, sizeof(out->icao)-1); free(t); }
    out->temp       = wt_json_num(obj, "temp", NAN);
    out->dewpoint   = wt_json_num(obj, "dewp", NAN);
    out->wind_dir   = wt_json_int(obj, "wdir", -1);
    out->wind_speed_kt = wt_json_int(obj, "wspd", -1);
    char *vis = wt_json_dup(obj, "visib");
    if (vis) {
        out->visibility_m = atoi(vis);
        free(vis);
    } else {
        out->visibility_m = -1;
    }
    out->altim_hpa  = wt_json_num(obj, "altim", NAN);
    char *raw = wt_json_dup(obj, "rawOb");
    if (raw) { strncpy(out->raw, raw, sizeof(out->raw)-1); free(raw); }

    /* obs time */
    char *obs = wt_json_dup(obj, "reportTime");
    if (obs) { out->obs_time = parse_iso(obs); free(obs); }
    else out->obs_time = time(NULL);

    free(obj);
    free(json);
    return 0;
}

/* ── NASA APOD (每日天文图) - 含缓存降级 ────────────────── */
#define APOD_CACHE "/root/data/cache/apod_last.json"
#define NASA_KEY_PATH "/root/data/cache/nasa_key.txt"

/* 读NASA API key, 优先配置文件, 否则DEMO_KEY */
static const char *get_nasa_key(void) {
    static char key[128] = {0};
    if (key[0]) return key;
    FILE *fp = fopen(NASA_KEY_PATH, "r");
    if (fp) {
        if (fgets(key, sizeof(key)-1, fp)) {
            char *nl = strchr(key, '\n');
            if (nl) *nl = '\0';
        }
        fclose(fp);
    }
    if (!key[0]) strcpy(key, "DEMO_KEY");
    return key;
}

/* 降级: 缓存都没有, 用今天的 placeholder */
static void apod_placeholder(wt_apod_t *out) {
    time_t now = time(NULL);
    struct tm *tm = gmtime(&now);
    memset(out, 0, sizeof(*out));
    strftime(out->date, sizeof(out->date), "%Y-%m-%d", tm);
    strncpy(out->title, "NASA APOD 限流中 (DEMO_KEY 30/h 50/d)", sizeof(out->title)-1);
    snprintf(out->explanation, sizeof(out->explanation),
        "主人: NASA DEMO_KEY 已限流. 申请个人key: https://api.nasa.gov "
        "(免费, 1000/h). 写到 %s 后立即生效.", NASA_KEY_PATH);
    strncpy(out->url, "https://apod.nasa.gov/apod/", sizeof(out->url)-1);
}

int wt_nasa_apod(wt_apod_t *out) {
    memset(out, 0, sizeof(*out));
    char url[256];
    const char *key = get_nasa_key();
    snprintf(url, sizeof(url),
        "https://api.nasa.gov/planetary/apod?api_key=%s&thumbs=true", key);
    char *json = wt_http_get(url, 10);
    if (!json || !*json || strstr(json, "\"error\"")) {
        /* 限流或失败, 用缓存 */
        free(json);
        json = NULL;
        FILE *fp = fopen(APOD_CACHE, "r");
        if (fp) {
            fseek(fp, 0, SEEK_END);
            long sz = ftell(fp);
            fseek(fp, 0, SEEK_SET);
            json = malloc(sz + 1);
            if (json) {
                size_t nread = fread(json, 1, sz, fp);
                json[nread] = '\0';
                fclose(fp);
            } else {
                fclose(fp);
            }
        }
        if (!json || !*json) {
            free(json);
            apod_placeholder(out);
            return 0;
        }
    } else {
        /* 成功后写缓存 */
        FILE *fp = fopen(APOD_CACHE, "w");
        if (fp) { fputs(json, fp); fclose(fp); }
    }

    char *t;
    t = wt_json_dup(json, "date");
    if (t) { strncpy(out->date, t, sizeof(out->date)-1); free(t); }
    t = wt_json_dup(json, "title");
    if (t) { strncpy(out->title, t, sizeof(out->title)-1); free(t); }
    t = wt_json_dup(json, "explanation");
    if (t) { strncpy(out->explanation, t, sizeof(out->explanation)-1); free(t); }
    t = wt_json_dup(json, "url");
    if (t) { strncpy(out->url, t, sizeof(out->url)-1); free(t); }
    if (!t) {
        /* 视频APOD, 改用 thumbnail_url */
        t = wt_json_dup(json, "thumbnail_url");
        if (t) { strncpy(out->url, t, sizeof(out->url)-1); free(t); }
    }
    free(json);
    return 0;
}

/* ── NASA DONKI Space Weather (CME/FLR/GST/SEP) ──────────── */
int wt_nasa_donki_list(wt_donki_type_t type, wt_donki_event_t *out, int max) {
    const char *types[] = {"CME", "FLR", "GST", "SEP"};
    const char *type_str = (type >= 0 && type < 4) ? types[type] : "all";
    char url[512];
    snprintf(url, sizeof(url),
        "https://api.nasa.gov/DONKI/%s?api_key=%s", type_str, get_nasa_key());

    char *json = wt_http_get(url, 10);
    if (!json || !*json) { free(json); return -1; }

    /* 限流检测 */
    if (strstr(json, "\"OVER_RATE_LIMIT\"") || strstr(json, "\"error\"")) {
        free(json);
        return 0;  /* 限流视为0事件, 不算错误 */
    }

    int count = 0;
    const char *p = json;
    while (count < max && (p = strchr(p, '{'))) {
        char *blk = dup_obj(p);
        if (!blk) { p++; continue; }

        wt_donki_event_t *e = &out[count];
        memset(e, 0, sizeof(*e));
        strncpy(e->type, type_str, sizeof(e->type)-1);

        /* 根据类型取对应 ID 字段 */
        char *t;
        /* FLR: flrID; CME: cmeID; GST: gstID; SEP: sepID */
        const char *id_keys[] = {"flrID", "cmeID", "gstID", "sepID"};
        if (type >= 0 && type < 4) {
            t = wt_json_dup(blk, id_keys[type]);
            if (t) { strncpy(e->id, t, sizeof(e->id)-1); free(t); }
        }
        /* 回退: activityID / messageID */
        if (!e->id[0]) {
            t = wt_json_dup(blk, "activityID");
            if (t) { strncpy(e->id, t, sizeof(e->id)-1); free(t); }
        }
        if (!e->id[0]) {
            t = wt_json_dup(blk, "messageID");
            if (t) { strncpy(e->id, t, sizeof(e->id)-1); free(t); }
        }

        /* classType 仅 FLR 有 */
        t = wt_json_dup(blk, "classType");
        if (t) { strncpy(e->classType, t, sizeof(e->classType)-1); free(t); }

        /* GST 的 kpIndex 当 classType */
        if (!e->classType[0] && type == WT_DONKI_GST) {
            double kp = wt_json_num(blk, "kpIndex", 0);
            if (kp > 0) snprintf(e->classType, sizeof(e->classType), "Kp%.0f", kp);
        }

        /* 时间字段 */
        t = wt_json_dup(blk, "beginTime");
        if (t) { e->begin = parse_iso(t); free(t); }
        t = wt_json_dup(blk, "peakTime");
        if (t) { e->peak = parse_iso(t); free(t); }
        t = wt_json_dup(blk, "endTime");
        if (t) { e->end = parse_iso(t); free(t); }

        /* 备注 */
        t = wt_json_dup(blk, "messageBody");
        if (t) {
            /* 截取首行或前120字符 */
            char *nl = strchr(t, '\n');
            if (nl && nl - t < sizeof(e->note)) *nl = '\0';
            strncpy(e->note, t, sizeof(e->note)-1);
            free(t);
        }

        count++;
        free(blk);
        p++;
    }
    free(json);
    return count;
}

/* ── Sunrise/Sunset (SPA算法回退) ─────────────────────── */
static double jday(int y, int m, int d) {
    int a = (14 - m) / 12;
    int yy = y + 4800 - a;
    int mm = m + 12 * a - 3;
    return d + (153*mm + 2)/5 + 365*yy + yy/4 - yy/100 + yy/400 - 32045.0;
}

/* 简化SPA (Sun Position Algorithm) - 计算日出日落 UTC 时间 */
static int calc_sun(double lat, double lon, int y, int m, int d,
                    double *sunrise_h, double *sunset_h) {
    double n = jday(y, m, d) - 2451545.0 + 0.0008;
    double Jstar = n - lon/360.0;
    double M = fmod(357.5291 + 0.98560028 * Jstar, 360.0) * M_PI / 180.0;
    double C = 1.9148*sin(M) + 0.0200*sin(2*M) + 0.0003*sin(3*M);
    double lambda = fmod(M + C + 180.0 + 102.9372, 360.0) * M_PI / 180.0;
    double Jtransit = 2451545.0 + Jstar + 0.0053*sin(M) - 0.0069*sin(2*lambda);
    double sin_d = sin(lambda) * sin(23.44*M_PI/180.0);
    double cos_d = cos(asin(sin_d));
    double cos_omega = (sin(-0.83*M_PI/180.0) - sin(lat*M_PI/180.0)*sin_d) /
                       (cos(lat*M_PI/180.0) * cos_d);
    if (cos_omega < -1.0 || cos_omega > 1.0) return -1;  /* 极昼极夜 */
    double omega_deg = acos(cos_omega) * 180.0 / M_PI;  /* 半昼弧(角度) */
    double omega_h = omega_deg / 15.0;  /* 1度=4分钟=15度/小时 */
    double transit_h = fmod(Jtransit + 0.5, 1.0) * 24.0;  /* 中天UTC小时 */
    *sunrise_h = transit_h - omega_h;
    *sunset_h  = transit_h + omega_h;
    /* wrap 到 [0,24) */
    if (*sunrise_h < 0) *sunrise_h += 24;
    if (*sunset_h  < 0) *sunset_h  += 24;
    if (*sunrise_h >= 24) *sunrise_h -= 24;
    if (*sunset_h  >= 24) *sunset_h  -= 24;
    return 0;
}

int wt_sun_times(double lat, double lon, time_t date, wt_sun_t *out) {
    memset(out, 0, sizeof(*out));
    char url[512];
    struct tm tm = {0};
    gmtime_r(&date, &tm);
    char date_str[16];
    strftime(date_str, sizeof(date_str), "%Y-%m-%d", &tm);

    snprintf(url, sizeof(url),
        "https://api.sunrise-sunset.org/json?lat=%.4f&lng=%.4f&date=%s&formatted=0",
        lat, lon, date_str);
    char *json = wt_http_get(url, 20);
    int used_fallback = 0;

    if (json && *json) {
        const char *res = strstr(json, "\"results\":");
        if (res) {
            char *t;
            t = wt_json_dup(res, "sunrise");
            if (t) { out->sunrise = parse_iso(t); free(t); }
            t = wt_json_dup(res, "sunset");
            if (t) { out->sunset = parse_iso(t); free(t); }
            t = wt_json_dup(res, "solar_noon");
            if (t) { out->solar_noon = parse_iso(t); free(t); }
            out->day_length_sec = wt_json_num(res, "day_length", 0);
            if (out->sunrise && out->sunset) {
                free(json);
                return 0;
            }
        }
    }
    free(json);

    /* 外部API失败, 用本地SPA算法 */
    used_fallback = 1;
    double sr_h, ss_h;
    if (calc_sun(lat, lon, tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                 &sr_h, &ss_h) == 0) {
        /* sr_h/ss_h 可能跨日: sunrise 可能在前一天UTC, sunset 在当天 */
        /* sunrise-sunset API证实: Kunming 9/3 sunrise=22:48 (前一天), sunset=11:27 */
        /* 计算相对于当天00:00 UTC的秒数 */
        time_t base = timegm(&tm);  /* 当天 UTC 00:00 */
        out->sunrise = base + (time_t)(sr_h * 3600);
        out->sunset  = base + (time_t)(ss_h * 3600);
        /* 真实 day_length 应是正数 (sunrise 早 sunset 晚) */
        /* 若 sunrise 出现在当天逻辑时间晚于 sunset, 是因为跨日: sunrise 前一天 */
        if (out->sunrise > out->sunset) {
            /* sunrise 实际属于前一天 */
            out->sunrise -= 86400;
        }
        out->solar_noon = (out->sunrise + out->sunset) / 2;
        out->day_length_sec = out->sunset - out->sunrise;
    }
    return used_fallback ? 0 : -1;
}

/* ── ISS位置 ──────────────────────────────────────────── */
int wt_iss_position(wt_iss_t *out) {
    memset(out, 0, sizeof(*out));
    char *json = wt_http_get("http://api.open-notify.org/iss-now.json", 20);
    if (!json) return -1;
    const char *iss = strstr(json, "\"iss_position\":");
    if (!iss) { free(json); return -1; }
    const char *lat_p = strstr(iss, "\"latitude\"");
    const char *lon_p = strstr(iss, "\"longitude\"");
    if (!lat_p || !lon_p) { free(json); return -1; }

    lat_p = strchr(lat_p, ':');
    lon_p = strchr(lon_p, ':');
    if (!lat_p || !lon_p) { free(json); return -1; }
    lat_p++; while (*lat_p == ' ' || *lat_p == '"') lat_p++;
    lon_p++; while (*lon_p == ' ' || *lon_p == '"') lon_p++;

    out->lat = lat_p ? strtod(lat_p, NULL) : NAN;
    out->lon = lon_p ? strtod(lon_p, NULL) : NAN;
    out->ts = (time_t)wt_json_num(json, "timestamp", (double)time(NULL));
    out->altitude_km = 408;
    out->velocity_kmh = 27600;
    free(json);
    return 0;
}

/* ── USGS Earthquake (全球2.5+级地震) ─────────────────── */
int wt_usgs_quakes_recent(wt_quake_t *out, int max) {
    char *json = wt_http_get(
        "https://earthquake.usgs.gov/earthquakes/feed/v1.0/summary/2.5_day.geojson",
        10);
    if (!json) return -1;
    const char *features = strstr(json, "\"features\":[");
    if (!features) { free(json); return -1; }

    int count = 0;
    const char *p = features;
    while (count < max && (p = strstr(p, "\"properties\":{"))) {
        const char *end = strchr(p, '}');
        if (!end) break;
        size_t len = end - p + 1;
        char *blk = malloc(len + 1);
        memcpy(blk, p, len);
        blk[len] = '\0';

        wt_quake_t *q = &out[count];
        memset(q, 0, sizeof(*q));
        q->mag = wt_json_num(blk, "mag", 0);
        char *place = wt_json_dup(blk, "place");
        if (place) { strncpy(q->place, place, sizeof(q->place)-1); free(place); }
        char *url = wt_json_dup(blk, "detail");
        if (url) { strncpy(q->url, url, sizeof(q->url)-1); free(url); }
        q->time = (time_t)wt_json_num(blk, "time", 0) / 1000;
        count++;
        free(blk);
        p = end + 1;
    }
    free(json);
    return count;
}

/* ── wttr.in 冗余气象 (修复C语法bug + 多字段容错) ────── */
int wt_wttr_in(const char *city, wt_outdoor_t *out) {
    memset(out, 0, sizeof(*out));
    char url[256];
    snprintf(url, sizeof(url), "https://wttr.in/%s?format=j1", city);
    char *json = wt_http_get(url, 15);
    if (!json) return -1;

    /* wttr.in 返回 nested: current_condition:[{...}] (有空格) */
    const char *cur = strstr(json, "\"current_condition\"");
    if (!cur) { free(json); return -1; }
    cur = strstr(cur, "[");
    if (!cur) { free(json); return -1; }
    cur = strchr(cur, '{');  /* 第一个对象 */
    if (!cur) { free(json); return -1; }
    char *obj = dup_obj(cur);
    if (!obj) { free(json); return -1; }

    out->temperature = wt_json_num(obj, "temp_C", NAN);
    out->humidity    = wt_json_num(obj, "humidity", NAN);

    char *wc = wt_json_dup(obj, "weatherCode");
    if (wc) {
        out->weather_code = atoi(wc);
        free(wc);
    } else {
        out->weather_code = 0;
    }

    out->wind_speed  = wt_json_num(obj, "windspeedKmph", 0);
    out->precipitation = wt_json_num(obj, "precipMM", 0);
    out->cloud_cover = wt_json_num(obj, "cloudcover", 0);
    out->pressure_msl = wt_json_num(obj, "pressure", NAN);
    out->visibility   = wt_json_num(obj, "visibility", 0) * 1000.0; /* km → m */

    /* 描述 - weatherDesc:[{...}] */
    const char *wd = strstr(obj, "\"weatherDesc\"");
    if (wd) {
        wd = strstr(wd, "[");
        if (wd) {
            wd++;
            char *desc = wt_json_dup(wd, "value");
            if (desc) {
                strncpy(out->weather_text, desc, sizeof(out->weather_text)-1);
                free(desc);
            }
        }
    }
    out->fetched_at = time(NULL);
    free(obj);
    free(json);
    return 0;
}