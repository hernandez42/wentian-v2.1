#!/usr/bin/env python3
"""
WeatherNext v3.0 — Google WeatherNext 3 优先, WN2回退
====================================================
主模型: Google Maps Platform Weather API (WeatherNext 3, 1小时间隔, 5km精度)
备选:   Open-Meteo WeatherNext 2 Ensemble (64成员, 15天)
回退:   Open-Meteo 标准预报

API:  https://weather.googleapis.com/v1/forecast/hours:lookup  (WN3, 需API Key)
      https://ensemble-api.open-meteo.com/v1/ensemble?models=google_weathernext2_ensemble (WN2)
      https://api.open-meteo.com/v1/forecast (标准)

v3.0 (2026-09-09): 主源换为Google Maps Platform Weather API(WN3)
  需设置环境变量 GOOGLE_MAPS_API_KEY
  如无可用的Key, 自动降级到WN2
"""
import json, os, time
from datetime import datetime
import urllib.request, ssl

OUT_DIR = '/root/data/fusion'
LAT = '25.09917'
LON = '102.92667'
GOOGLE_API_KEY = os.environ.get('GOOGLE_MAPS_API_KEY', '')


def _ctx():
    c = ssl.create_default_context(); c.check_hostname = True; c.verify_mode = ssl.CERT_REQUIRED
    return c


def _fetch(url: str, timeout: int = 30, retries: int = 2) -> dict:
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
    return {}


def fetch_google_wn3():
    """Google Maps Platform Weather API (WeatherNext 3驱动)"""
    if not GOOGLE_API_KEY:
        print('  [WN3] 无GOOGLE_MAPS_API_KEY, 跳过')
        return {}
    print('  [WN3] 查询 Google Maps Weather API (WeatherNext 3)...')
    url = (f"https://weather.googleapis.com/v1/forecast/hours:lookup?"
           f"location.latitude={LAT}&location.longitude={LON}&"
           f"key={GOOGLE_API_KEY}")
    data = _fetch(url, timeout=15)
    if data and 'error' not in data:
        print(f'  [WN3] 成功: 获取到天气预报')
        with open(f'{OUT_DIR}/google_wn3_raw.json', 'w') as f:
            json.dump(data, f, indent=2)
        return data
    print(f'  [WN3] Google API失败, 降级到WN2')
    return {}


def fetch_weathernext():
    """Open-Meteo WeatherNext 2 Ensemble (回退)"""
    print('  [WeatherNext] 拉取 google_weathernext2_ensemble ...')
    url = (f"https://ensemble-api.open-meteo.com/v1/ensemble?"
           f"latitude={LAT}&longitude={LON}&"
           f"models=google_weathernext2_ensemble&"
           f"hourly=temperature_2m,precipitation,pressure_msl,relative_humidity_2m,"
           f"cloud_cover,wind_speed_10m,wind_direction_10m,dew_point_2m&"
           f"forecast_days=15")
    return _fetch(url)