#!/usr/bin/env python3
"""
wt_fetch_tec.py — IGS 实时/快速电离层 TEC 抓取 (APEX ΔG · ℱ)
============================================================
目标: 用 IGS 全球电离层地图(GIM)的真实TEC数据替代硬编码Klobuchar系数

多源自动降级:
  1. IGS Rapid GIM (UWM/波兰) — 24小时延迟, 无需注册
  2. IGS Final GIM (CDDIS/NASA) — 11天延迟, 无注册
  3. GLONASS-IAC (俄罗斯) — 近实时
  4. WHU (武汉大学) — 近实时, 国内

输出: /root/data/fusion/tec_realtime.json
  格式: {ts, source, tec_kunming, lat, lon, validity}
"""

import json, os, sys, struct, time
from datetime import datetime, timedelta

OUT = '/root/data/fusion/tec_realtime.json'
LAT, LON = 25.08, 102.91  # 昆明长水


def _fetch_url(url, timeout=15):
    import urllib.request
    try:
        req = urllib.request.Request(url, headers={'User-Agent': 'WenTian/2.3'})
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return r.read()
    except Exception as e:
        return None


def _doy(y, m, d):
    return (datetime(y, m, d) - datetime(y, 1, 1)).days + 1


def parse_ionex_header(data):
    """解析IONEX文件头, 获取格网参数"""
    if isinstance(data, bytes):
        data = data.decode('ascii', errors='replace')
    lines = data.split('\n')
    h = {}
    for i, line in enumerate(lines[:160]):
        if len(line) < 60:
            continue
        label = line[60:].strip()
        if 'EPOCH OF FIRST MAP' in label:
            # 2026  9  6  0  0  0
            parts = line[:60].split()
            if len(parts) >= 6:
                h['epoch'] = datetime(int(parts[0]), int(parts[1]),
                                      int(parts[2]), int(parts[3]),
                                      int(parts[4]), int(float(parts[5])))
        elif 'LAT1 / LAT2 / DLAT' in label:
            vals = [float(x) for x in line[:60].split()]
            h['lat_min'], h['lat_max'], h['lat_d'] = vals[0], vals[1], vals[2]
        elif 'LON1 / LON2 / DLON' in label:
            vals = [float(x) for x in line[:60].split()]
            h['lon_min'], h['lon_max'], h['lon_d'] = vals[0], vals[1], vals[2]
    return h


def extract_tec_at_latlon(data, lat0, lon0):
    """解析 IONEX（WHU格式:纬度切片头+空格分隔整数）"""
    if isinstance(data, bytes):
        data = data.decode('ascii', errors='replace')
    lines = data.split('\n')
    
    in_map = False
    best_lat = None
    best_diff = 999
    best_data = []
    cur_lat = None
    cur_data = []
    
    for line in lines:
        if 'START OF TEC MAP' in line:
            in_map = True
            continue
        if not in_map:
            continue
        if 'END OF TEC MAP' in line:
            # 结算当前纬度切片
            if cur_lat is not None and cur_data:
                diff = abs(cur_lat - lat0)
                if diff < best_diff:
                    best_diff = diff
                    best_lat = cur_lat
                    best_data = list(cur_data)
            break  # 只读第一张地图
        
        # 纬度切片头: "  87.5-180.0 180.0   5.0 450.0  LAT/LON1/LON2/DLON/H"
        if 'LAT/LON' in line:
            # 结算上一纬度
            if cur_lat is not None and cur_data:
                diff = abs(cur_lat - lat0)
                if diff < best_diff:
                    best_diff = diff
                    best_lat = cur_lat
                    best_data = list(cur_data)
            # WHU格式: 纬度和lon1粘在一起 "87.5-180.0" 或 "-87.5-180.0"
            raw = line[:60].strip()
            cur_lat = None
            cur_data = []
            # 找第一个数字串作为纬度
            import re
            m = re.match(r'(-?\d+\.?\d*)(-?\d+\.?\d*)', raw)
            if m:
                try:
                    cur_lat = float(m.group(1))
                except ValueError:
                    pass
            continue
        
        # 跳过epoch行
        if 'EPOCH OF CURRENT MAP' in line:
            continue
        
        # 数据行: 空格分隔整数
        if cur_lat is not None:
            vals = line.strip().split()
            if vals and vals[0].lstrip('-').replace('.','',1).isdigit():
                cur_data.extend(vals)
    
    if best_data:
        nlon = len(best_data)
        lon_step = 360.0 / nlon
        ci = round((lon0 - (-180)) / lon_step)
        ci = max(0, min(ci, nlon - 1))
        if ci < len(best_data):
            try:
                val = float(best_data[ci]) / 10.0
                return val, 'ok'
            except (ValueError, IndexError):
                pass
    return None, 'no_match'


# ═══ 源1: WHU 武汉大学 IGS Rapid GIM (国内服务器, 2天延迟) ═══
def fetch_whu():
    now = datetime.utcnow()
    # WHU 文件延迟约2天, 回溯查找
    for lag in [2, 3, 4, 5]:
        dt = now - timedelta(days=lag)
        doy = _doy(dt.year, dt.month, dt.day)
        yy = dt.year % 100
        fn = f'whrg{doy:03d}0.{yy:02d}i.Z'
        url = f'ftp://igs.gnsswhu.cn/pub/whu/MGEX/ionosphere/{dt.year}/{fn}'
        data = _fetch_url(url, timeout=20)
        if data:
            try:
                import gzip
                raw = gzip.decompress(data)
            except Exception as e:
                print(f'[fetch_whu] decompress fail for {fn}: {type(e).__name__} {e}')
                continue
            tec, status = extract_tec_at_latlon(raw, LAT, LON)
            if tec is not None:
                return {'source': f'whu_rapid_{dt.strftime("%Y%m%d")}', 'tec': round(tec, 1),
                        'status': status, 'doy': doy, 'url': url}
            else:
                print(f'[fetch_whu] parse fail for {fn}: status={status}')
        else:
            print(f'[fetch_whu] no data for {fn} (lag={lag})')
    return None


# ═══ 源2: IGS Rapid GIM (UW-Madison/波兰, 回退) ═══
def fetch_uwm():
    now = datetime.utcnow()
    doy = _doy(now.year, now.month, now.day)
    yy = now.year % 100
    url = f'http://igsiono.uwm.edu.pl/testowy/rapid/imgtmp/igrg{doy:03d}0.{yy:02d}i.Z'
    data = _fetch_url(url)
    if data:
        try:
            import zlib
            raw = zlib.decompress(data, -zlib.MAX_WBITS)
        except Exception:
            import gzip
            try:
                raw = gzip.decompress(data)
            except Exception:
                raw = data
        tec, status = extract_tec_at_latlon(raw, LAT, LON)
        if tec is not None:
            return {'source': 'igrapid_uwm', 'tec': round(tec, 1), 'status': status}
    return None


# ═══ 源3: GLONASS-IAC (俄罗斯, 暂不实现) ═══
def fetch_glonass():
    return None


# ═══ 源4: 已有数据推算 ═══
def compute_tec_local(s4=None, kp=None, f107=None):
    """从本地已有数据推算等效TEC (当无外部源时)
    比纯硬编码好一点: 使用真实的Kp/S4/F10.7输入"""
    tec_src = {}
    if s4 is not None and s4 >= 0:
        tec_src['s4'] = s4 * 100
    if kp is not None and kp >= 0:
        tec_src['kp'] = (kp + 1) * 5
    if f107 is not None and f107 > 0:
        tec_src['f107'] = f107 * 0.1
    vals = [v for v in tec_src.values() if v > 0]
    if vals:
        tec = sum(vals) / len(vals)
        return round(tec, 1), 'local_composite', tec_src
    return None, 'no_local_data', {}


def main():
    now = datetime.utcnow()
    result = {'ts': int(time.time()), 'sources': {}}
    tec = None
    source = 'none'

    # 源1: WHU武汉大学 (国内, 最快)
    r = fetch_whu()
    if r:
        result['sources']['whu'] = r
        tec = r['tec']
        source = 'whu'

    # 源2: IGS Rapid GIM (波兰, 回退)
    if tec is None:
        r = fetch_uwm()

    # 回退: 本地推算
    if tec is None:
        s4, kp, f107 = None, None, None
        # 从wentian_latest读已有数据
        try:
            wd = json.load(open('/root/data/fusion/wentian_latest.json'))
            dd = wd.get('data', {})
            s4 = dd.get('multisrc_s4', {}).get('fused_s4')
            kp = dd.get('swpc', {}).get('kp')
            f107 = dd.get('swpc_f107', {}).get('flux_sfu')
        except Exception:
            pass
        tec, source, srcs = compute_tec_local(s4, kp, f107)
        result['local_composite'] = srcs

    result['tec_kunming'] = tec
    result['lat'] = LAT
    result['lon'] = LON
    result['source'] = source
    result['time'] = now.isoformat()

    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    with open(OUT + '.tmp', 'w') as f:
        json.dump(result, f, indent=2, ensure_ascii=False)
    os.rename(OUT + '.tmp', OUT)
    print(f'[wt_fetch_tec] TEC={tec} TECU from {source} | {len(result["sources"])} sources')


if __name__ == '__main__':
    main()
