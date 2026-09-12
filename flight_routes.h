#ifndef FLIGHT_ROUTES_H
#define FLIGHT_ROUTES_H

/* ============================================================
 * flight_routes.h - 寻龙尺航线数据库
 * ============================================================
 * ZPPP昆明长水始发/到达 20条主要国内航线
 * 航班号前2-3位字母映射到航线
 *
 * 距离来源: Great-circle distance 近似计算
 * ============================================================ */

#include <string.h>
#include <ctype.h>

/* ── 航线记录 ────────────────────────────────────────────── */
typedef struct {
    char flight_prefix[8];   /* 航班号前缀(如 "MU", "CA417", "3U") */
    const char *dep_icao;    /* 起飞机场ICAO */
    const char *arr_icao;    /* 到达机场ICAO */
    const char *dep_name;    /* 起飞机场名称 */
    const char *arr_name;    /* 到达机场名称 */
    int distance_nm;         /* 航线距离(海里) */
} flight_route_t;

/* ── 20条ZPPP主要航线 ──────────────────────────────────── */
/* 航班号前缀: 以中国民航真实航司代码为参考 */
static const flight_route_t flight_routes[] = {
    /* 航司   前缀  ICAO出发  ICAO到达  出发名   到达名      距离NM */
    {"MU",     "ZPPP", "ZUUU", "昆明长水", "成都双流",    300},
    {"CA",     "ZPPP", "ZBAA", "昆明长水", "北京首都",   1100},
    {"CZ",     "ZPPP", "ZGGG", "昆明长水", "广州白云",    550},
    {"3U",     "ZPPP", "ZGSZ", "昆明长水", "深圳宝安",    580},
    {"MF",     "ZPPP", "ZSSS", "昆明长水", "上海虹桥",   1000},
    {"8L",     "ZPPP", "ZSHC", "昆明长水", "杭州萧山",    950},
    {"ZH",     "ZPPP", "ZSNJ", "昆明长水", "南京禄口",    900},
    {"HO",     "ZPPP", "ZSPD", "昆明长水", "上海浦东",   1020},
    {"FM",     "ZPPP", "ZUCK", "昆明长水", "重庆江北",    330},
    {"GJ",     "ZPPP", "ZGHA", "昆明长水", "长沙黄花",    520},
    {"SC",     "ZPPP", "ZLXY", "昆明长水", "西安咸阳",    640},
    {"HU",     "ZPPP", "ZJHK", "昆明长水", "海口美兰",    500},
    {"KY",     "ZPPP", "ZGSD", "昆明长水", "珠海金湾",    540},
    {"JD",     "ZPPP", "ZPDQ", "昆明长水", "香格里拉",    200},
    {"TV",     "ZPPP", "ZPJH", "昆明长水", "西双版纳",    220},
    {"DR",     "ZPPP", "ZPLJ", "昆明长水", "丽江三义",    160},
    {"PN",     "ZPPP", "ZHCC", "昆明长水", "郑州新郑",    820},
    {"GS",     "ZPPP", "ZWWW", "昆明长水", "乌鲁木齐",   1350},
    {"DZ",     "ZPPP", "ZYTX", "昆明长水", "沈阳桃仙",   1250},
    {"QW",     "ZPPP", "ZYHB", "昆明长水", "哈尔滨太平", 1450},
};

#define FLIGHT_ROUTES_COUNT  (sizeof(flight_routes) / sizeof(flight_routes[0]))

/* ── 通过航班号查找航线 ───────────────────────────────── */
/* 匹配规则: 取航班号前2-3个字母字符(忽略数字), 与flight_prefix对比
 * 返回: 匹配的航线指针, NULL=未找到 */
__attribute__((unused))
static const flight_route_t *find_route(const char *flight_no) {
    if (!flight_no || !*flight_no) return NULL;

    /* 提取航班号的前字母部分 */
    char prefix[16];
    int pi = 0;
    const char *p = flight_no;
    /* 提取前导字母+数字混合的航司代码 (如 "3U", "8L" 等两位码)
     * 规则: 最多取前4个字母数字混合字符作为前缀,
     * 遇到纯数字且非连续字母序列的第一个数字时停止 */
    int has_alpha = 0;
    while (*p && pi < (int)sizeof(prefix) - 1) {
        if (isalpha((unsigned char)*p)) {
            prefix[pi++] = (char)toupper((unsigned char)*p);
            has_alpha = 1;
        } else if (*p >= '0' && *p <= '9') {
            /* 允许数字作为前缀的一部分(如 "3U" 中的 '3'),
             * 但前提是: ①还没遇到字母 ②或前缀很短(≤2) */
            if (pi < 2 || !has_alpha) {
                prefix[pi++] = *p;
            } else {
                break;  /* 数字前已有字母且前缀够长 → 视为航班号数字部分 */
            }
        } else {
            /* 跳过连字符、空格等 */
            p++;
            continue;
        }
        p++;
    }
    prefix[pi] = '\0';

    if (pi == 0) return NULL;

    /* 在路由表中匹配 */
    for (int i = 0; i < (int)FLIGHT_ROUTES_COUNT; i++) {
        if (strcmp(prefix, flight_routes[i].flight_prefix) == 0) {
            return &flight_routes[i];
        }
    }

    return NULL;
}

#endif /* FLIGHT_ROUTES_H */
