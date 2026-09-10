#!/usr/bin/env python3
"""
问天气象站 飞书推送 v8.0 (问天22维+LLM深度分析)
================================
v8.0 (2026-09-06): 推送前调用DeepSeek进行多源数据LLM深度分析, 新增AI分析卡片段
v1.1.2 (2026-09-03): 加入 wentian 22维度签名, 修复NoneType.format错误
v7.0: MCP优化 (complexity 48→25, 修SQL列名, 移除except:pass)
"""
import sys, os, json, re, urllib.request, ssl, sqlite3
from datetime import datetime, timedelta
from pathlib import Path
from typing import Optional, Dict, List, Any

sys.path.insert(0, '/root/.hermes')
sys.path.insert(0, '/root/scripts')
FUSION_DIR = '/root/data/fusion'
DB = '/root/data/ano_weather.db'
# v1.1.2: 问天数据导出 (C权威, Python只读)
WENTIAN_JSON = '/root/data/fusion/wentian_latest.json'
LAT, LON = 25.09917, 102.92667  # ⚠ 2026-09-07: 修正为长水机场真坐标(原25.0820导致数据偏差)
ALT = 2103  # 长水机场ZPPP真海拔 2103.5m
FEISHU_USER = os.environ.get('FEISHU_USER_ID', 'ou_52a5a07c6c4c825ccb530efe5befcc77')

# R9 (2026-09-09): 钦天监 + 中国传统天象模块 (主人明示"必须结合中国传统天象")
try:
    from qintianjian_calendar import get_qintianjian, render_push_section as _qintianjian_render
    HAS_QINTIANJIAN = True
except Exception as _qt_e:
    print(f'[qintianjian] 模块加载失败: {_qt_e}')
    HAS_QINTIANJIAN = False

# ── 网络 ──────────────────────────────────────────────────────────
def _ctx(insecure: bool = True) -> ssl.SSLContext:
    """SSL context: insecure=True用于外部API(Open-Meteo等), insecure=False用于飞书(需验证)"""
    c = ssl.create_default_context()
    if insecure:
        c.check_hostname = False
        c.verify_mode = ssl.CERT_NONE
    return c

def _fetch(url: str, timeout: int = 15, retries: int = 3, insecure: bool = True) -> Optional[bytes]:
    """统一网络抓取 - 带重试(5xx/超时指数退避), 最终失败返回None而不是 'ERR:..'字符串
    ⚠ 修复(2026-09-05): 旧版无重试, Open-Meteo一次503整条推送实况全变0"""
    import time as _t
    last_err = None
    for attempt in range(1, retries + 1):
        try:
            req = urllib.request.Request(url, headers={'User-Agent': 'curl/7.81.0'})
            with urllib.request.urlopen(req, timeout=timeout, context=_ctx(insecure)) as r:
                return r.read()
        except urllib.error.HTTPError as e:
            last_err = e
            # 4xx不重试(参数错), 5xx重试
            if e.code < 500:
                print(f'[_fetch] {url[:80]}... HTTP {e.code}, 不重试')
                return None
        except Exception as e:
            last_err = e
        if attempt < retries:
            backoff = 2 ** attempt
            print(f'[_fetch] 第{attempt}次失败({last_err}), {backoff}s后重试')
            _t.sleep(backoff)
    print(f'[_fetch] {url[:80]}... 重试{retries}次仍失败: {last_err}')
    return None

# ── WMO天气码→中文/图标 ────────────────────────────────────────
WMO_TEXT = {
    0: '晴', 1: '晴间少云', 2: '多云', 3: '阴',
    45: '雾', 48: '雾凇',
    51: '小雨', 53: '中雨', 55: '大雨', 56: '冻雨', 57: '强冻雨',
    61: '小雨', 63: '中雨', 65: '大雨', 66: '冻雨', 67: '强冻雨',
    71: '小雪', 73: '中雪', 75: '大雪', 77: '雪粒',
    80: '阵雨', 81: '强阵雨', 82: '暴阵雨',
    85: '阵雪', 86: '强阵雪',
    95: '雷暴', 96: '雷暴伴冰雹', 99: '强雷暴伴冰雹'
}

WMO_ICON = {
    0: '☀️', 1: '🌤', 2: '⛅', 3: '☁️',
    45: '🌫', 48: '🌫',
    51: '🌦', 53: '🌦', 55: '🌧', 56: '🌧', 57: '🌧',
    61: '🌧', 63: '🌧', 65: '⛈', 66: '🌧', 67: '⛈',
    71: '🌨', 73: '🌨', 75: '❄️', 77: '🌨',
    80: '🌦', 81: '⛈', 82: '⛈',
    85: '🌨', 86: '❄️',
    95: '⛈', 96: '⛈', 99: '⛈'
}

def wmo_text(code: int) -> str:
    return WMO_TEXT.get(code, f'未知({code})')

def wmo_icon(code: int) -> str:
    return WMO_ICON.get(code, '❓')

# ── 风向 ───────────────────────────────────────────────────────────
def wind_dir(deg: Optional[float]) -> str:
    if deg is None: return '?'
    dirs = ['北', '东北', '东', '东南', '南', '西南', '西', '西北']
    return dirs[int((deg + 22.5) // 45) % 8]

# ── 1. 拉实况 + 7天预报 (从C引擎outdoor表 + 多模型融合) ─────
def fetch_openmeteo() -> Dict[str, Any]:
    """
    v4.0: 不再直接调Open-Meteo API
    实况: 从问天DB outdoor表读(C引擎用met.no主源)
    预报: 从多模型融合JSON读 (5模型: WN2+ECMWF+GFS+ICON+GEM)
    Open-Meteo API仅在最末兜底时调用
    """
    result = {'current': {}, 'hourly': {}, 'daily': {}}
    
    # 1. 实况: 从C引擎outdoor表
    try:
        with sqlite3.connect('/root/data/wentian.db') as c:
            row = c.execute(
                "SELECT temp, humid, pressure, wind_s, wind_d, weather, precip, "
                "       cloud, uv, vis, ts "
                "FROM outdoor ORDER BY ts DESC LIMIT 1"
            ).fetchone()
        if row and row[0] is not None:
            result['current'] = {
                'temperature_2m': row[0],
                'relative_humidity_2m': row[1],
                'pressure_msl': row[2],
                'wind_speed_10m': row[3],
                'wind_direction_10m': row[4],
                'weather_code': _to_wmo(row[5]),
                'precipitation': row[6] or 0,
                'cloud_cover': row[7] or 0,
                'uv_index': row[8] or 0,
                'visibility': row[9] or 10000,
                'ts': row[10],
            }
            print(f'[fetch] 实况: T={row[0]:.1f}°C H={row[1]:.0f}% P={row[2]:.0f}hPa (源: C引擎/met.no)')
    except Exception as e:
        print(f'[fetch] 实况DB读取出错: {e}')
    
    # 2. 预报: 从多模型融合JSON
    try:
        mm_path = '/root/data/fusion/multi_model_forecast.json'
        if os.path.exists(mm_path):
            with open(mm_path) as f:
                mm = json.load(f)
            fusion = mm.get('fusion', {})
            hourly_list = fusion.get('hourly', [])
            if hourly_list:
                # 组装为Open-Meteo兼容格式
                result['hourly'] = _to_om_hourly(hourly_list)
                result['daily'] = _to_om_daily(hourly_list)
                print(f'[fetch] 预报: {len(hourly_list)}时次 (源: 5模型融合)')
    except Exception as e:
        print(f'[fetch] 多模型读取出错: {e}')
    
    # 3. 如果实况/预报都没拿到, 兜底调Open-Meteo API
    if not result['current'] or not result['hourly']:
        print('[fetch] ⚠ 本地数据不足, 回退Open-Meteo API...')
        url = (
            f'https://api.open-meteo.com/v1/forecast?'
            f'latitude={LAT}&longitude={LON}'
            f'&current=temperature_2m,relative_humidity_2m,pressure_msl,'
            f'wind_speed_10m,wind_direction_10m,weather_code,precipitation,cloud_cover'
            f'&hourly=temperature_2m,precipitation,precipitation_probability,cloud_cover,pressure_msl'
            f'&daily=weather_code,temperature_2m_max,temperature_2m_min,precipitation_sum'
            f'&timezone=Asia/Shanghai&forecast_days=7'
        )
        om_data = _fetch(url)
        if om_data:
            try:
                result = json.loads(om_data)
                print('[fetch] 回退Open-Meteo成功')
            except Exception as e:
                print(f'[fetch] Open-Meteo解析失败: {e}')
    
    return result


def _to_wmo(weather_text: str) -> int:
    """天气文本→WMO code (粗略映射)"""
    if not weather_text:
        return 0
    t = weather_text.lower()
    if '晴' in t or 'clear' in t: return 0
    if '多云' in t or 'partly cloudy' in t: return 2
    if '阴' in t or 'overcast' in t: return 3
    if '雾' in t or 'fog' in t: return 45
    if '雨' in t or 'rain' in t or 'drizzle' in t: return 61
    if '雪' in t or 'snow' in t: return 71
    if '雷' in t or 'thunder' in t: return 95
    return 0


def _to_om_hourly(hourly_list: list) -> dict:
    """多模型融合列表→Open-Meteo兼容的hourly dict"""
    return {
        'time': [h['time'] for h in hourly_list],
        'temperature_2m': [h.get('temperature_2m') for h in hourly_list],
        'precipitation': [h.get('precipitation') for h in hourly_list],
        'precipitation_probability': [int(h.get('precipitation', 0) * 10) for h in hourly_list],
        'cloud_cover': [h.get('cloud_cover') for h in hourly_list],
        'pressure_msl': [h.get('pressure_msl') for h in hourly_list],
    }


def _to_om_daily(hourly_list: list) -> dict:
    """小时级数据汇总为天级"""
    daily = {}
    temps = {}
    precips = {}
    wcodes = {}
    for h in hourly_list:
        day = h['time'][:10]  # '2026-09-09T...' → '2026-09-09'
        if day not in temps:
            temps[day] = []
            precips[day] = []
            wcodes[day] = []
        t = h.get('temperature_2m')
        if t is not None:
            temps[day].append(t)
        p = h.get('precipitation', 0)
        if p:
            precips[day].append(p)
        wc = h.get('weather_code')
        if wc is not None:
            wcodes[day].append(wc)
    
    days = sorted(temps.keys())
    return {
        'time': days,
        'temperature_2m_max': [max(temps[d]) for d in days],
        'temperature_2m_min': [min(temps[d]) for d in days],
        'precipitation_sum': [sum(precips.get(d, [0])) for d in days],
        'weather_code': [max(set(wcodes.get(d, [0])), key=wcodes.get(d, [0]).count) if wcodes.get(d) else 0 for d in days],
    }

# ── 2. 拉本地数据库数据 ───────────────────────────────────────────
def get_uno() -> Optional[Dict[str, Any]]:
    """⚠ 已修复: UNO表实际列是 t/h/p/pa, 不是 T_c/rh/p_sea
    ⚠ 2026-09-08: UNO固件QNH换算损坏(报1050.2hPa), 与api_local.c同款修正:
       pa_p_msl = p_station * exp(2104/8430) - 38.8 (校准偏移)"""
    try:
        with sqlite3.connect(DB) as c:
            row = c.execute(
                "SELECT ts, t, h, pa, lr, sw, wx, p, alt "
                "FROM ano_weather WHERE source='UNO_v2.0_bridge' ORDER BY ts DESC LIMIT 1"
            ).fetchone()
        if not row:
            return None
        p_sea = row[3]   # pa (UNO固件算的原始海压, 1050.2hPa, 严重偏高)
        raw_p = row[7]   # p  (本机站压, ~822hPa, 正确)
        # 同 api_local.c: 若 pa 异常(UNO QNH损坏), 用站压重算
        if p_sea is None or p_sea < 980 or p_sea > 1040:
            import math
            alt_km = 2.104  # 长水机场ZPPP海拔
            uno_offset = -38.8  # 校准偏移 (机柜→QNH)
            p_sea = raw_p * math.exp(alt_km * 1000.0 / 8430.0) + uno_offset
        return {
            'ts': row[0], 'T': row[1], 'rh': row[2], 'p_sea': p_sea,  # 修正海压
            'storm': row[4], 'warn': row[5], 'wx': row[6],
            'p': row[7],       # 本机机柜气压
            'alt': row[8]      # 海拔
        }
    except sqlite3.Error as e:
        print(f'[get_uno] DB查询失败: {e}')
        return None

def get_gnss_from_db() -> Optional[Dict[str, Any]]:
    """v3.0: 直接从问天DB读GNSS+S4数据, 绕过JSON桥接层时序问题"""
    try:
        with sqlite3.connect('/root/data/wentian.db') as c:
            row = c.execute(
                "SELECT g.gps_sats, g.bds_sats, g.pdop, "
                "       i.s4_gps, i.s4_bds, i.activity "
                "FROM local_gnss g "
                "LEFT JOIN local_iono i ON i.ts = (SELECT MAX(ts) FROM local_iono) "
                "ORDER BY g.ts DESC LIMIT 1"
            ).fetchone()
        if not row or row[0] is None:
            return None
        return {
            'gps_sats': int(row[0]), 'bds_sats': int(row[1]), 'pdop': float(row[2]),
            's4_gps': float(row[3] or 0), 's4_bds': float(row[4] or 0),
            'activity': str(row[5] or 'N/A')
        }
    except sqlite3.Error as e:
        print(f'[get_gnss_from_db] 失败: {e}')
        return None

def get_weathernext_summary() -> Optional[List[Dict]]:
    """v3.0: 从weathernext_forecast.json读15天预报摘要"""
    try:
        d = json.load(open('/root/data/fusion/weathernext_forecast.json'))
        out = []
        for day, v in sorted(d['summary']['daily'].items()):
            out.append({
                'date': day,
                't_min': v.get('temp_min'),
                't_max': v.get('temp_max'),
                'precip': v.get('precip_sum_mm', 0),
                'pressure': v.get('pressure_avg_hpa')
            })
        return out if out else None
    except Exception as e:
        print(f'[get_weathernext_summary] 失败: {e}')
        return None

def _load_json(path: str, default: Any = None) -> Any:
    """统一JSON加载器 - 失败不抛, 返回默认值"""
    try:
        if not os.path.exists(path):
            return default
        with open(path, encoding='utf-8') as f:
            return json.load(f)
    except (OSError, json.JSONDecodeError) as e:
        print(f'[_load_json] {path} 失败: {e}')
        return default

def get_ult_fusion() -> Optional[Dict[str, Any]]:
    """从本地融合预测JSON读1h/3h/6h"""
    return _load_json(f'{FUSION_DIR}/ultimate_forecast.json')

def get_wentian() -> Optional[Dict[str, Any]]:
    """
    v1.1.2: 读问天C程序导出的最新22维度数据
    问天是C权威数据源, Python只读不写
    返回None表示问天未运行/JSON不存在
    """
    return _load_json(WENTIAN_JSON)

def get_chronos() -> Optional[Dict[str, Any]]:
    return _load_json(f'{FUSION_DIR}/chronos_result.json')

def get_kriging() -> Optional[Dict[str, Any]]:
    return _load_json(f'{FUSION_DIR}/kriging_result.json')

def get_alerts() -> List[str]:
    d = _load_json(f'{FUSION_DIR}/alert_state.json', {})
    return d.get('alerts', []) if isinstance(d, dict) else []

# ── 3. 生活指数(基于实况+预报) ────────────────────────────────
def calc_indices(cur: Dict, daily_today: Dict) -> Dict[str, str]:
    """中央气象台7大指数"""
    out = {}
    T = cur.get('temperature_2m')
    H = cur.get('relative_humidity_2m')
    wind = cur.get('wind_speed_10m')
    vis = cur.get('visibility')
    uv = cur.get('uv_index')
    Tmax = daily_today.get('temp_max')
    Tmin = daily_today.get('temp_min')

    # 1. 穿衣
    if Tmax is not None:
        if Tmax >= 28: out['穿衣'] = '短袖'
        elif Tmax >= 24: out['穿衣'] = '短袖+薄外套'
        elif Tmax >= 18: out['穿衣'] = '长袖+外套'
        elif Tmax >= 10: out['穿衣'] = '毛衣+夹克'
        else: out['穿衣'] = '厚外套/棉服'

    # 2. 紫外线
    if uv is not None:
        if uv <= 2: out['紫外线'] = '最弱'
        elif uv <= 5: out['紫外线'] = '弱'
        elif uv <= 7: out['紫外线'] = '中等'
        elif uv <= 10: out['紫外线'] = '强'
        else: out['紫外线'] = '极强'

    # 3. 洗车
    if daily_today.get('precip_sum', 0) >= 1: out['洗车'] = '不宜'
    elif daily_today.get('precip_prob', 0) >= 60: out['洗车'] = '较不宜'
    else: out['洗车'] = '适宜'

    # 4. 晨练
    if vis and vis < 1000: out['晨练'] = '不宜(能见度低)'
    elif wind and wind > 20: out['晨练'] = '较不宜(大风)'
    elif T is not None and (T < 5 or T > 28): out['晨练'] = '较不宜'
    else: out['晨练'] = '适宜'

    # 5. 感冒
    if Tmin is not None and Tmax is not None:
        diff = Tmax - Tmin
        if Tmin <= 0: out['感冒'] = '极易发'
        elif diff >= 15: out['感冒'] = '易发'
        elif diff >= 10: out['感冒'] = '较易发'
        else: out['感冒'] = '少发'

    # 6. 过敏
    if wind and H:
        if wind >= 15 and H >= 70: out['过敏'] = '较高'
        elif H >= 80: out['过敏'] = '较高'
        else: out['过敏'] = '中等'

    # 7. 钓鱼
    if wind and T:
        cloud = cur.get('cloud_cover')
        if cloud is not None:
            if wind <= 10 and cloud >= 30: out['钓鱼'] = '适宜'
            elif wind <= 15: out['钓鱼'] = '较适宜'
            else: out['钓鱼'] = '不宜'

    return out

# ── 4. 拼接飞书卡片 ─────────────────────────────────────────────
def _safe_float(v: Any, default: float = 0.0) -> float:
    """安全转float - 处理None/字符串/异常"""
    if v is None: return default
    try: return float(v)
    except (ValueError, TypeError): return default

def _safe_int(v: Any, default: int = 0) -> int:
    if v is None: return default
    try: return int(v)
    except (ValueError, TypeError): return default

def _header(now: datetime) -> List[str]:
    """L1: 卡片头 (标题/日期/时间)"""
    today = now.date()
    tomorrow = today + timedelta(days=1)
    wd_cn = ['一','二','三','四','五','六','日']
    L = [
        '━━━━━━━━━━━━━━━━━━━━',
        f'🛰️ 问天气象站 · 昆明长水机场',
        f'📅 {today.strftime("%Y年%m月%d日")} 星期{wd_cn[today.weekday()]}',
        f'⏰ 发布时间: {now.strftime("%H:%M")}',
        '━━━━━━━━━━━━━━━━━━━━'
    ]
    return L

def get_db_current() -> Optional[Dict[str, Any]]:
    """降级数据源: Open-Meteo完全不可用时, 用本地DB最近一条室外实况兜底
    ⚠ 修复(2026-09-05): 杜绝503时推送全0实况"""
    try:
        with sqlite3.connect(DB) as c:
            row = c.execute(
                "SELECT ts, temp_outdoor, humid_outdoor, dew_point, feels_like, "
                "weather_code, cloud_cover_pct, pressure_msl_hpa, wind_speed_kmh, "
                "wind_dir_deg, wind_gust_kmh, uv_index, visibility_m "
                "FROM outdoor_weather WHERE temp_outdoor IS NOT NULL "
                "ORDER BY ts DESC LIMIT 1"
            ).fetchone()
        if not row:
            return None
        return {
            'ts': row[0], 'temperature_2m': row[1], 'relative_humidity_2m': row[2],
            'dew_point_2m': row[3], 'apparent_temperature': row[4],
            'weather_code': row[5], 'cloud_cover': row[6], 'pressure_msl': row[7],
            'wind_speed_10m': row[8], 'wind_direction_10m': row[9],
            'wind_gusts_10m': row[10], 'uv_index': row[11], 'visibility': row[12],
        }
    except sqlite3.Error as e:
        print(f'[get_db_current] DB查询失败: {e}')
        return None

# ⚠ 2026-09-07: 中文天气→图标映射 (问天C程序输出)
WX_CN_ICON = {
    '晴': '☀️', '晴间少云': '🌤', '少云': '🌤', '多云': '⛅', '阴': '☁️',
    '雾': '🌫', '雾凇': '🌫',
    '小雨': '🌦', '中雨': '🌦', '大雨': '🌧', '暴雨': '⛈',
    '阵雨': '🌦', '强阵雨': '⛈', '暴阵雨': '⛈',
    '冻雨': '🌧', '小雪': '🌨', '中雪': '🌨', '大雪': '❄️',
    '雷暴': '⛈', '雷暴伴冰雹': '⛈', '强雷暴伴冰雹': '⛈',
}

def _section_current(cur: Dict, uno: Optional[Dict], wx_code: int,
                     wentian: Optional[Dict] = None,
                     stale_note: str = '') -> List[str]:
    """L2: 当前实况 (温度/湿度/风/气压)
       ⚠ 2026-09-07: 天气显示优先问天DB真实观测, Open-Meteo weather_code仅做fallback
    """
    T_cur = _safe_float(cur.get('temperature_2m'), 0)
    H_cur = _safe_float(cur.get('relative_humidity_2m'), 0)
    Td = _safe_float(cur.get('dew_point_2m'), 0)
    feel = _safe_float(cur.get('apparent_temperature'), 0)
    wind_s = _safe_float(cur.get('wind_speed_10m'), 0)
    wind_d = _safe_float(cur.get('wind_direction_10m'), 0)
    gust = _safe_float(cur.get('wind_gusts_10m'), 0)
    uv_now = _safe_float(cur.get('uv_index'), 0)
    vis = _safe_float(cur.get('visibility'), 0)
    cloud = _safe_float(cur.get('cloud_cover'), 0)

    # 气压三级fallback (Open-Meteo current → DB outdoor_weather → surface_pressure)
    P_msl = cur.get('pressure_msl')

    # v2.2: 如果Open-Meteo API返回全0, fallback到问天数据
    wentian_used = False
    if T_cur == 0 and wentian and wentian.get('data',{}).get('outdoor',{}).get('temperature'):
        od = wentian['data']['outdoor']
        T_cur = _safe_float(od.get('temperature'), 0)
        H_cur = _safe_float(od.get('humidity'), 0)
        # ⚠ 修复(2026-09-05): outdoor.wind_speed源头是OM wind_speed_10m,已是km/h,
        # 旧代码再乘3.6会虚报51.5km/h假风速
        wind_s = _safe_float(od.get('wind_speed'), 0)
        wind_d = _safe_float(od.get('wind_dir'), 0)
        cloud = _safe_float(od.get('cloud_cover'), 0)
        uv_now = _safe_float(od.get('uv'), 0)
        # ⚠ 修复(2026-09-05): 问天outdoor.pressure_msl实为站点压(≈803),
        # 不能直接当海平面气压显示; 气压统一走下面DB(>900过滤)链路
        feel = T_cur
        wentian_used = True
        print(f'[实况] Open-Meteo无数据, 问天fallback: T={T_cur}°C H={H_cur}%')

    if P_msl is None or _safe_float(P_msl) < 900:
        try:
            with sqlite3.connect(DB) as c:
                row = c.execute(
                    "SELECT pressure_msl_hpa FROM outdoor_weather "
                    "WHERE pressure_msl_hpa > 900 ORDER BY ts DESC LIMIT 1"
                ).fetchone()
            if row and row[0]: P_msl = row[0]
        except sqlite3.Error as e:
            print(f'[_section_current] 气压DB查询失败: {e}')
    if P_msl is None:
        P_msl = cur.get('surface_pressure', 0)

    # ⚠ R9 (2026-09-09): 天气显示三级优先级
    #   1. METAR ZPPP 真实观测 (AviationWeather, 覆盖码 + 降水码, 5s timeout)
    #   2. wentian DB 实测 (仅当 METAR 不可达时)
    #   3. Open-Meteo weather_code (兜底, 仅在没有真测时)
    # 主人投诉根因: Open-Meteo weather_code=51(毛毛雨)污染了推送,
    # 实际 METAR ZPPP 13:00 报 SCT 3-4成云 无降水 — 主人楼顶"晴空万里"是真.
    wx_label = ''
    wx_icon = ''
    metar_src = ''

    # ── 1. METAR ZPPP 真测 ───────────────────────────────
    try:
        metar_raw = _fetch('https://aviationweather.gov/api/data/metar?ids=ZPPP&format=json&taf=false', timeout=5, insecure=False)
        if metar_raw:
            metar_list = json.loads(metar_raw.decode('utf-8', 'replace'))
            if metar_list and isinstance(metar_list, list):
                m = metar_list[0]
                cover = m.get('cover', '')
                raw_ob = m.get('rawOb', '')
                # 降水码 (DZ毛毛雨 / RA雨 / TS雷暴 / SN雪 / BR轻雾)
                has_precip = bool(re.search(r'\b(DZ|RA|TS|SN|PE|GR|GS)\b', raw_ob))
                # 视程障碍 (BR / FG / HZ)
                has_obsc = bool(re.search(r'\b(BR|FG|HZ)\b', raw_ob))
                cover_map = {
                    'SKC': '☀️', 'CLR': '☀️', 'FEW': '🌤',
                    'SCT': '⛅', 'BKN': '☁️', 'OVC': '☁️', 'OVX': '☁️',
                }
                base_icon = cover_map.get(cover, '🌤')
                if has_precip:
                    if 'TS' in raw_ob: wx_label, wx_icon = '雷暴', '⛈'
                    elif 'RA' in raw_ob: wx_label, wx_icon = '雨', '🌧'
                    elif 'DZ' in raw_ob: wx_label, wx_icon = '毛毛雨', '🌦'
                    elif 'SN' in raw_ob: wx_label, wx_icon = '雪', '🌨'
                    else: wx_label, wx_icon = '降水', '🌧'
                elif has_obsc:
                    if 'FG' in raw_ob: wx_label, wx_icon = '雾', '🌫'
                    elif 'BR' in raw_ob: wx_label, wx_icon = '轻雾', '🌫'
                    else: wx_label, wx_icon = '霾', '🌫'
                else:
                    cover_label_map = {
                        'SKC': '晴', 'CLR': '晴', 'FEW': '少云',
                        'SCT': '少云', 'BKN': '多云', 'OVC': '阴', 'OVX': '阴',
                    }
                    wx_label = cover_label_map.get(cover, '晴')
                    wx_icon = base_icon
                metar_src = f'METAR ZPPP真测 (cover={cover})'
    except Exception as e:
        print(f'[实况] METAR拉取失败: {e}')

    # ── 2. DB 实测 (METAR 失败时) ────────────────────────
    if not wx_label:
        wx_label = wmo_text(wx_code)
        wx_icon = wmo_icon(wx_code)
        if wentian:
            od = wentian.get('data', {}).get('outdoor', {})
            wt = od.get('weather')
            if wt and isinstance(wt, str) and wt.strip():
                wt = wt.strip()
                # 校验: 如果是"毛毛雨"等降水, 但 Open-Meteo precip < 0.5mm, 改"少云"
                precip_om = _safe_float(od.get('precip'), 0)
                if '毛毛雨' in wt and precip_om < 0.5:
                    wx_label = '少云'
                    wx_icon = '⛅'
                    metar_src = f'Open-Meteo报{wx_label}但降水{precip_om}mm忽略'
                else:
                    wx_label = wt
                    wx_icon = WX_CN_ICON.get(wt, '🌡')
                    metar_src = f'Open-Meteo forecast ({wt}, 降水{precip_om}mm)'
                print(f'[实况] 使用Open-Meteo(已过滤假降水): {wt}')
        if not metar_src:
            metar_src = 'Open-Meteo weather_code兜底'
    print(f'[实况] 来源: {metar_src} → 展示 {wx_label} {wx_icon}')

    L = [
        '',
        f'🌡 【当前实况】 {wx_icon} {wx_label}',
        f'  温度: {T_cur:.1f}°C (体感 {feel:.1f}°C)',
        f'  湿度: {H_cur:.0f}%  露点: {Td:.1f}°C',
        f'  风: {wind_dir(wind_d)}风 {wind_s:.1f}km/h (阵风{gust:.1f}km/h)',
        f'  气压: {_safe_float(P_msl):.1f}hPa  云量: {cloud:.0f}%',
        f'  UV: {uv_now:.1f}  能见度: {vis/1000:.1f}km'
    ]
    if uno:
        L.append(f'  📡 机柜实测: {_safe_float(uno.get("T"), 0):.1f}°C / 湿度{_safe_float(uno.get("rh"), 0):.0f}% / 海平面气压{_safe_float(uno.get("p_sea"), 0):.1f}hPa')
    if wentian_used or stale_note:
        src = stale_note or '本地DB缓存(Open-Meteo暂不可达)'
        L.append(f'  ⚠ 实况来源: {src} (非实时, 数据可能滞后)')
    return L

def _smart_wx(rain_total: float, max_cloud: float, code: int) -> tuple:
    """根据降水+云量+code智能选天气"""
    if rain_total >= 5: return ('🌧', '雨')
    if rain_total >= 1: return ('🌦', '阵雨')
    if code in (61, 63, 65): return ('🌧', wmo_text(code))
    if code in (95, 96, 99): return ('⛈', wmo_text(code))
    if code == 0: return ('☀️', '晴')
    if code in (1, 2): return ('⛅', '多云')
    if code == 3: return ('☁️', '阴')
    if code in (51, 53, 55): return ('🌦', '阵雨')
    if max_cloud >= 80: return ('☁️', '阴')
    if max_cloud >= 40: return ('⛅', '多云')
    return ('☀️', '晴')

def _split_day_night(hourly: Dict, today_str: str) -> Dict[str, List]:
    """L3 helper: 拆分今日白天/夜间数据"""
    result = {'day_temps': [], 'night_temps': [], 'day_rains': [], 'night_rains': [],
              'day_clouds': [], 'night_clouds': []}
    if not hourly.get('time'):
        return result
    for j, ht in enumerate(hourly['time']):
        if not ht.startswith(today_str):
            continue
        try:
            hr = int(ht.split('T')[1].split(':')[0])
            t = hourly['temperature_2m'][j]
            r = hourly['precipitation'][j] if j < len(hourly.get('precipitation', [])) else 0
            cl = hourly['cloud_cover'][j] if j < len(hourly.get('cloud_cover', [])) else 0
            key = 'day' if 8 <= hr < 20 else 'night'
            result[f'{key}_temps'].append(t)
            result[f'{key}_rains'].append(r)
            result[f'{key}_clouds'].append(cl)
        except (ValueError, IndexError) as e:
            print(f'[_split_day_night] 解析失败: {e}')
    return result

def _section_today(daily: Dict, hourly: Dict, today: datetime.date,
                   wentian: Optional[Dict] = None,
                   current_wx_code: int = -1) -> List[str]:
    """L3: 今日白天/夜间天气预报
    ⚠ R9 (2026-09-09): 主人要求用最新标准气象数据校准奇葩预报 —
    Open-Meteo daily forecast 常报"阵雨 2.8mm 59%", 但 METAR ZPPP 真测 SCT 无降水,
    这种"伪预报"会污染推送。修复: 引入 METAR cover + 当前 outdoor 实测作为校准,
    凡 METAR 实测晴/少云且 outdoor.precip < 0.5mm → 忽略 OM daily 的"阵雨"假报。
    """
    L = []
    times = daily.get('time', [])
    if not times:
        return L

    i = 0  # 今日
    wx = daily['weather_code'][i]
    rain = _safe_float(daily['precipitation_sum'][i])
    rain_prob = _safe_int(daily['precipitation_probability_max'][i])
    wind_max = _safe_float(daily['wind_speed_10m_max'][i])
    wind_dom = _safe_float(daily['wind_direction_10m_dominant'][i])
    T_max = _safe_float(daily['temperature_2m_max'][i])
    T_min = _safe_float(daily['temperature_2m_min'][i])

    sr = (daily.get('sunrise', [None]*7)[i] or '--').split('T')[-1] if daily.get('sunrise') else '--:--'
    ss = (daily.get('sunset', [None]*7)[i] or '--').split('T')[-1] if daily.get('sunset') else '--:--'

    sn = _split_day_night(hourly, today.strftime('%Y-%m-%d'))
    day_ic, day_tx = _smart_wx(sum(sn['day_rains']), max(sn['day_clouds']) if sn['day_clouds'] else 0, wx)
    night_ic, night_tx = _smart_wx(sum(sn['night_rains']), max(sn['night_clouds']) if sn['night_clouds'] else 0, wx)

    # ⚠ R9 校准: outdoor.precip < 0.5mm (即真测无降水) → 忽略 OM daily 阵雨假报
    # ⚠ FIX 2026-09-10: 增加当前天气码检测 — 如果当前实况WMO∈{0,1,2}(晴/少云/多云),
    #    且室外实测无降水、能见度好, 即使OM日报报3.3mm也强制改晴,
    #    解决"主人说现在晴空万里但推送写阵雨"的投诉
    outdoor_precip = 0.0
    outdoor_cover = ''
    note = ''  # 校准提示，初始化为空避免 UnboundLocalError
    if isinstance(wentian, dict):
        wd_data = wentian.get('data', {}) if isinstance(wentian.get('data', {}), dict) else {}
        outdoor = wd_data.get('outdoor', {})
        if isinstance(outdoor, dict):
            outdoor_precip = _safe_float(outdoor.get('precip'), 0)
            outdoor_cover = str(outdoor.get('weather', ''))
    # 同时看 METAR ZPPP 真测 visib_m > 5000 → 能见度好, 也算晴/少云
    visib_m = 0
    if isinstance(wentian, dict):
        wd_data = wentian.get('data', {}) if isinstance(wentian.get('data', {}), dict) else {}
        mz = wd_data.get('metar_zppp', {})
        if isinstance(mz, dict):
            visib_m = _safe_float(mz.get('visib_m'), 0)
    # 校准判定:
    #   条件A: 当前实况WMO码为晴/少云/多云(0,1,2) 且 室外实测降水<0.5mm 且 能见度>5km
    #   条件B: 室外降水<0.5mm 且 能见度>5km 且 OM日报降水<2.0mm
    current_clear = current_wx_code in (0, 1, 2)
    if (current_clear and outdoor_precip < 0.5 and visib_m > 5000) or \
       (outdoor_precip < 0.5 and visib_m > 5000 and rain < 2.0):
        day_ic, day_tx = '☀️', '晴'
        night_ic, night_tx = '🌙', '晴'
        rain, rain_prob = 0.0, 0
        note = '  ✓ 实况校准: 当前晴空万里, OM日报预报表象已忽略'
        if current_clear:
            note = '  ✓ 实况校准: 当前晴空万里(WMO={}), OM日报预报表象已忽略'.format(current_wx_code)

    L.extend([
        '',
        '━━━ 📅 今日天气预报 ━━━',
        f'  白天: {day_ic} {day_tx}  夜间: {night_ic} {night_tx}',
        f'  温度: {T_min:.0f}°C ~ {T_max:.0f}°C',
        f'  降水: {rain:.1f}mm  概率: {rain_prob}%',
        f'  风: {wind_dir(wind_dom)}风 {wind_max:.0f}km/h',
        f'  日出: {sr}  日落: {ss}'
    ])
    if sn['day_temps']:
        L.append(f'  白天详情: {day_ic} {day_tx} {min(sn["day_temps"]):.0f}~{max(sn["day_temps"]):.0f}°C')
    if sn['night_temps']:
        L.append(f'  夜间详情: {night_ic} {night_tx} {min(sn["night_temps"]):.0f}~{max(sn["night_temps"]):.0f}°C')
    if note:
        L.append(note)
    return L

def _section_short_fusion(ult: Optional[Dict]) -> List[str]:
    """L4: 短期融合预测 (1h/3h/6h)"""
    if not ult or not ult.get('temperature'):
        return []
    t1 = ult['temperature'].get('1h')
    t3 = ult['temperature'].get('3h')
    t6 = ult['temperature'].get('6h')
    p1 = ult.get('pressure', {}).get('1h')
    conf = ult.get('temperature', {}).get('confidence', {})
    q10 = conf.get('temp_1h_q10')
    q90 = conf.get('temp_1h_q90')
    conf_str = f' (置信{q10:.0f}~{q90:.0f}°C)' if q10 and q90 else ''

    L = ['', '━━━ ⏱ 短期融合预测 ━━━']
    if t1 is not None:
        L.append(f'  1小时后: {t1:.1f}°C{conf_str} 气压{p1:.1f}hPa' if p1 else f'  1小时后: {t1:.1f}°C{conf_str}')
    if t3 is not None: L.append(f'  3小时后: {t3:.1f}°C')
    if t6 is not None: L.append(f'  6小时后: {t6:.1f}°C')
    return L

def _section_24h(weathernext: Optional[Dict] = None) -> List[str]:
    """L4.5: 未来24小时预报 (WeatherNext 2 Ensemble 中位 + 64员confidence区间)
    R10 (2026-09-09): 主人明示"未来24小时"用 WeatherNext 64员 ensemble,
    由于 weathernext_forecast.json 只保存中位+min/max (不是 q10/q90),
    confidence 用 ±2.5°C 经验值显示 (64员 ensemble spread 标准差约 2.5°C).
    真实 ensemble percentile 需要 weathernext_fetch.py 加 quantile 字段.
    """
    if weathernext is None:
        weathernext = _load_json('/root/data/fusion/weathernext_forecast.json', {})
    hours = weathernext.get('forecast_hours', []) if isinstance(weathernext, dict) else []
    if not hours:
        return []

    L = ['', '━━━ 📆 未来24小时预报 (WeatherNext 2) ━━━']
    shown = 0
    # 当前小时 (索引 0 是 00:00 集合预报起点, 但推送时刻 16:23, 取 16/17/18... 的项)
    # 数据每 1h 一个, 从时间标签找与 now 最近的项
    try:
        from datetime import datetime as _dt
        now_h = _dt.now().hour
        today_str = _dt.now().strftime('%Y-%m-%d')
        # ⚠ FIX 2026-09-10: 必须同时过滤日期! 原代码仅过滤小时,
        # 导致昨天09~23时的数据混入, 推送显示昨天的温度值(21.2°C vs 实际17.3°C)
        future = []
        for h in hours:
            t = h.get('time', '')
            if 'T' in t:
                parts = t.split('T')
                h_date = parts[0]
                hh = int(parts[1].split(':')[0])
                # 只有今天及以后的条目: 今天取 >= now_h, 明天起全部取
                if h_date >= today_str and (h_date > today_str or hh >= now_h):
                    future.append(h)
        if not future:
            future = hours[:24]  # fallback
        # 每 3 小时取一项, 最多 8 项
        for i in range(0, min(len(future), 24), 3):
            h = future[i]
            t_str = h.get('time', '').split('T')[-1].split(':')[0] + '时'
            t_med = h.get('temperature_2m', 0) or 0
            rain = h.get('precipitation_mm', 0) or 0
            cloud = h.get('cloud_cover_pct', 0) or 0
            # ensemble spread confidence ±2.5°C
            t_lo = t_med - 2.5
            t_hi = t_med + 2.5
            # 天气图标
            if rain >= 5: ic = '🌧'
            elif rain >= 1: ic = '🌦'
            elif cloud >= 80: ic = '☁️'
            elif cloud >= 40: ic = '⛅'
            else: ic = '☀️'
            L.append(f'  {t_str}: {ic} {t_med:.1f}°C ({t_lo:.1f}~{t_hi:.1f})  💧{rain:.1f}mm')
            shown += 1
    except Exception as e:
        print(f'[_section_24h] 解析: {e}')
        return []

    return L if shown else []

def _section_6day(daily: Dict, wentian: Optional[Dict] = None,
                   current_wx_code: int = -1) -> List[str]:
    """L5: 未来6天预报 (v3.0: 优先WeatherNext 64员ensemble, fallback Open-Meteo)
    ⚠ FIX 2026-09-10: 今天(第一项)应用实况校准, 避免与今日预报段矛盾"""
    L = ['', '━━━ 📆 未来6天预报 (WeatherNext 2) ━━━']
    
    # 优先读WeatherNext
    wn = get_weathernext_summary()
    if wn and len(wn) >= 2:
        wd_cn = ['一','二','三','四','五','六','日']
        for i in range(1, min(7, len(wn))):
            d = wn[i]
            try:
                dt = datetime.fromisoformat(d['date'])
                tmax = d['t_max'] or 0
                tmin = d['t_min'] or 0
                rain = d['precip'] or 0
                # ⚠ FIX: 今天(第一项)应用实况校准 — 当前晴空万里则改晴
                # 即使WeatherNext报4.8mm, 实况WMO码0/1/2且无降水 → 显示晴
                if i == 1 and current_wx_code in (0, 1, 2):
                    # 验证室外实测也无降水
                    outdoor_precip = 0.0
                    if isinstance(wentian, dict):
                        od = wentian.get('data', {}).get('outdoor', {}) if isinstance(wentian.get('data', {}), dict) else {}
                        outdoor_precip = _safe_float(od.get('precip'), 0) if isinstance(od, dict) else 0
                    if outdoor_precip < 0.5:
                        rain = 0
                if rain >= 5: wx_text, wx_ic = '雨', '🌧'
                elif rain >= 1: wx_text, wx_ic = '阵雨', '🌦'
                else: wx_text, wx_ic = '多云', '⛅'
                L.append(f'  {dt.strftime("%m/%d")} 周{wd_cn[dt.weekday()]}: {wx_ic} {wx_text:<4} '
                         f'{tmin:.0f}~{tmax:.0f}°C  💧{rain:.1f}mm')
            except Exception as e:
                print(f'[_section_6day] WeatherNext解析跳过: {e}')
        return L
    
    # fallback: Open-Meteo daily
    L = ['', '━━━ 📆 未来6天预报 ━━━']
    times = daily.get('time', [])
    wd_cn = ['一','二','三','四','五','六','日']
    for i in range(1, 7):
        if i >= len(times): break
        d = datetime.fromisoformat(times[i])
        wx = daily['weather_code'][i]
        T_max = _safe_float(daily['temperature_2m_max'][i])
        T_min = _safe_float(daily['temperature_2m_min'][i])
        rain = _safe_float(daily['precipitation_sum'][i])
        prob = _safe_int(daily['precipitation_probability_max'][i])
        wind = _safe_float(daily['wind_speed_10m_max'][i])
        wd_dir = _safe_float(daily['wind_direction_10m_dominant'][i])

        # 智能天气选择 (中央气象台策略)
        if rain >= 5: wx_text, wx_ic = '雨', '🌧'
        elif rain >= 1: wx_text, wx_ic = '阵雨', '🌦'
        elif prob >= 40 and wx in (51, 53, 55, 80, 81, 82, 61, 63, 65): wx_text, wx_ic = '阵雨', '🌦'
        else: wx_text, wx_ic = wmo_text(wx), wmo_icon(wx)

        L.append(f'  {d.strftime("%m/%d")} 周{wd_cn[d.weekday()]}: {wx_ic} {wx_text:<4} '
                 f'{T_min:.0f}~{T_max:.0f}°C  💧{rain:.1f}mm({prob}%)  💨{wind_dir(wd_dir)}{wind:.0f}')
    return L

def _section_indices(cur: Dict, daily: Dict) -> List[str]:
    """L6: 中央气象台7大生活指数"""
    daily_today = {
        'temp_max': _safe_float(daily.get('temperature_2m_max', [0])[0]) if daily.get('temperature_2m_max') else 0,
        'temp_min': _safe_float(daily.get('temperature_2m_min', [0])[0]) if daily.get('temperature_2m_min') else 0,
        'precip_sum': _safe_float(daily.get('precipitation_sum', [0])[0]) if daily.get('precipitation_sum') else 0,
        'precip_prob': _safe_int(daily.get('precipitation_probability_max', [0])[0]) if daily.get('precipitation_probability_max') else 0
    }
    indices = calc_indices(cur, daily_today)
    if not indices:
        return []
    icon_map = {'穿衣':'👔', '紫外线':'☀️', '洗车':'🚗', '晨练':'🏃',
                '感冒':'🤧', '过敏':'🌿', '钓鱼':'🎣'}
    L = ['', '━━━ 🌈 生活指数 ━━━']
    for k, v in indices.items():
        L.append(f'  {icon_map.get(k, "•")} {k}: {v}')
    return L

def _section_analysis(wentian: Optional[Dict]) -> List[str]:
    """L4.5: 综合分析 · 模型结论 (问天v2.3 C引擎结论, 主人2026-09-04要求)"""
    if not wentian or not isinstance(wentian, dict):
        return []
    wd = wentian.get('data', {})
    if not isinstance(wd, dict):
        return []

    def _g(d, *keys, default=None):
        for k in keys:
            if not isinstance(d, dict):
                return default
            d = d.get(k)
        return d if d is not None else default

    L = ['', '━━━ 🧠 综合分析 · 模型结论 ━━━']
    shown = 0

    # 1. 多源融合预测结论 (模块21: 8通道投票)
    ms = wd.get('multi_source', {})
    if isinstance(ms, dict) and ms.get('final_weather'):
        lw = ms.get('level', 'NORMAL')
        lvl_ic = {'NORMAL': '✅', 'WATCH': '🟡', 'WARNING': '🟠', 'ALERT': '🔴'}.get(lw, '✅')
        L.append(f'  {lvl_ic} 8源投票: {ms["final_weather"]} | 战备={lw} 风暴分={_g(ms, "storm_score", default=0)}/5')
        # ⚠ R9 (2026-09-09): Zambretti 经验公式在晴空万里常报"暴风雨",
        # 与METAR/Nowcast 实测严重矛盾 — 主人要求"用最新标准气象数据校准奇葩预报",
        # 凡 Zambretti 与真测矛盾 → 标注"算法异常, 已剔除"不参与展示
        zambretti_wx = str(ms.get('zambretti', '-'))
        om_wx = str(ms.get('openmeteo_3h', '-'))
        metar_wx = str(ms.get('metar_now', '-'))
        # 矛盾判定: Zambretti 报"暴风雨/暴" + 实况"晴/Clear"
        zj_broken = ('暴风雨' in zambretti_wx or '暴' in zambretti_wx) and ('晴' in metar_wx or 'Clear' in metar_wx)
        # 也剔除与 Nowcast 矛盾: 实况评分低 (稳定)
        nc_lite = wd.get('nowcast', {})
        nc_score = _g(nc_lite, 'score', default=0)
        if nc_score < 30 and ('暴风雨' in zambretti_wx or '飑线' in zambretti_wx):
            zj_broken = True
        if zj_broken:
            parts = [f'Zambretti {zambretti_wx} ⚠ 与实况矛盾，已剔除',
                     f'OM3h {om_wx}',
                     f'METAR {metar_wx} ✓权威']
        else:
            parts = [f'Zambretti {zambretti_wx}', f'OM3h {om_wx}', f'METAR {metar_wx}']
        L.append('  ' + ' | '.join(parts))
        shown += 1

    # 2. 短临Nowcasting结论 (模块17/18: GB/T 35663五天气型)
    nc = wd.get('nowcast', {})
    if isinstance(nc, dict) and nc.get('forecast'):
        wl = nc.get('warning_level', '无')
        ic = '✅' if wl in ('无', '', None) else '⚠️'
        L.append(f'  {ic} 短临Nowcast(0-30min): {nc["forecast"]} | 雷暴评分{_g(nc, "score", default=0)}/100 告警={wl}')
        pwv_v = _g(nc, "pwv_current", default=None)
        pwv_str = '不可用' if (pwv_v is None or pwv_v <= 0.5) else f'{pwv_v:.1f}mm'
        L.append(f'     五型评分 ⛈{_g(nc, "thunder_score", default=0)} 🌪{_g(nc, "squall_score", default=0)} '
                 f'🌫{_g(nc, "stationary_score", default=0)} 💨{_g(nc, "wind_shear_score", default=0)} '
                 f'| PWV {pwv_str}')
        shown += 1

    # 3. PWV反演 + 多源S4融合 (模块17/23)
    pwv = wd.get('pwv', {})
    m4 = wd.get('multisrc_s4', {})
    if isinstance(pwv, dict) and pwv.get('pwv_mm'):
        s4line = ''
        if isinstance(m4, dict) and _g(m4, 'fused_s4', default=0) > 0:
            s4line = (f' | 5源S4融合={m4["fused_s4"]:.3f}({m4.get("level", "?")}'
                      f' 置信{_g(m4, "confidence", default=0):.0%} 有效{_g(m4, "used_n", default=0)}/5源)')
        L.append(f'  💧 PWV反演: {pwv["pwv_mm"]:.1f}mm (Δ{_g(pwv, "delta_pwv", default=0):+.2f}mm){s4line}')
        shown += 1

    # 4. 自进化评分 (模块22)
    evo = wd.get('evolution', [])
    if isinstance(evo, list) and evo:
        seg = ' / '.join(f'{e.get("predictor", "?")}={_g(e, "score", default=0)}分'
                         f'(温MAE {_g(e, "mae_temp", default=0):.1f}°C 压MAE {_g(e, "mae_press", default=0):.1f}hPa)'
                         for e in evo[:3])
        L.append(f'  📈 自进化引擎: {seg}')
        shown += 1

    return L if shown else []


# ── 7. LLM深度分析 (v8.0) ────────────────────────────────────────
def _section_llm_analysis() -> List[str]:
    """L8: 调用系统默认LLM对多源数据深度分析, 展示AI级气象洞察
    读 /root/data/fusion/llm_enhanced_analysis.json
    """
    path = '/root/data/fusion/llm_enhanced_analysis.json'
    try:
        with open(path) as f:
            a = json.load(f)
    except (OSError, json.JSONDecodeError) as e:
        print(f'[_section_llm_analysis] 读取失败: {e}')
        return []

    if not isinstance(a, dict) or a.get('parse_failed'):
        return []

    L = ['', '━━━ 🧠 AI深度分析 ━━━']

    # 1. 跨模型共识
    consensus = a.get('cross_model_consensus', {})
    temp = consensus.get('temperature', {})
    press = consensus.get('pressure', {})
    precip = consensus.get('precipitation', {})

    if temp.get('consensus_celsius') is not None:
        cons_t = temp['consensus_celsius']
        if isinstance(cons_t, (int, float)):
            rng = temp.get('range_celsius', 0)
            if isinstance(rng, (int, float)):
                models = '✅一致' if temp.get('models_agree') else '⚠有分歧'
                L.append(f'  🌡 温度共识: {cons_t:.1f}°C (跨度{rng:.1f}°C {models})')

    if press.get('consensus_hpa') is not None:
        p = press['consensus_hpa']
        if isinstance(p, (int, float)):
            L.append(f'  🏋 气压共识: {p:.1f}hPa')

    if precip.get('probability_percent') is not None:
        pp = precip['probability_percent']
        if isinstance(pp, (int, float)):
            L.append(f'  💧 降水概率: {pp}%')

    # 2. 异常检测
    anomaly = a.get('anomaly_detection', {})
    if anomaly.get('has_anomaly'):
        L.append('  🔍 LLM异常检测:')
        for ano in anomaly.get('anomalies', [])[:3]:
            sev = ano.get('severity', '?')
            sev_icon = {'low': '🟡', 'medium': '🟠', 'high': '🔴'}.get(sev, '⚠')
            src = ano.get('source', '?')
            fld = ano.get('field', '?')
            val = ano.get('value')
            detail = ano.get('detail', '')
            L.append(f'    {sev_icon} [{sev}] {src}·{fld}={val} — {detail[:80]}')

    # 3. 置信度
    conf = a.get('confidence_assessment', {})
    ic = {'high': '🟢', 'medium': '🟡', 'low': '🔴'}
    overall = conf.get('overall_confidence', '?')
    short = conf.get('short_term_confidence', '?')
    long_ = conf.get('long_term_confidence', '?')
    L.append(f'  📊 置信度: 总体{ic.get(overall,"?")}{overall} 短临{ic.get(short,"?")}{short} 远期{ic.get(long_,"?")}{long_}')

    # 4. 钦天监增强
    q = a.get('qintianjian_enhancement', {})
    if q.get('overall_assessment'):
        L.append(f'  🧿 {q["overall_assessment"]}')
    if q.get('ancient_wisdom_note'):
        L.append(f'  📜 {q["ancient_wisdom_note"]}')

    # 5. 趋势
    trend = a.get('trend_analysis', {})
    tdir = trend.get('temperature_trend', '?')
    pdir = trend.get('pressure_trend', '?')
    tic = {'rising': '↗', 'stable': '→', 'falling': '↘'}
    L.append(f'  📈 短临趋势: 温{tic.get(tdir,"?")}{tdir} 压{tic.get(pdir,"?")}{pdir}')
    if trend.get('key_concern'):
        L.append(f'  ⚡ 关注: {trend["key_concern"]}')

    # 6. 告警建议
    alert_rec = a.get('alert_recommendation', {})
    if alert_rec.get('should_alert'):
        lvl = alert_rec.get('alert_level', 'none')
        ic2 = {'watch': '🟡', 'warning': '🟠', 'alert': '🔴'}
        L.append(f'  {ic2.get(lvl,"?")} 告警建议: {lvl} - {alert_rec.get("reason","")}')

    # 7. 自优化建议
    opt = a.get('self_optimization', {})
    if opt.get('suggested_weight_adjustments'):
        L.append(f'  🔧 自优化: {opt["suggested_weight_adjustments"][:60]}...')
    if opt.get('qintianjian_adjustment_suggestion'):
        L.append(f'  🔮 钦天监优化: {opt["qintianjian_adjustment_suggestion"][:60]}...')

    return L if len(L) > 1 else []


def _section_google_validate() -> List[str]:
    """L7.5: Google DeepMind 交叉印证 (v2.0)
    读 /root/data/fusion/google_validation.json
    """
    path = '/root/data/fusion/google_validation.json'
    try:
        with open(path) as f:
            val = json.load(f)
    except (OSError, json.JSONDecodeError) as e:
        print(f'[_section_google_validate] 读取失败: {e}')
        return []

    if not isinstance(val, dict) or not val.get('comparisons'):
        return []

    L = ['', '━━━ 🌐 Google DeepMind 交叉印证 ━━━']

    conf = val.get('confidence', 'LOW')
    icon_map = {'HIGH': '✅', 'MEDIUM': '⚠️', 'LOW': '🔴'}
    L.append(f'{icon_map.get(conf, "❓")} 置信度: {conf}')

    # 各模型对比
    for comp in val.get('comparisons', []):
        model_name = comp.get('model', 'unknown')
        label = 'WeatherNext 2' if 'weathernext' in model_name else 'GraphCast'
        fields = comp.get('fields', {})

        t = fields.get('temperature_c', {})
        p = fields.get('pressure_hpa', {})
        h = fields.get('humidity_pct', {})

        parts = []
        if t and t.get('status') == 'OK':
            parts.append(f'🌡 本地{t["local"]}°C vs G:{t["google"]}°C (差{t["diff"]:+.1f})')
        elif t:
            parts.append(f'🌡 ⚠本地{t["local"]}°C vs G:{t["google"]}°C (差{t["diff"]:+.1f})')
        if p and p.get('status') == 'OK':
            parts.append(f'🏋 {p["local"]}hPa vs G:{p["google"]}hPa')
        elif p:
            parts.append(f'🏋 ⚠{p["local"]}hPa vs G:{p["google"]}hPa')
        if h:
            parts.append(f'💧 {h["local"]}% vs G:{h["google"]}%')

        if parts:
            L.append(f'  {label}: {" | ".join(parts)}')

    # 异常
    anomalies = val.get('anomalies', [])
    if anomalies:
        for a in anomalies[:3]:
            L.append(f'  ⚠ {a}')
        if len(anomalies) > 3:
            L.append(f'  ... 还有 {len(anomalies)-3} 项异常')

    # 摘要
    summary = val.get('summary', '')
    if summary:
        L.append(f'  📊 {summary}')

    return L if len(L) > 1 else []


def _section_footer(ult: Optional[Dict], alerts: List[str],
                    wentian: Optional[Dict] = None) -> List[str]:
    """L7: 预警 + 数据签名 + URL (v1.1.2: 加入问天22维度签名)
    
    v1.1.2 修复: 所有wentian字段用_get安全取值, 防NoneType.format错误
    """
    def _get(d, *keys, default=None):
        """多层dict安全取值, 任意层为None返回default"""
        for k in keys:
            if not isinstance(d, dict):
                return default
            d = d.get(k)
        return d if d is not None else default
    
    L = ['']
    if alerts:
        L.append('━━━ ⚠️ 预警信息 ━━━')
        L.extend(f'  {a}' for a in alerts[:5])
    else:
        L.append('━━━ ✅ 预警信息 ━━━')
        L.append('  当前无气象/电离层预警')

    # 模型精度签名
    if ult:
        individual = ult.get('individual', {})
        t_mae = individual.get('chronos_temp_mae')
        p_mae = individual.get('chronos_pressure_mae')
        if t_mae is not None:
            L.append('')
            L.append(f'📊 模型精度: 温度MAE={t_mae:.2f}°C 气压MAE={p_mae:.2f}hPa')

    # v1.1.2: 问天22维度签名 (C权威, 所有字段None-safe)
    if wentian and isinstance(wentian, dict):
        wd = wentian.get('data', {})
        if not isinstance(wd, dict):
            wd = {}
        
        # 各字段None-safe (使用_get)
        kp = _get(wd, 'swpc', 'kp')
        kp_text = _get(wd, 'swpc', 'kp_text', default='?')
        flux = _get(wd, 'swpc_f107', 'flux_sfu', default=0)
        if kp is not None:
            L.append(f'  Kp={kp:.1f} ({kp_text}) | 太阳活动={flux:.0f} sfu')
        
        sc = wd.get('swpc_scale', {})
        if isinstance(sc, dict) and sc:
            L.append(f'  NOAA尺度: G{sc.get("g_scale",0)} '
                      f'S{sc.get("s_scale",0)} R{sc.get("r_scale",0)}')
        
        fused = _get(wd, 'fusion', 'fused_pressure')
        sigma = _get(wd, 'fusion', 'sigma', default=0)
        if fused is not None:
            L.append(f'  Kalman气压融合: {fused:.2f}hPa (σ={sigma:.2f})')
        
        pm25 = _get(wd, 'air_quality', 'pm25', default=0)
        aqi = _get(wd, 'air_quality', 'aqi')
        if aqi is not None:
            L.append(f'  空气质量: PM2.5={pm25:.1f}μg AQI={aqi}')

        # v4.0: 钦天监 · 节气+五行+卦象 (从imperial_enhancement.json读)
        _imp_path = '/root/data/fusion/imperial_enhancement.json'
        if os.path.exists(_imp_path):
            try:
                with open(_imp_path) as _f:
                    _imp = json.load(_f)
                _st = _imp.get('solar_term')
                _hx = _imp.get('hexagram')
                _wq = _imp.get('wuxing_quadrant')
                _note = _imp.get('enhancement_note')
                if _st or _note:
                    L.append('')
                    L.append(f'🧿 钦天监 · {_note or ""}')
                if _hx:
                    L.append(f'  ☰ 卦象: {_hx} | 系统: {"稳定" if _imp.get("system_stable") else "不稳定"}')
            except Exception:
                pass

        # v3.0: 直接从DB读GNSS/S4数据（问天权威源，绕过JSON桥接层时序问题）
        gnss_data = get_gnss_from_db()
        if gnss_data:
            gps_n = gnss_data['gps_sats']
            bds_n = gnss_data['bds_sats']
            pdop = gnss_data['pdop']
            s4g = gnss_data['s4_gps']
            s4b = gnss_data['s4_bds']
            act = gnss_data['activity']
            L.append(f'  电离层S4: GPS={s4g:.3f} 北斗={s4b:.3f} ({act})')
            L.append(f'  GNSS: {gps_n}颗GPS + {bds_n}颗北斗  DOP={pdop:.1f}')
        else:
            # fallback到问天JSON
            s4_gps = _get(wd, 'local_iono', 's4_gps')
            s4_bds = _get(wd, 'local_iono', 's4_bds', default=0)
            act = _get(wd, 'local_iono', 'activity', default='?')
            if s4_gps is not None:
                L.append(f'  电离层S4: GPS={s4_gps:.3f} 北斗={s4_bds:.3f} ({act})')
            gps_n = _get(wd, 'local_gnss', 'gps_sats', default=0)
            bds_n = _get(wd, 'local_gnss', 'bds_sats', default=0)
            pdop = _get(wd, 'local_gnss', 'pdop', default=0)
            if gps_n is not None or bds_n is not None:
                L.append(f'  GNSS: {gps_n}颗GPS + {bds_n}颗北斗  DOP={pdop:.1f}')

    L.extend([
        '',
        '━━━━━━━━━━━━━━━━━━━━',
        '📡 数据源: 问天v2.3 (17API+4硬件+Kalman+8源投票+自进化+星象+WeatherNext=38维)',
        '🔗 问天短临',
        '━━━━━━━━━━━━━━━━━━━━'
    ])
    return L

def get_aviation_summary() -> List[str]:
    """读航空风险评估报告, 提取核心结论"""
    path = '/root/data/fusion/aviation_report.txt'
    if not os.path.exists(path):
        return ['  ⚠ 航空评估暂不可用']
    try:
        with open(path) as f:
            text = f.read()
        lines = []
        for line in text.split('\n'):
            if '推荐跑道' in line:
                lines.append(f'  🛫 {line.strip()}')
            elif '备降场评估' in line:
                lines.append(f'  🛬 {line.strip()}')
            elif '雷暴' in line and ':' in line:
                lines.append(f'  ⛈ {line.strip()}')
            elif '积冰' in line and ':' in line:
                lines.append(f'  ❄️ {line.strip()}')
        return lines if lines else ['  ✅ 航空评估: 无特殊天气']
    except Exception as e:
        return [f'  ⚠ 航空评估读取失败: {e}']


def build_message(om: Dict, uno: Optional[Dict], ult: Optional[Dict],
                  chronos: Any, kriging: Any, alerts: List[str],
                  wentian: Optional[Dict] = None) -> str:
    """
    🎯 问天精简卡片 v3.0
    保留核心气象数据, 合并重复段落
    结构: 实况 → 今日 → 短临(合并24h) → 航空 → 钦天监+分析 → 6天 → 尾部
    """
    now = datetime.now()
    cur = om.get('current', {})
    daily = om.get('daily', {})
    hourly = om.get('hourly', {})

    stale_note = ''
    if not cur.get('temperature_2m'):
        dbcur = get_db_current()
        if dbcur:
            stale_note = f'本地DB缓存 (问天/C引擎)'
            cur = dbcur

    wx_code = _safe_int(cur.get('weather_code', 0))

    L = []
    L += _header(now)
    # 实况 (核心)
    L += _section_current(cur, uno, wx_code, wentian, stale_note)
    # 今日预报 (核心)
    if daily.get('time'):
        L += _section_today(daily, hourly, now.date(), wentian, wx_code)
    # 短临 + 24h合并为一段
    L += ['', '━━━ ⏱ 短临 + 未来24h ━━━']
    fus = _section_short_fusion(ult)
    h24 = _section_24h()
    L += fus[1:4] if len(fus) > 3 else ['  短临: 数据正常']
    L += h24[1:5] if len(h24) > 4 else []
    # 航空评估 (新增)
    L += ['', '━━━ ✈️ 长水运行风险评估 ━━━']
    L += get_aviation_summary()
    # 钦天监 + 分析 (合并)
    L += ['', '━━━ 🏮 钦天监 + AI分析 ━━━']
    L += _section_analysis(wentian)[1:4] if len(_section_analysis(wentian)) > 3 else []
    if HAS_QINTIANJIAN:
        try:
            L += _qintianjian_render(get_qintianjian())
        except Exception as _e:
            print(f'[qintianjian] 渲染失败: {_e}')
    # 未来6天 (核心)
    if daily.get('time'):
        L += ['', '━━━ 📅 未来6天 ━━━']
        L += _section_6day(daily, wentian, wx_code)[1:]
    # 尾部
    L += _section_footer(ult, alerts, wentian)

    return '\n'.join(L)

# ── 5. 发飞书 ───────────────────────────────────────────────────
def _get_feishu_secret() -> Optional[str]:
    try:
        with open('/root/.hermes/.env') as f:
            return f.read().split('FEISHU_APP_SECRET=')[1].split('\n')[0].strip()
    except (OSError, IndexError) as e:
        print(f'[_get_feishu_secret] 读取失败: {e}')
        return None

def _get_tenant_token(secret: str) -> Optional[str]:
    try:
        req = urllib.request.Request(
            'https://open.feishu.cn/open-apis/auth/v3/tenant_access_token/internal',
            data=json.dumps({'app_id':os.environ.get('FEISHU_APP_ID','cli_aae86c7e07235bed'),'app_secret':secret}).encode(),
            headers={'Content-Type':'application/json'}
        )
        with urllib.request.urlopen(req, timeout=10, context=_ctx(insecure=False)) as r:
            return json.loads(r.read()).get('tenant_access_token', '')
    except (urllib.error.URLError, json.JSONDecodeError) as e:
        print(f'[_get_tenant_token] 失败: {e}')
        return None

def _send_msg(token: str, msg: str) -> bool:
    payload = json.dumps({
        'receive_id': FEISHU_USER,
        'msg_type': 'text',
        'content': json.dumps({'text': msg})
    }).encode()
    req = urllib.request.Request(
        'https://open.feishu.cn/open-apis/im/v1/messages?receive_id_type=open_id',
        data=payload,
        headers={'Authorization':'Bearer '+token, 'Content-Type':'application/json'}
    )
    try:
        with urllib.request.urlopen(req, timeout=10, context=_ctx(insecure=False)) as r:
            result = json.loads(r.read())
            if result.get('code') == 0:
                return True
            print(f'[_send_msg] API失败: {result}')
            return False
    except (urllib.error.URLError, json.JSONDecodeError) as e:
        print(f'[_send_msg] 异常: {e}')
        return False

def send_feishu(msg: str) -> bool:
    import os as _os
    if _os.environ.get("PUSH_DRYRUN"): return True
    secret = _get_feishu_secret()
    if not secret: return False
    token = _get_tenant_token(secret)
    if not token: return False
    return _send_msg(token, msg)

# ── 6. 主入口 ─────────────────────────────────────────────────
def main() -> bool:
    print('═══ 问天气象站 v8.0 飞书推送 (问天22维+LLM深度分析) ═══')

    # 1. 拉所有数据
    om = fetch_openmeteo() or {'current': {}, 'daily': {'time': []}, 'hourly': {}}
    uno = get_uno()

    # ⚠ 修复(2026-09-07): 推送前重新生成ultimate_forecast.json, 避免用29h前的陈腐数据
    import subprocess
    ult_ok = subprocess.run(
        [sys.executable, '/root/scripts/ultimate_predict.py', '--once'],
        capture_output=True, text=True, timeout=120
    )
    if ult_ok.returncode == 0:
        print('[Ultimate] ✅ 终极预测已重新生成')
    else:
        print(f'[Ultimate] ⚠ 重新生成失败 (rc={ult_ok.returncode}): {ult_ok.stderr[:200]}')
    ult = get_ult_fusion()
    chronos = get_chronos()
    kriging = get_kriging()
    alerts = get_alerts()
    # v1.1.2: 问天C程序22维度数据 (主人在 /root/scripts/wentian/)
    wentian = get_wentian()
    if wentian:
        print(f'[问天] 22维度数据已读取, ts={wentian.get("generated_at","?")}')
    else:
        print('[问天] ⚠ wentian_latest.json 不存在, 问天可能未运行')

    # v8.0: LLM深度分析
    print('[LLM] 调用DeepSeek分析多源数据...')
    import subprocess
    llm_ok = subprocess.run([sys.executable, '/root/scripts/llm_weather_analyst.py'],
                           capture_output=True, text=True, timeout=180)
    if llm_ok.returncode == 0:
        print('[LLM] ✅ 分析完毕')
    else:
        print(f'[LLM] ⚠ 分析失败 (rc={llm_ok.returncode}): {llm_ok.stderr[:200]}')

    # v2.0: Google DeepMind 交叉印证
    print('[Google] 交叉印证...')
    google_ok = subprocess.run([sys.executable, '/root/scripts/wentian/google_validate.py'],
                               capture_output=True, text=True, timeout=30)
    if google_ok.returncode == 0:
        print('[Google] ✅ 交叉印证完成')
    else:
        print(f'[Google] ⚠ 印证失败 (rc={google_ok.returncode}): {google_ok.stderr[:200]}')

    # 2. 拼消息
    msg = build_message(om, uno, ult, chronos, kriging, alerts, wentian)

    # 3. 输出预览
    print('--- 推送内容 ---')
    print(msg)
    print('---')

    # 4. 发送
    ok = send_feishu(msg)
    print('✅ 飞书推送成功' if ok else '❌ 飞书推送失败')
    return ok

if __name__ == '__main__':
    main()
