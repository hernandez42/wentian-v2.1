/* ============================================================
 * wentian.c - 问天气象站主程序 v1.1
 * ============================================================
 * 项目: 问天 v1.1 (WenTian Weather Station)
 * 所有者: 主人朱涛 BG8SBA, 昆明长水机场楼顶
 * 锁定: 详见 WENTIAN-LOCK.md, 非授权LLM请勿擅改
 *
 * 职责: 编排21+数据源采集, 打印报告, Daemon循环
 *
 * 用法:
 *   wentian once      抓一次所有API (≈30-40秒)
 *   wentian report    完整报告 (含采集+打印)
 *   wentian daemon N  后台循环, N秒周期 (默认300秒)
 *
 * 调用栈:
 *   main() → wt_db_init() + wt_local_db_init()
 *          → wentian_collect_all()  [核心编排]
 *            ├─ wt_openmeteo_*()      8个子API
 *            ├─ wt_aviation_metar()   主+备用机场
 *            ├─ wt_nasa_apod()        含限流降级
 *            ├─ wt_nasa_donki_list()  CME/FLR/GST/SEP
 *            ├─ wt_swpc_*()           NOAA Kp/F107/G-scale
 *            ├─ wt_sun_times()        +本地SPA回退
 *            ├─ wt_iss_position()
 *            ├─ wt_usgs_quakes_recent()
 *            ├─ wt_wttr_in()
 *            ├─ wt_local_uno_robust() + CSV回退
 *            ├─ wt_local_gnss() + nmea_read_gnss() 回退
 *            ├─ wt_local_iono()
 *            ├─ wt_local_sdr()
 *            └─ wt_kf_fuse_pressure() 三源Kalman融合
 *
 * 全局状态: g_kf_pressure / g_kf_temp (静态Kalman滤波器实例)
 *
 * 线程: 单线程同步采集, 每个HTTP超时独立, 但总耗时累加
 *
 * 信号: daemon模式可被SIGINT/SIGTERM打断 (未实现优雅退出)
 * ============================================================ */
#include "wentian.h"
#include <unistd.h>
#include <termios.h>
#include <fcntl.h>
#include <math.h>
#include <sys/stat.h>

/* ── 全局Kalman滤波器状态 ───────────────────────────────── */
static wt_kf_filter_t g_kf_pressure;
static wt_kf_filter_t g_kf_temp;
static int g_kf_inited = 0;

static void kf_init_once(void) {
    if (g_kf_inited) return;
    wt_kf_init(&g_kf_pressure, 1013.25, 0.01, 0.5);   /* 气压: 低q低r */
    wt_kf_init(&g_kf_temp, 20.0, 0.05, 0.3);           /* 温度 */
    g_kf_inited = 1;
}

/* ── 报告模式: GNSS/电离层空时重试+离线回退 ────────────── */
static int wt_local_uno_robust(wt_uno_t *out) {
    if (wt_local_uno(out) == 0) return 0;
    /* 回退: 直接读 ano_weather 最新行 */
    FILE *fp = fopen("/root/data/uno_weather.csv", "r");
    if (!fp) return -1;
    char line[1024], last[1024] = {0};
    while (fgets(line, sizeof(line), fp)) {
        if (strstr(line, "UNO_v2.0_bridge")) strcpy(last, line);
    }
    fclose(fp);
    if (!last[0]) return -1;
    /* 解析 CSV: ts|UNO_v2.0_bridge|t|h|p|pa|alt|wx */
    memset(out, 0, sizeof(*out));
    char *p = last;
    char *ts_end = strchr(p, '|');
    if (!ts_end) return -1;
    *ts_end = '\0';
    out->ts = parse_iso(p);
    p = ts_end + 1;
    /* 跳过 source */
    p = strchr(p, '|'); if (!p) return -1; p++;
    out->cabinet_temp = strtod(p, &p); if (*p == '|') p++;
    out->cabinet_humid = strtod(p, &p); if (*p == '|') p++;
    out->cabinet_pressure = strtod(p, &p); if (*p == '|') p++;
    out->sea_level_pressure = strtod(p, &p); if (*p == '|') p++;
    out->altitude = strtod(p, &p);
    return 0;
}

/* ── NMEA 串口直接读取 ATGM336H ───────────────────────── */
static int nmea_read_gnss(wt_gnss_t *out) {
    memset(out, 0, sizeof(*out));
    /* 找USB串口 - 优先 by-id */
    const char *dev = "/dev/ttyUSB2";
    if (access(dev, R_OK) != 0) dev = "/dev/ttyUSB0";
    int fd = open(dev, O_RDONLY | O_NONBLOCK);
    if (fd < 0) return -1;
    struct termios tio;
    tcgetattr(fd, &tio);
    cfmakeraw(&tio);
    cfsetspeed(&tio, B9600);
    tcsetattr(fd, TCSANOW, &tio);

    char buf[4096] = {0};
    int n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return -1;
    buf[n] = '\0';

    /* 解析最新 RMC/GGA/GSV/GLL - 简化为 lat/lon/alt/sats */
    out->ts = time(NULL);
    out->fix = 1;
    char *line = strtok(buf, "\r\n");
    char *latest = NULL;
    while (line) { latest = line; line = strtok(NULL, "\r\n"); }
    if (!latest) return -1;

    if (strstr(latest, "$GNGGA") || strstr(latest, "$GPGGA")) {
        /* $xxGGA,time,lat,N,lon,E,fix,sats,hdop,alt,M,... */
        char lat_s[16] = {0}, lat_ns[4] = {0}, lon_s[16] = {0}, lon_ew[4] = {0};
        int fix_q = 0, sats = 0;
        double hdop = 0, alt = 0;
        if (sscanf(latest, "%*[^,],%*[^,],%15[^,],%3[^,],%15[^,],%3[^,],%d,%d,%lf,%lf",
            lat_s, lat_ns, lon_s, lon_ew, &fix_q, &sats, &hdop, &alt) >= 8) {
            double lat = atof(lat_s);
            double lon = atof(lon_s);
            /* ddmm.mmmm → dd.dddd */
            int lat_d = (int)(lat / 100);
            double lat_m = lat - lat_d * 100;
            out->lat = lat_d + lat_m / 60.0;
            if (lat_ns[0] == 'S') out->lat = -out->lat;
            int lon_d = (int)(lon / 100);
            double lon_m = lon - lon_d * 100;
            out->lon = lon_d + lon_m / 60.0;
            if (lon_ew[0] == 'W') out->lon = -out->lon;
            out->fix = fix_q;
            out->total_sats = sats;
            out->hdop = hdop;
            out->alt = alt;
            out->altitude_msl = alt;
        }
    }
    return out->fix > 0 ? 0 : -1;
}

/* ── 收集所有数据源 ────────────────────────────────────── */
int wentian_collect_all(void) {
    int ok = 0, fail = 0;
    kf_init_once();

    printf("\n━━━ 1. Open-Meteo 室外气象权威 ━━━\n");
    wt_outdoor_t outdoor = {0};
    if (wt_openmeteo_current(&outdoor) == 0) {
        printf("  ✅ T=%.1f°C H=%.0f%% P=%.1fhPa 风%.0fkm/h 天气:%s\n",
            outdoor.temperature, outdoor.humidity, outdoor.pressure_msl,
            outdoor.wind_speed, outdoor.weather_text);
        wt_db_save_outdoor(&outdoor);
        ok++;
    } else fail++;

    printf("\n━━━ 2. Open-Meteo 空气质量 ━━━\n");
    double pm25 = 0, pm10 = 0;
    int aqi = 0;
    if (wt_openmeteo_air_quality(&pm25, &pm10, &aqi) == 0) {
        printf("  ✅ PM2.5=%.1fμg PM10=%.1fμg AQI=%d\n", pm25, pm10, aqi);
        wt_db_save_air(pm25, pm10, aqi);
        ok++;
    } else fail++;

    printf("\n━━━ 3. Open-Meteo 海洋气象 (南海) ━━━\n");
    double wave_h = 0, wave_p = 0;
    if (wt_openmeteo_marine(&wave_h, &wave_p) == 0) {
        printf("  ✅ 浪高=%.1fm 周期=%.1fs\n", wave_h, wave_p);
        wt_db_save_marine(wave_h, wave_p);
        ok++;
    } else fail++;

    printf("\n━━━ 4. Open-Meteo 洪水预警 ━━━\n");
    double discharge = NAN;
    if (wt_openmeteo_flood(&discharge, NULL) == 0) {
        if (isnan(discharge)) printf("  ⚠ 无可用数据 (内陆/无河流)\n");
        else printf("  ✅ 河流流量=%.2fm³/s\n", discharge);
        wt_db_save_flood(isnan(discharge) ? 0 : discharge, 0);
        ok++;
    } else fail++;

    printf("\n━━━ 5. AviationWeather 机场实测 (ZPPP长水) ━━━\n");
    wt_metar_t metar = {0};
    if (wt_aviation_metar("ZPPP", &metar) == 0) {
        printf("  ✅ T=%.0f°C 风%d°/%dkt 气压=%.0fhPa\n",
            metar.temp, metar.wind_dir, metar.wind_speed_kt, metar.altim_hpa);
        printf("     RAW: %s\n", metar.raw);
        wt_db_save_metar(&metar);
        ok++;
        /* 备用机场 */
        printf("\n━━━ 5b. 备用机场 METAR (ZUUK/ZPLJ/ZPPP) ━━━\n");
        const char *alt_icaos[] = {"ZUUK", "ZPLJ", "ZUTF"};
        for (int i = 0; i < 3; i++) {
            wt_metar_t alt = {0};
            if (wt_aviation_metar(alt_icaos[i], &alt) == 0) {
                printf("  ✅ %s T=%.0f°C 风%d°/%dkt 气压=%.0fhPa\n",
                    alt.icao, alt.temp, alt.wind_dir, alt.wind_speed_kt, alt.altim_hpa);
                wt_db_save_metar(&alt);
            }
        }
    } else fail++;

    printf("\n━━━ 6. NASA APOD 每日天文图 ━━━\n");
    wt_apod_t apod = {0};
    if (wt_nasa_apod(&apod) == 0) {
        if (strstr(apod.title, "限流中")) {
            printf("  ⚠ %s\n", apod.title);
            printf("     %s\n", apod.explanation);
        } else {
            printf("  ✅ 日期:%s\n  标题:%.80s\n  URL:%s\n",
                apod.date, apod.title, apod.url);
            wt_db_save_apod(&apod);
        }
        ok++;
    } else fail++;

    printf("\n━━━ 7. NASA DONKI Space Weather ━━━\n");
    wt_donki_event_t events[20];
    int n_flr = wt_nasa_donki_list(WT_DONKI_FLR, events, 20);
    int donki_any = 0;
    if (n_flr > 0) {
        printf("  ✅ 太阳耀斑 (FLR): %d 个近期事件\n", n_flr);
        for (int i = 0; i < n_flr && i < 5; i++) {
            printf("     [%s] %s - %.80s\n",
                events[i].classType, events[i].id, events[i].note);
            wt_db_save_donki(&events[i]);
        }
        donki_any = 1;
    } else printf("  ⚠ FLR 无数据 (NASA DEMO_KEY限流或当日无事件)\n");

    int n_cme = wt_nasa_donki_list(WT_DONKI_CME, events, 20);
    if (n_cme > 0) {
        printf("  ✅ 日冕物质抛射 (CME): %d 个近期事件\n", n_cme);
        for (int i = 0; i < n_cme && i < 3; i++) {
            printf("     [%s] %s\n", events[i].classType[0] ? events[i].classType : "CME", events[i].id);
            wt_db_save_donki(&events[i]);
        }
        donki_any = 1;
    }

    int n_gst = wt_nasa_donki_list(WT_DONKI_GST, events, 20);
    if (n_gst > 0) {
        printf("  ✅ 地磁风暴 (GST): %d 个近期事件\n", n_gst);
        for (int i = 0; i < n_gst && i < 3; i++) {
            printf("     [%s] %s\n", events[i].classType[0] ? events[i].classType : "GST", events[i].id);
            wt_db_save_donki(&events[i]);
        }
        donki_any = 1;
    }
    if (donki_any) ok++;  /* 至少有数据就算成功 */

    printf("\n━━━ 7b. NOAA SWPC 太空天气 (Kp/F10.7/G-scale) ━━━\n");
    wt_kp_t kp = {0};
    if (wt_swpc_kp(&kp) == 0) {
        printf("  ✅ Kp=%.1f (指数=%d 文本=%s) 站点=%d A-running=%.0f\n",
            kp.kp, kp.kp_index, kp.kp_text, kp.station_count, kp.a_running);
        wt_db_save_kp(&kp);
        ok++;
    } else fail++;
    wt_f107_t f107 = {0};
    if (wt_swpc_f107(&f107) == 0) {
        printf("  ✅ F10.7=%.1f sfu 90日均值=%.1f\n", f107.flux_sfu, f107.ninety_day_mean);
        wt_db_save_f107(&f107);
        ok++;
    } else fail++;
    wt_swpc_scale_t scale = {0};
    if (wt_swpc_scales(&scale) == 0) {
        printf("  ✅ NOAA尺度: G%d S%d R%d (日期=%s)\n",
            scale.g_scale, scale.s_scale, scale.r_scale, scale.date);
        wt_db_save_scale(&scale);
        ok++;
    } else fail++;

    printf("\n━━━ 8. Sunrise/Sunset 日出日落 ━━━\n");
    wt_sun_t sun = {0};
    if (wt_sun_times(WENTIAN_LAT, WENTIAN_LON, time(NULL), &sun) == 0) {
        struct tm tm;
        gmtime_r(&sun.sunrise, &tm);
        printf("  ✅ 日出:%02d:%02d:%02d UTC\n", tm.tm_hour, tm.tm_min, tm.tm_sec);
        gmtime_r(&sun.sunset, &tm);
        printf("     日落:%02d:%02d:%02d UTC 昼长=%.1fh\n",
            tm.tm_hour, tm.tm_min, tm.tm_sec, sun.day_length_sec / 3600.0);
        wt_db_save_sun(&sun, WENTIAN_LAT, WENTIAN_LON);
        ok++;
    } else fail++;

    printf("\n━━━ 9. ISS 国际空间站 ━━━\n");
    wt_iss_t iss = {0};
    if (wt_iss_position(&iss) == 0) {
        printf("  ✅ ISS 位置: 纬度%.2f° 经度%.2f° 高度%.0fkm\n",
            iss.lat, iss.lon, iss.altitude_km);
        wt_db_save_iss(&iss);
        ok++;
    } else fail++;

    printf("\n━━━ 10. USGS 全球地震 (2.5+级, 24h) ━━━\n");
    wt_quake_t quakes[20];
    int n_quakes = wt_usgs_quakes_recent(quakes, 20);
    if (n_quakes > 0) {
        printf("  ✅ 共 %d 次地震 (M2.5+, 24h)\n", n_quakes);
        int nearby = 0;
        for (int i = 0; i < n_quakes; i++) {
            double dlat = quakes[i].lat - WENTIAN_LAT;
            dlat = dlat < 0 ? -dlat : dlat;
            double dlon = quakes[i].lon - WENTIAN_LON;
            dlon = dlon < 0 ? -dlon : dlon;
            if (dlat < 30 && dlon < 30 && quakes[i].mag >= 4.0) {
                printf("     ⚠ M%.1f %s (主人附近)\n", quakes[i].mag, quakes[i].place);
                wt_db_save_quake(&quakes[i]);
                nearby++;
            } else if (quakes[i].mag >= 6.0) {
                printf("     🌍 M%.1f %s\n", quakes[i].mag, quakes[i].place);
                wt_db_save_quake(&quakes[i]);
            }
        }
        if (nearby == 0) printf("     主人附近无4级+地震\n");
        ok++;
    } else fail++;

    printf("\n━━━ 11. wttr.in 冗余气象 ━━━\n");
    wt_outdoor_t wttr = {0};
    if (wt_wttr_in("Kunming", &wttr) == 0) {
        printf("  ✅ Kunming T=%.0f°C H=%.0f%% P=%.0fhPa 风%.0fkm/h 天气:%.30s\n",
            wttr.temperature, wttr.humidity, wttr.pressure_msl,
            wttr.wind_speed, wttr.weather_text);
        ok++;
    } else fail++;

    /* ═══════════════════════════════════════════════════════
     * 🎯 主人: "别因噎废食, 1+1大于x"
     * 自家硬件接入: UNO+北斗+SDR+电离层
     * ═══════════════════════════════════════════════════════ */
    printf("\n━━━ 12. 主人UNO机柜 (本地温/湿/压, 证明机柜恒温) ━━━\n");
    wt_uno_t uno = {0};
    if (wt_local_uno_robust(&uno) == 0) {
        time_t now = time(NULL);
        int age = (int)(now - uno.ts);
        printf("  ✅ 机柜温=%.1f°C 湿=%.0f%% 压=%.1fhPa 海平面压=%.1fhPa 天气=%s (数据%d秒前)\n",
            uno.cabinet_temp, uno.cabinet_humid, uno.cabinet_pressure,
            uno.sea_level_pressure, uno.weather, age);
        printf("     ⚠ 主人: 机柜温度只能证明机柜恒温, 不能用于室外预测!\n");
        wt_local_save_uno(&uno);
        ok++;
    } else fail++;

    printf("\n━━━ 13. 主人ATGM336H GPS+北斗 (主人海拔校准) ━━━\n");
    wt_gnss_t gnss = {0};
    int gnss_ok = 0;
    /* 优先SQLite, 空数据时NMEA回退 */
    if (wt_local_gnss(&gnss) == 0 && gnss.fix > 0 && gnss.lat != 0.0) {
        gnss_ok = 1;
    } else if (nmea_read_gnss(&gnss) == 0) {
        gnss_ok = 1;
        printf("     ⚠ SQLite空, 改用NMEA串口实时\n");
    }
    if (gnss_ok) {
        printf("  ✅ 位置=%.4f°N,%.4f°E 海拔=%.0fm fix=%d 卫星: GPS=%d颗 北斗=%d颗\n",
            gnss.lat, gnss.lon, gnss.alt, gnss.fix, gnss.gps_sats, gnss.bds_sats);
        printf("     DOP: PDOP=%.1f HDOP=%.1f VDOP=%.1f\n", gnss.pdop, gnss.hdop, gnss.vdop);
        wt_local_save_gnss(&gnss);
        ok++;
    } else fail++;

    printf("\n━━━ 14. 主人GNSS电离层 (S4+Klobuchar) ━━━\n");
    wt_iono_t iono = {0};
    if (wt_local_iono(&iono) == 0 && iono.s4_gps > 0) {
        printf("  ✅ S4: GPS=%.3f 北斗=%.3f (活动: %s)\n",
            iono.s4_gps, iono.s4_bds, iono.activity);
        printf("     SNR: GPS=%.1fdB 北斗=%.1fdB PDOP=%.1f\n",
            iono.gps_snr_avg, iono.bds_snr_avg, iono.pdop_avg);
        printf("     Klobuchar斜向延迟=%.1fns 倾斜因子=%.2f\n",
            iono.klob_slant_delay, iono.klob_slant_factor);
        wt_local_save_iono(&iono);
        ok++;
    } else fail++;

    printf("\n━━━ 15. 主人V4 SDR + LNA + 全向天线 (扫频峰值) ━━━\n");
    wt_sdr_t sdr_peaks[3] = {0};
    int n_sdr = 0;
    if (wt_local_sdr(sdr_peaks, 3, &n_sdr) == 0 && n_sdr > 0) {
        for (int i = 0; i < n_sdr; i++) {
            printf("  ✅ [%s] 峰值 %.3fMHz @ %.1fdBm (SNR=%.1fdB, 噪底=%.1fdBm)\n",
                sdr_peaks[i].band, sdr_peaks[i].peak_freq_mhz,
                sdr_peaks[i].peak_dbm, sdr_peaks[i].peak_snr,
                sdr_peaks[i].noise_floor_dbm);
            wt_local_save_sdr(&sdr_peaks[i]);
        }
        ok++;
    } else {
        printf("  ⚠ SDR扫频文件未找到\n");
    }

    /* ═══ 16. Kalman气压融合 (UNO+OM+METAR) ═══════════ */
    printf("\n━━━ 16. Kalman气压融合 (UNO+OM+METAR) ━━━\n");
    if (uno.cabinet_pressure > 0 && outdoor.pressure_msl > 0 && metar.altim_hpa > 0) {
        double p_uno_sealevel = uno.sea_level_pressure > 0 ? uno.sea_level_pressure : uno.cabinet_pressure;
        double fused = wt_kf_fuse_pressure(&g_kf_pressure,
            p_uno_sealevel, outdoor.pressure_msl, metar.altim_hpa);
        printf("  ✅ 融合气压=%.2fhPa (UNO海平面=%.1f OM=%.1f METAR=%.0f, σ=%.2f)\n",
            fused, p_uno_sealevel, outdoor.pressure_msl, metar.altim_hpa,
            kf1d_uncertainty(&g_kf_pressure.kf));
        wt_db_save_fused_pressure(fused, p_uno_sealevel, outdoor.pressure_msl,
            metar.altim_hpa, kf1d_uncertainty(&g_kf_pressure.kf));
        ok++;
    } else {
        printf("  ⚠ 气压数据不全, 跳过Kalman融合\n");
    }

    /* ═══ 17. GNSS PWV实时反演(C) ═════════════════════ */
    printf("\n━━━ 17. GNSS PWV实时反演(C实现) ━━━\n");
    if (wt_pwv_run() == 0) {
        ok++;
    } else {
        fail++;
    }

    /* ═══ 18. 短临雷暴Nowcasting ═══════════════════════ */
    printf("\n━━━ 18. 短临雷暴Nowcasting (0-30min) ━━━\n");
    if (wt_nowcast_run() == 0) {
        ok++;
    } else {
        fail++;
    }

    /* ═══ 19. 软件雷达三路相干 ═════════════════════════ */
    printf("\n━━━ 19. 软件雷达三路相干(SDR+GNSS+UNO) ━━━\n");
    if (wt_radar_correlate_run() == 0) {
        ok++;
    } else {
        fail++;
    }

    /* ═══ 20. GNSS电离层闪烁监测(C实现) ════════════════ */
    printf("\n━━━ 20. GNSS电离层闪烁监测(C实现) ━━━\n");
    if (wt_gnss_ion_run() == 0) {
        ok++;
    } else {
        fail++;
    }

    /* ═══ 21. 多源融合预测 (原Python multi_source_predict.py) ══ */
    printf("\n━━━ 21. 多源融合预测 (8通道投票) ━━━\n");
    if (wt_predict_run() == 0) {
        ok++;
    } else {
        fail++;
    }

    /* ═══ 22. 自进化自愈自完善 (api_evolve.c) ═══════════ */
    printf("\n━━━ 22. 自进化自愈自完善引擎 ━━━\n");
    /* 失败不算, 因为数据不足是常态 */
    wt_evo_run();

    /* ═══ 23. 多源融合 S4 (api_multisrc.c) ═══════════════ */
    printf("\n━━━ 23. 多源融合 S4 引擎 (5源加权) ━━━\n");
    /* 失败不算, 因为某些数据源可能离线 */
    wt_multisrc_run();

    /* ═══ 24. 开源专业数据集成 (api_open_data.c) ═══════ */
    printf("\n━━━ 24. 开源专业数据集成 (4 API) ━━━\n");
    /* 失败不算, 因为某些 API 可能暂时离线 */
    wt_open_data_run();

    printf("\n━━━ 总结 ━━━\n  成功: %d  失败: %d  (24模块)\n", ok, fail);
    printf("  17开放API + 4本地硬件 + 1Kalman融合 + 1PWV反演 + 1电离层 + 1相干雷达 + 1多源预测 + 1自进化 + 1多源融合 + 4开源API = 32 维度\n\n");
    return 0;
}

/* ── 打印报告 ──────────────────────────────────────────── */
int wentian_print_report(void) {
    printf("══════════════════════════════════════\n");
    printf("  问天气象站 v%s 报告\n", WENTIAN_VERSION);
    printf("  位置: %.4f°N, %.4f°E 海拔%.0fm\n",
        WENTIAN_LAT, WENTIAN_LON, (double)WENTIAN_ALT);
    printf("  时间: %s", ctime(&(time_t){time(NULL)}));
    printf("══════════════════════════════════════\n");
    wentian_collect_all();
    return 0;
}

/* ── Daemon 模式 ──────────────────────────────────────── */
int wentian_daemon(int interval_sec) {
    printf("═══ 问天 Daemon 模式 周期=%ds ═══\n", interval_sec);
    if (wt_db_init(WENTIAN_DB) != 0) return -1;
    while (1) {
        wentian_collect_all();
        sleep(interval_sec);
    }
    return 0;
}

/* ── main ──────────────────────────────────────────────── */
int main(int argc, char **argv) {
    if (argc < 2) {
        printf("用法: %s {once|report|daemon}\n", argv[0]);
        printf("  once     - 抓一次所有API\n");
        printf("  report   - 完整报告\n");
        printf("  daemon N - 后台循环 (N秒周期, 默认300)\n");
        return 1;
    }

    mkdir("/root/data/cache", 0755);  /* APOD缓存目录 */

    if (wt_db_init(WENTIAN_DB) != 0) {
        fprintf(stderr, "DB初始化失败, 继续运行\n");
    }
    wt_local_db_init(WENTIAN_DB);
    wt_nowcast_db_init(WENTIAN_DB);    /* Nowcast表初始化 */
    wt_radar_db_init(WENTIAN_DB);      /* 相干雷达表初始化 */
    wt_pwv_db_init(WENTIAN_DB);        /* PWV反演表初始化 */
    wt_gnss_ion_db_init(WENTIAN_DB);   /* 电离层表初始化 */

    if (strcmp(argv[1], "once") == 0) {
        return wentian_collect_all();
    }
    if (strcmp(argv[1], "report") == 0) {
        return wentian_print_report();
    }
    if (strcmp(argv[1], "daemon") == 0) {
        int sec = (argc >= 3) ? atoi(argv[2]) : 300;
        return wentian_daemon(sec);
    }
    fprintf(stderr, "未知命令: %s\n", argv[1]);
    return 1;
}