#!/usr/bin/env python3
"""
钦天监 v1.0 — 问天数据 × 中国传统文化融合引擎
==============================================
主人: 朱涛 BG8SBA, 昆明长水机场楼顶

核心思路:
  不是玄学占卜, 而是把中国传统文化中的时空体系
  与问天的 34 维科学数据进行结构化映射:

  1. 二十四节气: 太阳黄经 + 气象数据 → 节气状态量化
  2. 五行生克: 天气要素 → 五行映射 (气温→火, 湿度→水, 风→木, 气压→金, 地磁→土)
  3. 星象: GPS+BDS卫星数 + ISS位置 + 空间天气 → 天象日志
  4. 周易卦象: 八经卦 × 气象状态向量 → 当期卦象
  5. 气候异常预警: 当季数据偏离历史节气均值 → 标记异常

数据来源: 问天 26 张表, 34 维
输出: /root/data/fusion/imperial_observatory.json
      /root/data/fusion/lishu/ (历法日志目录)
"""

import json, os, math, sqlite3, time
from datetime import datetime, timedelta

WENTIAN_DB = '/root/data/wentian.db'
ANO_DB = '/root/data/ano_weather.db'
OUT_DIR = '/root/data/fusion'
LISHU_DIR = os.path.join(OUT_DIR, 'lishu')

os.makedirs(LISHU_DIR, exist_ok=True)

def sql(q, db):
    c = sqlite3.connect(db)
    try:
        r = c.execute(q).fetchone()
        return r if r else None
    except:
        return None
    finally:
        c.close()

# ═══════════════════════════════════════════════════════════
# 1. 二十四节气计算 (天文算法)
# ═══════════════════════════════════════════════════════════

def solar_term_index(sun_lon):
    """太阳黄经 → 节气索引 (0=立春, 23=大寒)"""
    terms = [
        '立春', '雨水', '惊蛰', '春分', '清明', '谷雨',
        '立夏', '小满', '芒种', '夏至', '小暑', '大暑',
        '立秋', '处暑', '白露', '秋分', '寒露', '霜降',
        '立冬', '小雪', '大雪', '冬至', '小寒', '大寒'
    ]
    for i, t in enumerate(terms):
        if sun_lon < i * 15 + 15:
            return t, i
    return '大寒', 23

def solar_term_from_date(dt):
    """近似太阳黄经 (基于日期, 0°=春分)"""
    doy = dt.timetuple().tm_yday
    sun_lon = (doy - 80) * 360.0 / 365.25
    while sun_lon < 0: sun_lon += 360
    while sun_lon >= 360: sun_lon -= 360
    return sun_lon

# ═══════════════════════════════════════════════════════════
# 2. 五行生克计算
# ═══════════════════════════════════════════════════════════

def wuxing_map(temp, humid, wind, press, kp):
    """天气数据 → 五行强度 (0-100)"""
    if temp is None: temp = 20
    if humid is None: humid = 50
    if wind is None: wind = 5
    if press is None: press = 1013
    if kp is None: kp = 0

    # 火: 气温偏离舒适基准(20°C) + Kp地磁
    fire = min(100, max(0, abs(temp - 20) * 4 + kp * 15))

    # 水: 湿度 + PWV (可降水量)
    water = min(100, max(0, humid * 0.8))

    # 木: 风速 + 天气变化率
    wood = min(100, max(0, wind * 6))

    # 金: 气压 + TEC
    press_dev = abs(press - 1013.25) * 2
    metal = min(100, max(0, press_dev))

    # 土: 综合稳定度 (低 = 稳定)
    earth = max(0, 100 - (fire + water + wood + metal) / 4)

    return {'fire': round(fire, 1), 'water': round(water, 1),
            'wood': round(wood, 1), 'metal': round(metal, 1),
            'earth': round(earth, 1)}

def wuxing_balance(wx):
    """五行平衡度 (0-100, 越高越均衡)"""
    vals = list(wx.values())
    avg = sum(vals) / len(vals)
    if avg == 0: return 100
    deviation = sum(abs(v - avg) for v in vals) / len(vals)
    return max(0, 100 - deviation * 1.5)

def wuxing_dominant(wx):
    """当前五行: 找出最旺和次旺"""
    cn = {'fire': '火', 'water': '水', 'wood': '木', 'metal': '金', 'earth': '土'}
    sorted_wx = sorted(wx.items(), key=lambda x: x[1], reverse=True)
    return {cn[k]: v for k, v in sorted_wx[:3]}

# ═══════════════════════════════════════════════════════════
# 3. 卦象映射 (八经卦 × 5维气象)
# ═══════════════════════════════════════════════════════════

BAGUA = {
    '乾': {'trigram': '☰', 'nature': '天', 'element': '金', 'direction': '西北'},
    '兑': {'trigram': '☱', 'nature': '泽', 'element': '金', 'direction': '西'},
    '离': {'trigram': '☲', 'nature': '火', 'element': '火', 'direction': '南'},
    '震': {'trigram': '☳', 'nature': '雷', 'element': '木', 'direction': '东'},
    '巽': {'trigram': '☴', 'nature': '风', 'element': '木', 'direction': '东南'},
    '坎': {'trigram': '☵', 'nature': '水', 'element': '水', 'direction': '北'},
    '艮': {'trigram': '☶', 'nature': '山', 'element': '土', 'direction': '东北'},
    '坤': {'trigram': '☷', 'nature': '地', 'element': '土', 'direction': '西南'},
}

def get_hexagram(wx):
    """五行强度 → 本卦 (五维→八卦)"""
    # 把五行映射到八卦方向
    mapping = {
        'fire': ('离', '乾'),
        'water': ('坎', '坤'),
        'wood': ('震', '巽'),
        'metal': ('兑', '乾'),
        'earth': ('坤', '艮'),
    }
    scores = {}
    for elem, (gua1, gua2) in mapping.items():
        val = wx.get(elem, 0)
        scores[gua1] = scores.get(gua1, 0) + val * 0.7
        scores[gua2] = scores.get(gua2, 0) + val * 0.3
    
    sorted_gua = sorted(scores.items(), key=lambda x: x[1], reverse=True)
    primary = sorted_gua[0][0] if sorted_gua else '坤'
    
    return {
        'primary': primary,
        'trigram': BAGUA[primary]['trigram'],
        'nature': BAGUA[primary]['nature'],
        'meaning': f'{primary}为{BAGUA[primary]["nature"]}',
        'score': round(sorted_gua[0][1], 1) if sorted_gua else 0
    }

# ═══════════════════════════════════════════════════════════
# 4. 二十四节气气候偏离
# ═══════════════════════════════════════════════════════════

def season_deviation(dt, temp, humid, press):
    """当前数据偏离历史节气均值 (模拟, 等积累>30天数据后自动校准)"""
    doy = dt.timetuple().tm_yday
    # 目前用经验值, 等积累数据后替换为真实统计
    return {
        'note': '数据积累不足30天, 无法计算节气偏离',
        'estimated_deviation': '等待数据积累'
    }

# ═══════════════════════════════════════════════════════════
# 5. 天象日志
# ═══════════════════════════════════════════════════════════

def celestial_log(gps, bds, kp, s4, tec_val):
    """星象日志"""
    items = []

    if gps and gps >= 12:
        items.append(f'北斗{int(bds or 0)}星可见, GPS{int(gps or 0)}星追迹')
    if kp and kp > 3:
        items.append(f'磁暴扰动(Kp={kp})——天道失常')
    if s4 and s4 > 0.3:
        items.append(f'电离层闪烁(S4={s4:.3f})——天有异象')
    if tec_val and tec_val > 20:
        items.append(f'电子总量异常(TEC={tec_val:.0f}TECU)——大气激荡')

    if not items:
        items.append('天象平和, 无异常')

    return items

# ═══════════════════════════════════════════════════════════
# 主入口
# ═══════════════════════════════════════════════════════════

def imperial_observatory_run():
    now = datetime.now()
    ts = int(time.time())

    # 获取问天数据
    row_o = sql("SELECT temperature, humidity, pressure_msl, wind_speed FROM outdoor ORDER BY ts DESC LIMIT 1", WENTIAN_DB)
    row_k = sql("SELECT noaa_kp_est FROM external_data ORDER BY ts DESC LIMIT 1", WENTIAN_DB)
    row_s = sql("SELECT s4_gps FROM local_iono ORDER BY ts DESC LIMIT 1", WENTIAN_DB)
    row_try = sql("SELECT fused_tec FROM multisrc_s4 ORDER BY ts DESC LIMIT 1", WENTIAN_DB)
    row_g = sql("SELECT ROUND(AVG(gps_sats),1) FROM gps_log WHERE datetime(ts)>=datetime('now','-1 day')", ANO_DB)
    row_b = sql("SELECT ROUND(AVG(bds_sats),1) FROM gps_log WHERE datetime(ts)>=datetime('now','-1 day')", ANO_DB)

    # 取值
    temp = row_o[0] if row_o else 20
    humid = row_o[1] if row_o else 50
    press = row_o[2] if row_o else 1013
    wind = row_o[3] if row_o else 5
    kp = row_k[0] if row_k else 0
    s4_val = row_s[0] if row_s else 0
    tec_val = row_try[0] if row_try else 8
    gps_avg = row_g[0] if row_g else 0
    bds_avg = row_b[0] if row_b else 0

    # 1. 节气
    sun_lon = solar_term_from_date(now)
    term_name, term_idx = solar_term_index(sun_lon)

    # 2. 五行
    wx = wuxing_map(temp, humid, wind, press, kp)
    balance = wuxing_balance(wx)
    dominant = wuxing_dominant(wx)

    # 3. 卦象
    gua = get_hexagram(wx)

    # 4. 天象
    logs = celestial_log(gps_avg, bds_avg, kp, s4_val, tec_val)

    # 5. 节气偏离
    deviation = season_deviation(now, temp, humid, press)

    # 构建输出
    output = {
        'ts': ts,
        'time': now.strftime('%Y-%m-%d %H:%M:%S'),
        'location': '昆明长水机场楼顶 (25.08°N, 102.91°E)',

        'solar_term': {
            'current': term_name,
            'index': term_idx,
            'sun_longitude_deg': round(sun_lon, 1),
            'note': f'时值{term_name}, 太阳黄经{round(sun_lon, 1)}°'
        },

        'wuxing': {
            'elements': wx,
            'balance_score': round(balance, 1),
            'dominant': dominant,
            'interpretation': f'五行{"均衡" if balance > 60 else "偏颇"}, 以{"".join(list(dominant.keys())[:2])}为旺'
        },

        'hexagram': gua,

        'celestial_watch': logs,

        'scientific_data': {
            'temperature_c': round(temp, 1),
            'humidity_pct': round(humid, 1),
            'pressure_hpa': round(press, 1),
            'wind_ms': round(wind, 1),
            'kp_index': kp,
            's4_scintillation': round(s4_val, 3),
            'tec_total_electron_content': round(tec_val, 1),
            'gps_satellites_24h_avg': gps_avg,
            'bds_satellites_24h_avg': bds_avg
        },

        'season_deviation': deviation,
        'daemon_version': '问天 v2.3 钦天监 v1.0'
    }

    # 写入 JSON
    with open(os.path.join(OUT_DIR, 'imperial_observatory.json'), 'w', encoding='utf-8') as f:
        json.dump(output, f, ensure_ascii=False, indent=2)

    # 历法日志 (每天一个文件)
    daily_log = os.path.join(LISHU_DIR, f'{now.strftime("%Y%m%d")}.json')
    existing = []
    if os.path.exists(daily_log):
        try:
            with open(daily_log, 'r') as f:
                existing = json.load(f)
        except:
            pass
    existing.append(output)
    with open(daily_log, 'w', encoding='utf-8') as f:
        json.dump(existing, f, ensure_ascii=False, indent=2)

    # 打印
    print(f'\n━━━ 钦天监 · 天文历法推演 ━━━')
    print(f'  🌞 时值{term_name} (太阳黄经{round(sun_lon, 1)}°)')
    print(f'  🔥 五行: {"|".join(f"{k}={v}" for k,v in wx.items())} | 平衡度={round(balance,1)}')
    print(f'  ☰ 卦象: {gua["trigram"]} {gua["meaning"]} (score={gua["score"]})')
    print(f'  🌌 天象: {" | ".join(logs)}')
    print(f'  ✅ 已存入 imperial_observatory.json + lishu/{now.strftime("%Y%m%d")}.json')

    return output


if __name__ == '__main__':
    imperial_observatory_run()
