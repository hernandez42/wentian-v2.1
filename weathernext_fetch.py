#!/usr/bin/env python3
"""
问天 多模型融合预报引擎 v4.0
============================
同时拉取全球顶级气象模型, 融合为综合预报

模型列表:
  [1] Google WeatherNext 2 (AI ensemble, 64成员, 15天) — Open-Meteo代理
  [2] ECMWF IFS 0.25° (全球最权威数值模型) — Open-Meteo代理
  [3] NOAA GFS Seamless (全球标准) — Open-Meteo代理
  [4] DWD ICON Global (德国气象局) — Open-Meteo代理
  [5] GEM Global (加拿大环境部) — Open-Meteo代理
  [6] Google Maps Weather API (WeatherNext 3, 需GOOGLE_MAPS_API_KEY)

输出: /root/data/fusion/multi_model_forecast.json
v4.0 (2026-09-09): 首次多模型融合
"""
import json, os, time, sys
from datetime import datetime
import urllib.request, ssl

OUT_DIR = '/root/data/fusion'
LAT = '25.09917'
LON = '102.92667'
GOOGLE_API_KEY = os.environ.get('GOOGLE_MAPS_API_KEY', '')


def _ctx():
    c = ssl.create_default_context(); c.check_hostname = True; c.verify_mode = ssl.CERT_REQUIRED
    return c


def _fetch(url: str, timeout: int = 25, retries: int = 2) -> dict:
    for attempt in range(1, retries + 1):
        try:
            req = urllib.request.Request(url, headers={'User-Agent': 'curl/7.81.0'})
            with urllib.request.urlopen(req, timeout=timeout, context=_ctx()) as r:
                return json.loads(r.read())
        except Exception as e:
            print(f'  [fetch] 第{attempt}次失败: {e}')
            if attempt < retries:
                import time as _t
                _t.sleep(2 ** attempt)
    return None


# ── 模型1: Google WeatherNext 2 Ensemble ──
def fetch_wn2():
    """Google DeepMind AI ensemble, 64成员, 0.25°分辨率"""
    print('  [模型1] Google WeatherNext 2 Ensemble...')
    url = (f"https://ensemble-api.open-meteo.com/v1/ensemble?"
           f"latitude={LAT}&longitude={LON}&"
           f"models=google_weathernext2_ensemble&"
           f"hourly=temperature_2m,precipitation,pressure_msl,relative_humidity_2m,"
           f"cloud_cover,wind_speed_10m,wind_direction_10m,weather_code&"
           f"forecast_days=15")
    return _fetch(url, timeout=30)


# ── 模型2: ECMWF IFS 0.25° ──
def fetch_ecmwf():
    """ECMWF IFS — 全球公认最准确的数值天气预报"""
    print('  [模型2] ECMWF IFS 0.25°...')
    url = (f"https://api.open-meteo.com/v1/forecast?"
           f"latitude={LAT}&longitude={LON}&"
           f"models=ecmwf_ifs&"
           f"hourly=temperature_2m,precipitation,pressure_msl,relative_humidity_2m,"
           f"cloud_cover,wind_speed_10m,wind_direction_10m,weather_code&"
           f"forecast_days=10")
    return _fetch(url)


# ── 模型3: NOAA GFS Seamless ──
def fetch_gfs():
    """NOAA GFS — 全球标准, 美国国家气象局"""
    print('  [模型3] NOAA GFS Seamless...')
    url = (f"https://api.open-meteo.com/v1/forecast?"
           f"latitude={LAT}&longitude={LON}&"
           f"models=gfs_seamless&"
           f"hourly=temperature_2m,precipitation,pressure_msl,relative_humidity_2m,"
           f"cloud_cover,wind_speed_10m,wind_direction_10m,weather_code&"
           f"forecast_days=10")
    return _fetch(url)


# ── 模型4: DWD ICON Global ──
def fetch_icon():
    """DWD ICON — 德国气象局高精度全球模型"""
    print('  [模型4] DWD ICON Global...')
    url = (f"https://api.open-meteo.com/v1/forecast?"
           f"latitude={LAT}&longitude={LON}&"
           f"models=dwd_icon_global&"
           f"hourly=temperature_2m,precipitation,pressure_msl,relative_humidity_2m,"
           f"cloud_cover,wind_speed_10m,wind_direction_10m,weather_code&"
           f"forecast_days=10")
    return _fetch(url)


# ── 模型5: GEM Global ──
def fetch_gem():
    """GEM — 加拿大环境部全球模型"""
    print('  [模型5] GEM Global...')
    url = (f"https://api.open-meteo.com/v1/forecast?"
           f"latitude={LAT}&longitude={LON}&"
           f"models=gem_global&"
           f"hourly=temperature_2m,precipitation,pressure_msl,relative_humidity_2m,"
           f"cloud_cover,wind_speed_10m,wind_direction_10m,weather_code&"
           f"forecast_days=10")
    return _fetch(url)


# ── 模型6: Google Maps Weather API (WN3) ──
def fetch_wn3():
    """Google WeatherNext 3 via Maps Platform API (需KEY)"""
    if not GOOGLE_API_KEY:
        print('  [模型6] Google WN3: 无API Key, 跳过')
        return None
    print('  [模型6] Google Maps Weather API (WeatherNext 3)...')
    url = (f"https://weather.googleapis.com/v1/forecast/hours:lookup?"
           f"location.latitude={LAT}&location.longitude={LON}&"
           f"key={GOOGLE_API_KEY}")
    return _fetch(url, timeout=15)


# ── 融合所有模型 ──
def fuse_models(results: dict) -> dict:
    """多模型加权融合为综合预报"""
    models = results.get('models', {})
    if not models:
        return {'error': '无可用模型'}
    
    fusion = {
        'ts': datetime.now().isoformat(),
        'lat': LAT, 'lon': LON,
        'models_available': list(models.keys()),
        'models_count': len(models),
        'ensemble': {}
    }
    
    # 提取每个模型的每小时数据, 加权平均
    hourly_data = {}  # {timestamp: {temp:[], precip:[], ...}}
    model_weights = {
        'wn2': 0.25,      # Google AI ensemble
        'ecmwf': 0.25,    # ECMWF 权威
        'gfs': 0.20,      # GFS 全球标准
        'icon': 0.15,     # DWD ICON
        'gem': 0.15,      # GEM 加拿大
    }
    
    for mname, mdata in models.items():
        if not mdata or 'hourly' not in mdata:
            continue
        hourly = mdata['hourly']
        times = hourly.get('time', [])
        weight = model_weights.get(mname, 0.2)
        
        for i, t in enumerate(times):
            if t not in hourly_data:
                hourly_data[t] = {'models': 0, 'weights': 0,
                                  'temp': 0, 'precip': 0, 'press': 0,
                                  'humid': 0, 'cloud': 0, 'wind': 0,
                                  'wcode': []}
            hourly_data[t]['models'] += 1
            hourly_data[t]['weights'] += weight
            
            for var, key in [('temperature_2m', 'temp'),
                             ('precipitation', 'precip'),
                             ('pressure_msl', 'press'),
                             ('relative_humidity_2m', 'humid'),
                             ('cloud_cover', 'cloud'),
                             ('wind_speed_10m', 'wind')]:
                vals = hourly.get(var, [])
                if i < len(vals) and vals[i] is not None:
                    hourly_data[t][key] += vals[i] * weight
            
            wc = hourly.get('weather_code', [])
            if i < len(wc) and wc[i] is not None:
                hourly_data[t]['wcode'].append(wc[i])
    
    # 整理输出
    times_sorted = sorted(hourly_data.keys())
    fusion['hourly'] = []
    for t in times_sorted:
        hd = hourly_data[t]
        w = hd['weights']
        if w > 0:
            entry = {
                'time': t,
                'models': hd['models'],
                'temperature_2m': round(hd['temp'] / w, 1),
                'precipitation': round(hd['precip'] / w, 1),
                'pressure_msl': round(hd['press'] / w, 1),
                'relative_humidity_2m': round(hd['humid'] / w, 1),
                'cloud_cover': round(hd['cloud'] / w, 1),
                'wind_speed_10m': round(hd['wind'] / w, 1),
            }
            # 多数投票: 取出现最多的天气码
            if hd['wcode']:
                from collections import Counter
                entry['weather_code'] = Counter(hd['wcode']).most_common(1)[0][0]
            fusion['hourly'].append(entry)
    
    fusion['hours_count'] = len(fusion['hourly'])
    return fusion


# ── 主函数 ──
def main():
    print('━' * 50)
    print('问天 多模型融合预报引擎 v4.1')
    print(f'坐标: {LAT}, {LON} (ZPPP 昆明长水)')
    print(f'时间: {datetime.now().strftime("%Y-%m-%d %H:%M:%S")}')

    results = {'ts': time.time(), 'models': {}}

    # 并行拉取所有模型
    import concurrent.futures
    fetchers = {
        'wn2': fetch_wn2,
        'ecmwf': fetch_ecmwf,
        'gfs': fetch_gfs,
        'icon': fetch_icon,
        'gem': fetch_gem,
    }
    if GOOGLE_API_KEY:
        fetchers['wn3'] = fetch_wn3

    # ⚠ 修复(2026-09-11): 旧 as_completed(timeout=60) 小于 wn2 单源最坏耗时
    # (30s×2重试+退避>60s), 超时抛 TimeoutError 未捕获 → main崩溃且本轮不落盘。
    # 预算提到120s + 异常兜底, 单源失败不再拖垮全局。
    with concurrent.futures.ThreadPoolExecutor(max_workers=6) as ex:
        future_map = {ex.submit(fn): name for name, fn in fetchers.items()}
        try:
            for future in concurrent.futures.as_completed(future_map, timeout=120):
                name = future_map[future]
                try:
                    data = future.result()
                    if data:
                        results['models'][name] = data
                        print(f'  ✅ {name} 成功')
                    else:
                        print(f'  ⚠ {name} 返回空')
                except Exception as e:
                    print(f'  ⚠ {name} 异常: {e}')
        except concurrent.futures.TimeoutError:
            print('  ⚠ 部分模型抓取超时, 用已到手的继续融合')

    # 融合
    print(f'\n  成功获取 {len(results["models"])}/6 个模型')
    fusion = fuse_models(results)

    # 保存 — ⚠ 修复(2026-09-11): 旧版先写文件后融合, 融合崩溃会留下半截JSON;
    # 且磁盘满时 json.dump 半截落盘。改为 tmp+rename 原子写。
    results['fusion'] = fusion
    path = f'{OUT_DIR}/multi_model_forecast.json'
    tmp = path + '.tmp'
    with open(tmp, 'w') as f:
        json.dump(results, f, indent=2, ensure_ascii=False)
    os.replace(tmp, path)
    print(f'\n✅ 融合预报已保存: {path}')
    print(f'   模型数: {fusion.get("models_count", 0)}')
    print(f'   预报时次: {fusion.get("hours_count", 0)}')

    # ⚠ 修复(2026-09-11): 旧版直接 dump wn2 的 Open-Meteo 原始响应
    # ({"latitude":...,"hourly":{...}}), 而全部4个下游消费者
    # (feishu_ultimate_push._section_24h / get_weathernext_summary,
    #  google_validate, score_forecast) 都期望 {model, forecast_hours[], summary} —
    # 契约断裂导致"未来24h"段永远空、6天永远走fallback、Google印证永远无数据。
    # 新版: 写标准契约格式, 4个消费者全部复活。
    if 'wn2' in results['models']:
        wn2_std = _to_standard_forecast(results['models']['wn2'])
        if wn2_std:
            p2 = f'{OUT_DIR}/weathernext_forecast.json'
            t2 = p2 + '.tmp'
            with open(t2, 'w') as f:
                json.dump(wn2_std, f, indent=2, ensure_ascii=False)
            os.replace(t2, p2)
            print(f'   标准契约: weathernext_forecast.json ({len(wn2_std["forecast_hours"])}时次)')


def _to_standard_forecast(wn2_raw):
    """Open-Meteo WN2 ensemble 原始响应 → 下游统一契约
    输出: {model, ts, forecast_hours: [{time, temperature_2m, precipitation_mm,
             pressure_msl_hpa, relative_humidity_pct, cloud_cover_pct,
             wind_speed_kmh, wind_direction_deg}], summary: {daily: {...}}}
    注: OM ensemble API 返回 64成员中位(_member后缀为成员值, 顶层无后缀即中位)。
    时间为 UTC(iso8601), 消费方比较时须转本地时(UTC+8)。"""
    try:
        hourly = wn2_raw.get('hourly', {})
        times = hourly.get('time', [])
        if not times:
            return None
        hours = []
        for i, t in enumerate(times):
            def _g(var):
                vals = hourly.get(var, [])
                return vals[i] if i < len(vals) and vals[i] is not None else None
            hours.append({
                'time': t,
                'temperature_2m': _g('temperature_2m'),
                'precipitation_mm': _g('precipitation'),
                'pressure_msl_hpa': _g('pressure_msl'),
                'relative_humidity_pct': _g('relative_humidity_2m'),
                'cloud_cover_pct': _g('cloud_cover'),
                'wind_speed_kmh': _g('wind_speed_10m'),
                'wind_direction_deg': _g('wind_direction_10m'),
                'weather_code': _g('weather_code'),
            })
        # daily summary
        daily = {}
        for h in hours:
            day = h['time'][:10]
            d = daily.setdefault(day, {'temps': [], 'precip': 0.0, 'hours_n': 0})
            if h['temperature_2m'] is not None:
                d['temps'].append(h['temperature_2m'])
            if h['precipitation_mm']:
                d['precip'] += h['precipitation_mm']
            d['hours_n'] += 1
        summary_daily = {}
        for day, d in sorted(daily.items()):
            if d['temps']:
                summary_daily[day] = {
                    'temp_min': round(min(d['temps']), 1),
                    'temp_max': round(max(d['temps']), 1),
                    'precip_sum_mm': round(d['precip'], 1),
                    'hours_n': d['hours_n'],
                }
        return {
            'model': 'google_weathernext2_ensemble',
            'ts': int(time.time()),
            'utc_offset_seconds': wn2_raw.get('utc_offset_seconds', 0),
            'forecast_hours': hours,
            'summary': {'daily': summary_daily},
        }
    except Exception as e:
        print(f'  ⚠ 标准格式转换失败: {e}')
        return None


if __name__ == '__main__':
    main()
