#!/usr/bin/env python3
"""
谷歌气象数据交叉印证模块 v1.0 — 问天 × Google DeepMind
================================================
功能:
  1. 读取 Google WeatherNext 2 + GraphCast 双模型数据
  2. 读取本地真实数据 (METAR, UNO, PWV, Open-Meteo)
  3. 交叉印证: 温度/气压/湿度/降水/天气型
  4. 输出印证报告 → google_validation.json
  5. 检测异常偏差并预警

调用方式:
  python3 google_validate.py              # 正常印证
  python3 google_validate.py --alert-only  # 仅当有异常偏差时输出

输出: /root/data/fusion/google_validation.json
"""
import json, os, sys
from datetime import datetime, timezone
from typing import Optional, Dict, Any, List

FUSION_DIR = '/root/data/fusion'
VALIDATION_OUT = f'{FUSION_DIR}/google_validation.json'

# 偏差阈值 (超出即标记为异常)
THRESHOLD_TEMP_C = 3.0      # 温度偏差 > 3°C 标记
THRESHOLD_PRESS_HPA = 5.0   # 气压偏差 > 5hPa 标记
THRESHOLD_HUMID_PCT = 20    # 湿度偏差 > 20% 标记
THRESHOLD_PRECIP_MM = 2.0   # 降水偏差 > 2mm 标记


def _load_json(path: str, default: Any = None) -> Any:
    try:
        if os.path.exists(path):
            with open(path, encoding='utf-8') as f:
                return json.load(f)
    except (OSError, json.JSONDecodeError) as e:
        print(f'[google_validate] {path}: {e}')
    return default


def _safe(v: Any, default: Any = None) -> Any:
    return v if v is not None else default


def load_local_data() -> Dict[str, Any]:
    """加载本地真实观测数据"""
    local = {}

    # 1. 问天最新数据 (含 METAR, UNO, PWV, Open-Meteo)
    wentian = _load_json(f'{FUSION_DIR}/wentian_latest.json', {})
    if isinstance(wentian, dict):
        wd = wentian.get('data', {}) if isinstance(wentian, dict) else {}
        local['outdoor'] = wd.get('outdoor', {})
        local['metar'] = wd.get('metar_zppp', {})
        local['pwv'] = wd.get('pwv', {})
        local['multi_source'] = wd.get('multi_source', {})
        local['external'] = wd.get('external', {})
        local['uno'] = wd.get('uno', {})
        local['generated_at'] = wentian.get('generated_at', '')

    # 2. Nowcast短临
    local['nowcast'] = _load_json(f'{FUSION_DIR}/nowcast.json', {})

    # 3. 终极融合预测
    local['ultimate'] = _load_json(f'{FUSION_DIR}/ultimate_forecast.json', {})

    return local


def load_google_data() -> Dict[str, Any]:
    """加载 Google DeepMind 双模型数据"""
    google = {}

    # 1. WeatherNext 2
    wn = _load_json(f'{FUSION_DIR}/weathernext_forecast.json', {})
    if isinstance(wn, dict) and wn.get('model') == 'google_weathernext2_ensemble':
        google['weathernext2'] = wn

    # 2. GraphCast
    gc = _load_json(f'{FUSION_DIR}/google_graphcast.json', {})
    if isinstance(gc, dict) and gc.get('model') == 'google_graphcast':
        google['graphcast'] = gc

    return google


def extract_now_local(local: Dict[str, Any]) -> Dict[str, Any]:
    """提取当前时刻的本地真实观测值"""
    now = {}

    # 室外 (Open-Meteo)
    od = local.get('outdoor', {})
    now['temperature'] = _safe(od.get('temperature'))
    now['humidity'] = _safe(od.get('humidity'))
    now['pressure_msl'] = _safe(od.get('pressure_msl'))
    now['wind_speed'] = _safe(od.get('wind_speed'))
    now['wind_dir'] = _safe(od.get('wind_dir'))
    now['weather'] = _safe(od.get('weather'))
    now['precip'] = _safe(od.get('precip'))
    now['cloud_cover'] = _safe(od.get('cloud_cover'))

    # METAR ZPPP (权威)
    met = local.get('metar', {})
    if met:
        met_temp = _safe(met.get('temp'))
        if met_temp is not None and met_temp > -50:
            now['metar_temp'] = met_temp
        met_p = _safe(met.get('pressure'))
        if met_p is not None and met_p > 900:
            now['metar_pressure'] = met_p
        now['metar_wind'] = _safe(met.get('wind_dir'))
        now['metar_wind_speed'] = _safe(met.get('wind_speed'))
        now['metar_raw'] = _safe(met.get('raw'))

    # UNO机柜
    uno = local.get('uno', {})
    if isinstance(uno, dict) and uno.get('T'):
        now['uno_temp'] = _safe(uno.get('T'))
        now['uno_rh'] = _safe(uno.get('rh'))
        now['uno_pressure'] = _safe(uno.get('p_sea'))

    # PWV
    pwv = local.get('pwv', {})
    if isinstance(pwv, dict):
        now['pwv_mm'] = _safe(pwv.get('pwv_mm'))
        now['pwv_delta'] = _safe(pwv.get('delta_pwv'))
        now['pwv_storm_score'] = _safe(pwv.get('storm_score'))

    # Nowcast
    nc = local.get('nowcast', {})
    if isinstance(nc, dict):
        now['nowcast_score'] = _safe(nc.get('score'))
        now['nowcast_primary'] = _safe(nc.get('primary_type'))
        now['thunder_score'] = _safe(nc.get('thunder_score'))
        now['squall_score'] = _safe(nc.get('squall_score'))
        now['false_cold_score'] = _safe(nc.get('false_cold_score'))
        now['stationary_score'] = _safe(nc.get('stationary_score'))
        now['wind_shear_score'] = _safe(nc.get('wind_shear_score'))

    # External sources
    ext = local.get('external', {})
    if isinstance(ext, dict):
        now['wttr_temp'] = _safe(ext.get('wttr_temp'))
        now['metno_temp'] = _safe(ext.get('metno_temp'))
        now['metno_pressure'] = _safe(ext.get('metno_pressure'))

    return now


def extract_now_google(google: Dict[str, Any], now_local: Dict) -> List[Dict[str, Any]]:
    """从 Google 模型中提取当前时刻的预测值"""
    now_google = []
    now_ts = datetime.now(timezone.utc).strftime('%Y-%m-%dT%H')

    for model_name in ['weathernext2', 'graphcast']:
        model = google.get(model_name)
        if not model or 'forecast_hours' not in model:
            continue

        # 找最近的预报小时 (匹配当前小时)
        best_entry = None
        best_diff = 9999.0
        now_dt = datetime.strptime(now_ts[:13] + ':00', '%Y-%m-%dT%H:%M')
        for entry in model['forecast_hours']:
            et = entry.get('time', '')
            if not et:
                continue
            # 直接匹配到小时
            if et[:13] == now_ts[:13]:
                best_entry = entry
                break
            # 否则记录最小时间差
            try:
                entry_dt = datetime.strptime(et[:16], '%Y-%m-%dT%H:%M')
                diff = abs((entry_dt - now_dt).total_seconds())
                if diff < best_diff:
                    best_diff = diff
                    best_entry = entry
            except (ValueError, OSError):
                continue

        if best_entry:
            now_google.append({
                'model': model_name,
                'temperature': _safe(best_entry.get('temperature_2m')),
                'precipitation_mm': _safe(best_entry.get('precipitation_mm')),
                'pressure_msl_hpa': _safe(best_entry.get('pressure_msl_hpa')),
                'relative_humidity_pct': _safe(best_entry.get('relative_humidity_pct')),
                'cloud_cover_pct': _safe(best_entry.get('cloud_cover_pct')),
                'wind_speed_kmh': _safe(best_entry.get('wind_speed_kmh')),
                'wind_direction_deg': _safe(best_entry.get('wind_direction_deg')),
                'dew_point_c': _safe(best_entry.get('dew_point_c')),
                'time': _safe(best_entry.get('time')),
            })

    return now_google


def cross_validate(local: Dict, google_now: List[Dict]) -> Dict[str, Any]:
    """交叉印证: 比较本地真实值与 Google 模型预测"""
    result = {
        'ts': int(datetime.now(timezone.utc).timestamp()),
        'time': datetime.now().strftime('%Y-%m-%d %H:%M:%S'),
        'location': '昆明长水 ZPPP',
        'local': _safe_dict(local),
        'google_models': google_now,
        'comparisons': [],
        'anomalies': [],
        'confidence': 'HIGH',  # 默认高置信度
        'summary': ''
    }

    if not google_now:
        result['confidence'] = 'LOW'
        result['summary'] = '无 Google 模型数据可印证'
        return result

    # 逐模型比较
    for gm in google_now:
        model_name = gm.get('model', 'unknown')
        comp = {'model': model_name, 'fields': {}, 'anomalies': []}

        # 温度
        gt = gm.get('temperature')
        lt = local.get('metar_temp') or local.get('temperature') or local.get('uno_temp')
        if gt is not None and lt is not None:
            diff = gt - lt
            comp['fields']['temperature_c'] = {
                'google': round(gt, 1),
                'local': round(lt, 1),
                'diff': round(diff, 1),
                'status': 'OK' if abs(diff) <= THRESHOLD_TEMP_C else 'ANOMALY'
            }
            if abs(diff) > THRESHOLD_TEMP_C:
                result['anomalies'].append(f'[{model_name}] 温度偏差 {diff:.1f}°C (Google {gt}°C vs 本地 {lt}°C)')

        # 气压
        gp = gm.get('pressure_msl_hpa')
        lp = local.get('metar_pressure') or local.get('pressure_msl') or local.get('uno_pressure')
        if gp is not None and lp is not None:
            diff = gp - lp
            comp['fields']['pressure_hpa'] = {
                'google': round(gp, 1),
                'local': round(lp, 1),
                'diff': round(diff, 1),
                'status': 'OK' if abs(diff) <= THRESHOLD_PRESS_HPA else 'ANOMALY'
            }
            if abs(diff) > THRESHOLD_PRESS_HPA:
                result['anomalies'].append(f'[{model_name}] 气压偏差 {diff:.1f}hPa (Google {gp}hPa vs 本地 {lp}hPa)')

        # 湿度
        gh = gm.get('relative_humidity_pct')
        lh = local.get('humidity') or local.get('uno_rh')
        if gh is not None and lh is not None:
            diff = gh - lh
            comp['fields']['humidity_pct'] = {
                'google': round(gh, 0),
                'local': round(lh, 0),
                'diff': round(diff, 0),
                'status': 'OK' if abs(diff) <= THRESHOLD_HUMID_PCT else 'ANOMALY'
            }
            if abs(diff) > THRESHOLD_HUMID_PCT:
                result['anomalies'].append(f'[{model_name}] 湿度偏差 {diff:.0f}% (Google {gh}% vs 本地 {lh}%)')

        # 降水
        gp_mm = gm.get('precipitation_mm')
        if gp_mm is not None and gp_mm > 0:
            lp_mm = local.get('precip', 0)
            diff = gp_mm - lp_mm
            if abs(diff) > THRESHOLD_PRECIP_MM:
                result['anomalies'].append(f'[{model_name}] 降水偏差 {diff:.1f}mm (Google {gp_mm}mm vs 本地 {lp_mm}mm)')

        # 云量
        gc = gm.get('cloud_cover_pct')
        lc = local.get('cloud_cover')
        if gc is not None and lc is not None:
            diff = gc - lc
            comp['fields']['cloud_pct'] = {
                'google': round(gc, 0),
                'local': round(lc, 0),
                'diff': round(diff, 0),
                'status': 'OK' if abs(diff) <= 30 else 'ANOMALY'
            }

        result['comparisons'].append(comp)

    # 综合置信度
    if len(result['anomalies']) >= 3:
        result['confidence'] = 'LOW'
        result['summary'] = f'⚠ 检测到 {len(result["anomalies"])} 项异常偏差, 需关注数据一致性'
    elif result['anomalies']:
        result['confidence'] = 'MEDIUM'
        result['summary'] = f'部分偏差: {len(result["anomalies"])} 项异常'
    else:
        result['confidence'] = 'HIGH'
        # 检查 Google 模型间的一致性
        if len(google_now) >= 2:
            t_vals = [g.get('temperature') for g in google_now if g.get('temperature') is not None]
            if len(t_vals) >= 2 and max(t_vals) - min(t_vals) > 2:
                result['summary'] = f'本地与Google模型一致✓ 但模型间温差{max(t_vals)-min(t_vals):.1f}°C'
            else:
                result['summary'] = '多源数据一致 ✓ 高置信度'
        else:
            result['summary'] = '本地与Google数据一致 ✓'

    return result


def _safe_dict(d: Dict) -> Dict:
    """安全化: 只保留标量值, 移除嵌套对象"""
    ok_keys = ['temperature', 'humidity', 'pressure_msl', 'wind_speed', 'wind_dir',
               'weather', 'precip', 'cloud_cover', 'pwv_mm', 'pwv_storm_score',
               'nowcast_score', 'nowcast_primary', 'thunder_score',
               'metar_temp', 'metar_pressure', 'uno_temp', 'uno_rh', 'uno_pressure',
               'metno_temp', 'metno_pressure', 'wttr_temp']
    out = {}
    for k in ok_keys:
        if k in d and d[k] is not None:
            out[k] = d[k]
    return out


def format_validation_card(val: Dict) -> List[str]:
    """格式化验证结果为飞书卡片行"""
    lines = []
    conf = val.get('confidence', 'LOW')
    icon_map = {'HIGH': '✅', 'MEDIUM': '⚠️', 'LOW': '🔴'}

    lines.append(f'{icon_map.get(conf, "❓")} 【Google DeepMind 交叉印证】')
    lines.append(f'  置信度: {conf}')

    # 各模型对比详情
    for comp in val.get('comparisons', []):
        model_name = comp.get('model', 'unknown')
        label = 'WeatherNext 2' if 'weathernext' in model_name else 'GraphCast'
        fields = comp.get('fields', {})

        t = fields.get('temperature_c', {})
        p = fields.get('pressure_hpa', {})
        h = fields.get('humidity_pct', {})

        parts = []
        if t:
            parts.append(f'🌡 {t["local"]}°C(G:{t["google"]}°C)')
        if p:
            parts.append(f'🏋 {p["local"]}hPa(G:{p["google"]}hPa)')
        if h:
            parts.append(f'💧 {h["local"]}%(G:{h["google"]}%)')

        if parts:
            lines.append(f'  {label}: {" | ".join(parts)}')
        else:
            lines.append(f'  {label}: 数据不足')

    # 异常
    anomalies = val.get('anomalies', [])
    if anomalies:
        for a in anomalies[:3]:  # 最多显示3条
            lines.append(f'  ⚠ {a}')
        if len(anomalies) > 3:
            lines.append(f'  ... 还有 {len(anomalies)-3} 项异常')

    lines.append(f'  📊 摘要: {val.get("summary", "")}')
    return lines


def main():
    alert_only = '--alert-only' in sys.argv

    print('━━━ Google 交叉印证 ━━━')

    # 1. 加载数据
    local = load_local_data()
    google = load_google_data()

    # 2. 提取当前值
    now_local = extract_now_local(local)
    now_google = extract_now_google(google, now_local)

    print(f'  本地数据源: {len([k for k,v in now_local.items() if v is not None])} 项有效')
    print(f'  Google模型: {len(now_google)} 个')
    for g in now_google:
        print(f'    {g["model"]}: {g.get("time","?")} ~ {g.get("temperature","?")}°C')

    # 3. 交叉印证
    result = cross_validate(now_local, now_google)

    # 4. 保存
    with open(VALIDATION_OUT, 'w') as f:
        json.dump(result, f, indent=2, ensure_ascii=False)
    print(f'  已保存: {VALIDATION_OUT}')

    # 5. 输出
    if not alert_only or result['anomalies']:
        for line in format_validation_card(result):
            print(f'  {line}')

    return result


if __name__ == '__main__':
    main()
