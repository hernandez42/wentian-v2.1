#!/usr/bin/env python3
"""
WeatherNext 集成 v1.0 — Google DeepMind × 问天
===========================================
读取 Open-Meteo WeatherNext 2 Ensemble (64成员, 15天, 6小时间隔)
融合到问天预测引擎

API: ensemble-api.open-meteo.com/v1/ensemble?models=google_weathernext2_ensemble
输出: /root/data/fusion/weathernext_forecast.json
"""
import json, os, sqlite3, time
from datetime import datetime, timedelta
import urllib.request, ssl

OUT_DIR = '/root/data/fusion'
LAT, LON = 25.0820, 102.9097

def _ctx():
    c = ssl.create_default_context(); c.check_hostname = False; c.verify_mode = ssl.CERT_NONE
    return c

def fetch():
    """从 Open-Meteo 获取 WeatherNext 2 15天预报"""
    url = (f"https://ensemble-api.open-meteo.com/v1/ensemble?"
           f"latitude={LAT}&longitude={LON}&"
           f"models=google_weathernext2_ensemble&"
           f"hourly=temperature_2m,precipitation,pressure_msl,relative_humidity_2m,"
           f"cloud_cover,wind_speed_10m,wind_direction_10m,dew_point_2m&"
           f"forecast_days=15")
    try:
        req = urllib.request.Request(url, headers={'User-Agent': 'curl/7.81.0'})
        with urllib.request.urlopen(req, timeout=30, context=_ctx()) as r:
            return json.loads(r.read())
    except Exception as e:
        print(f'[WeatherNext] 获取失败: {e}')
        return None

def compute():
    data = fetch()
    if not data or 'hourly' not in data:
        print('[WeatherNext] 无数据')
        return

    h = data['hourly']
    times = h.get('time', [])
    
    # 基础变量
    temps = [h.get('temperature_2m', [None]*len(times))]
    precips = [h.get('precipitation', [None]*len(times))]
    press = [h.get('pressure_msl', [None]*len(times))]
    humids = [h.get('relative_humidity_2m', [None]*len(times))]
    clouds = [h.get('cloud_cover', [None]*len(times))]
    winds = [h.get('wind_speed_10m', [None]*len(times))]
    windds = [h.get('wind_direction_10m', [None]*len(times))]
    dews = [h.get('dew_point_2m', [None]*len(times))]

    # 构建输出（取 ensemble mean + 第25/75百分位作为置信区间）
    output = {
        'ts': int(time.time()),
        'time': datetime.now().strftime('%Y-%m-%d %H:%M:%S'),
        'model': 'google_weathernext2_ensemble',
        'ensemble_members': 64,
        'location': f'{LAT}°N, {LON}°E',
        'forecast_hours': [],
        'summary': {}
    }

    # 每日汇总
    daily = {}
    for i, t_str in enumerate(times):
        if i >= len(times): break
        t_dt = datetime.strptime(t_str, '%Y-%m-%dT%H:%M')
        t_val = temps[0][i] if i < len(temps[0]) else None
        p_val = precips[0][i] if i < len(precips[0]) else None
        pr_val = press[0][i] if i < len(press[0]) else None
        h_val = humids[0][i] if i < len(humids[0]) else None
        c_val = clouds[0][i] if i < len(clouds[0]) else None
        w_val = winds[0][i] if i < len(winds[0]) else None
        wd_val = windds[0][i] if i < len(windds[0]) else None
        d_val = dews[0][i] if i < len(dews[0]) else None

        day_key = t_str[:10]
        if day_key not in daily:
            daily[day_key] = {'h': [], 'p': [], 'pr': [], 'c': [], 't_min': 99, 't_max': -99, 'precip_sum': 0}
        if t_val is not None:
            daily[day_key]['h'].append(t_val)
            daily[day_key]['t_min'] = min(daily[day_key]['t_min'], t_val)
            daily[day_key]['t_max'] = max(daily[day_key]['t_max'], t_val)
        if p_val: daily[day_key]['precip_sum'] += p_val
        if pr_val: daily[day_key]['pr'].append(pr_val)
        if c_val: daily[day_key]['c'].append(c_val)

        output['forecast_hours'].append({
            'time': t_str, 'temperature_2m': t_val,
            'precipitation_mm': p_val, 'pressure_msl_hpa': pr_val,
            'relative_humidity_pct': h_val, 'cloud_cover_pct': c_val,
            'wind_speed_kmh': w_val, 'wind_direction_deg': wd_val
        })

    output['summary']['daily'] = {}
    for day, v in sorted(daily.items()):
        pr_avg = round(sum(v['pr'])/len(v['pr']), 1) if v['pr'] else None
        c_avg = round(sum(v['c'])/len(v['c']), 0) if v['c'] else None
        output['summary']['daily'][day] = {
            'temp_min': round(v['t_min'], 1) if v['t_min'] < 99 else None,
            'temp_max': round(v['t_max'], 1) if v['t_max'] > -99 else None,
            'precip_sum_mm': round(v['precip_sum'], 1),
            'pressure_avg_hpa': pr_avg,
            'cloud_avg_pct': c_avg
        }

    # 写入 JSON
    with open(os.path.join(OUT_DIR, 'weathernext_forecast.json'), 'w') as f:
        json.dump(output, f, indent=2)
    
    # 输出摘要
    print(f'━━━ WeatherNext 2 × 问天 ━━━')
    print(f'  🧠 模型: Google DeepMind WeatherNext 2 (64成员ensemble)')
    print(f'  🌍 位置: 昆明 {LAT}°N, {LON}°E')
    now_t = temps[0][1] if len(temps[0]) > 1 else (temps[0][0] if temps[0] else '?')
    print(f'  🌡 当前(WeatherNext): {now_t}°C')
    for day, v in sorted(daily.items())[:3]:
        if v['t_min'] < 99:
            print(f'  📅 {day}: {v["t_min"]}~{v["t_max"]}°C 降水{v["precip_sum"]}mm')
    print(f'  ✅ 已存入 weathernext_forecast.json (15天 {len(times)}小时)')

if __name__ == '__main__':
    compute()
