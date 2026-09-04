/* ============================================================
 * api_openmeteo.c - Open-Meteo 一族 (8子API) v1.1
 * ============================================================
 * 项目: 问天 v1.1 (WenTian Weather Station)
 * 锁定: 详见 WENTIAN-LOCK.md, 非授权LLM请勿擅改
 *
 * Open-Meteo API族 (所有端点免key免注册):
 *   - api.open-meteo.com         Forecast / Climate
 *   - air-quality-api.open-meteo.com
 *   - marine-api.open-meteo.com
 *   - flood-api.open-meteo.com
 *   - geocoding-api.open-meteo.com
 *
 * 函数:
 *   wt_openmeteo_current()      室外权威 (温度/湿度/气压/天气码...)
 *   wt_openmeteo_air_quality()  PM2.5/PM10/欧洲AQI
 *   wt_openmeteo_marine()       浪高/周期 (查南海22°N 115°E)
 *   wt_openmeteo_flood()        GloFAS 河流流量 (修过daily解析)
 *   wt_openmeteo_climate()      CMIP6 30年气候
 *   wt_openmeteo_geocode()      地名→经纬度
 *
 * WMO代码转换: wmo_text() 静态函数, code→中文
 *
 * v1.1修复: flood改读 daily.river_discharge[0] (原版误读顶层, 得nan)
 * ============================================================ */
#include "wentian.h"
#include <math.h>

/* WMO code → 中文 */
static const char *wmo_text(int code) {
    switch (code) {
        case 0: return "晴";
        case 1: case 2: return "多云";
        case 3: return "阴";
        case 45: case 48: return "雾";
        case 51: case 53: case 55: return "毛毛雨";
        case 61: case 63: case 65: return "雨";
        case 71: case 73: case 75: return "雪";
        case 80: case 81: case 82: return "阵雨";
        case 95: return "雷暴";
        case 96: case 99: return "雷暴冰雹";
        default: return "未知";
    }
}

/* Open-Meteo Forecast - 当前室外气象权威 */
int wt_openmeteo_current(wt_outdoor_t *out) {
    memset(out, 0, sizeof(*out));
    char url[1024];
    snprintf(url, sizeof(url),
        "https://api.open-meteo.com/v1/forecast?"
        "latitude=%.4f&longitude=%.4f"
        "&current=temperature_2m,relative_humidity_2m,surface_pressure,"
        "weather_code,wind_speed_10m,wind_direction_10m,precipitation,"
        "cloud_cover,uv_index,visibility"
        "&timezone=Asia/Shanghai",
        WENTIAN_LAT, WENTIAN_LON);

    char *json = wt_http_get(url, 10);
    if (!json) return -1;

    /* 解析 current 字段 */
    const char *cur = strstr(json, "\"current\":");
    if (!cur) { free(json); return -1; }

    out->temperature  = wt_json_num(cur, "temperature_2m", NAN);
    out->humidity     = wt_json_num(cur, "relative_humidity_2m", NAN);
    out->pressure_msl = wt_json_num(cur, "surface_pressure", NAN);
    out->weather_code = wt_json_int(cur, "weather_code", 0);
    out->wind_speed   = wt_json_num(cur, "wind_speed_10m", 0);
    out->wind_dir     = wt_json_num(cur, "wind_direction_10m", 0);
    out->precipitation= wt_json_num(cur, "precipitation", 0);
    out->cloud_cover  = wt_json_num(cur, "cloud_cover", 0);
    out->uv_index     = wt_json_num(cur, "uv_index", 0);
    out->visibility   = wt_json_num(cur, "visibility", 0);
    out->fetched_at   = time(NULL);

    const char *txt = wmo_text(out->weather_code);
    strncpy(out->weather_text, txt, sizeof(out->weather_text) - 1);

    free(json);
    return 0;
}

/* Open-Meteo Air Quality */
int wt_openmeteo_air_quality(double *pm25, double *pm10, int *aqi) {
    char url[512];
    snprintf(url, sizeof(url),
        "https://air-quality-api.open-meteo.com/v1/air-quality?"
        "latitude=%.4f&longitude=%.4f&current=pm10,pm2_5,european_aqi",
        WENTIAN_LAT, WENTIAN_LON);
    char *json = wt_http_get(url, 10);
    if (!json) return -1;
    const char *cur = strstr(json, "\"current\":");
    if (!cur) { free(json); return -1; }
    if (pm25) *pm25 = wt_json_num(cur, "pm2_5", NAN);
    if (pm10) *pm10 = wt_json_num(cur, "pm10", NAN);
    if (aqi)  *aqi  = wt_json_int(cur, "european_aqi", -1);
    free(json);
    return 0;
}

/* Open-Meteo Marine (主人最近海洋约1000km, 但仍然全球可查) */
int wt_openmeteo_marine(double *wave_height, double *wave_period) {
    /* 查南海北部 (海南岛附近, ~22°N 115°E, 离昆明约1000km) */
    char url[512];
    snprintf(url, sizeof(url),
        "https://marine-api.open-meteo.com/v1/marine?"
        "latitude=22&longitude=115"
        "&current=wave_height,wave_period");
    char *json = wt_http_get(url, 10);
    if (!json) return -1;
    const char *cur = strstr(json, "\"current\":");
    if (!cur) { free(json); return -1; }
    if (wave_height) *wave_height = wt_json_num(cur, "wave_height", NAN);
    if (wave_period) *wave_period = wt_json_num(cur, "wave_period", NAN);
    free(json);
    return 0;
}

/* Open-Meteo Flood (GloFAS全球洪水预警) - daily 数组第0天 */
int wt_openmeteo_flood(double *discharge, double *level) {
    (void)level;  /* Open-Meteo flood API暂无level参数 */
    char url[512];
    snprintf(url, sizeof(url),
        "https://flood-api.open-meteo.com/v1/flood?"
        "latitude=%.4f&longitude=%.4f&daily=river_discharge",
        WENTIAN_LAT, WENTIAN_LON);
    char *json = wt_http_get(url, 10);
    if (!json) return -1;
    /* daily.river_discharge:[0.76,0.61,...] - 取第一个元素 */
    const char *daily = strstr(json, "\"daily\":{");
    double v = NAN;
    if (daily) {
        const char *rd = strstr(daily, "\"river_discharge\":[");
        if (rd) {
            const char *p = strchr(rd, '[');
            if (p) {
                p++;
                while (*p == ' ' || *p == '\n') p++;
                char *endp;
                v = strtod(p, &endp);
                if (endp == p) v = NAN;
            }
        }
    }
    if (discharge) *discharge = v;
    free(json);
    return 0;
}

/* Open-Meteo Climate (CMIP6 30年气候预测) */
int wt_openmeteo_climate(int month, int day, double *temp_mean) {
    (void)month; (void)day;  /* 当前用start_date/end_date范围 */
    char url[1024];
    snprintf(url, sizeof(url),
        "https://climate-api.open-meteo.com/v1/climate?"
        "latitude=%.4f&longitude=%.4f"
        "&start_date=2025-09-01&end_date=2026-09-01"
        "&models=CMCC_CM2_VHR4&daily=temperature_2m_mean",
        WENTIAN_LAT, WENTIAN_LON);
    char *json = wt_http_get(url, 10);
    if (!json) return -1;
    /* 取月平均 */
    if (temp_mean) *temp_mean = wt_json_num(json, "temperature_2m_mean", NAN);
    free(json);
    return 0;
}

/* Open-Meteo Geocoding */
int wt_openmeteo_geocode(const char *name, double *lat, double *lon) {
    char url[512];
    snprintf(url, sizeof(url),
        "https://geocoding-api.open-meteo.com/v1/search?name=%s&count=1",
        name);
    char *json = wt_http_get(url, 10);
    if (!json) return -1;
    const char *res = strstr(json, "\"results\":[");
    if (!res) { free(json); return -1; }
    if (lat) *lat = wt_json_num(res, "latitude", NAN);
    if (lon) *lon = wt_json_num(res, "longitude", NAN);
    free(json);
    return 0;
}