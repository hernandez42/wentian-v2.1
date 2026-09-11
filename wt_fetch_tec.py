#!/usr/bin/env python3
"""
wt_fetch_tec.py — IGS 实时/快速电离层 TEC 抓取 (APEX ΔG · ℱ)
============================================================
目标: 用 IGS 全球电离层地图(GIM)的真实TEC数据替代硬编码Klobuchar系数

多源自动降级:
  1. WHU 武汉大学 IGS Rapid GIM (国内服务器, 2天延迟)
  2. IGS Rapid GIM (UW-Madison/波兰, 回退)

说明: GLONASS-IAC 无公开近实时真实接口, 已从源列表中移除,
      不再用 S4/Kp/F10.7 编造 TEC 数值。
      无真实源时 tec_kunming 输出为 null(不可用), 绝不凑数。

输出: /root/data/fusion/tec_realtime.json
  格式: {ts, source, tec_kunming, lat, lon, validity}
"""

import json, os, sys, struct, time
from datetime import datetime, timedelta

OUT = '/root/data/fusion/tec_realtime.json'
LAT, LON = 25.09917, 102.92667  # 昆明长水
_LON_ORIGIN = None  # 由IONEX header的LON1字段填充(-180或0), 见extract入口


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


def _decompress(data: bytes):
    """按魔数解压: gzip(1f 8b) / zlib / .Z(1f 9d, unix LZW — python无内置,
    用系统 gzip -dc 透明解压, POSIX gzip兼容.Z格式)。绝不把解不开的当文本硬解析。"""
    if not data or len(data) < 2:
        return None
    if data[:2] == b'\x1f\x8b':          # gzip
        import gzip
        return gzip.decompress(data)
    if data[:2] in (b'\x1f\x9d', b'\x1f\xa0'):  # unix compress .Z
        import subprocess, tempfile, os
        try:
            with tempfile.NamedTemporaryFile(suffix='.Z', delete=False) as tf:
                tf.write(data)
                tmp = tf.name
            try:
                r = subprocess.run(['gzip', '-dc', tmp], capture_output=True, timeout=30)
                if r.returncode == 0 and r.stdout:
                    return r.stdout
                return None
            finally:
                os.unlink(tmp)
        except Exception:
            return None
    # 试zlib再裸
    import zlib
    try:
        return zlib.decompress(data, -zlib.MAX_WBITS)
    except Exception:
        pass
    try:
        import gzip
        return gzip.decompress(data)
    except Exception:
        return None  # 解不开就诚实失败, 不再拿二进制当IONEX文本


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
    global _LON_ORIGIN
    if isinstance(data, bytes):
        data = data.decode('ascii', errors='replace')
    # 先从header拿经度起点(-180或0), 供格点索引用
    hdr = parse_ionex_header(data)
    if 'lon_min' in hdr:
        _LON_ORIGIN = hdr['lon_min'] if hdr['lon_min'] in (-180.0, 0.0, 180.0) else -180.0
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
        # ⚠ 修复(2026-09-11): 旧代码硬编码 -180 起点, IONEX存在 -180~180 与
        # 0~360 两种经度约定(中国GIM源常用后者), 0~360时昆明102.9°E会被当282.9°采样
        # → 取错格点, TEC张冠李戴。用header解析的起点, 解析不到则探测: 若圆周内
        # 找不到lon0位置但+360能找到, 自动切换约定。
        lon_origin = _LON_ORIGIN if _LON_ORIGIN is not None else -180.0
        ci = round((lon0 - lon_origin) / lon_step)
        if ci < 0 or ci >= nlon:
            # 换一种经度约定再试
            alt_origin = 0.0 if lon_origin == -180.0 else -180.0
            ci2 = round((lon0 - alt_origin) / lon_step)
            if 0 <= ci2 < nlon:
                ci = ci2
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
            # ⚠ 修复(2026-09-11): .Z 是 unix compress LZW 格式, gzip/zlib python
            # 模块都解不开 → 旧代码BadGzipFile→换lag天→全失败→TEC恒unavailable。
            # 新: 按魔数解压(gzip -dc 兼容.Z), 解不开继续回溯。
            raw = _decompress(data)
            if raw is None:
                print(f'[fetch_whu] decompress fail for {fn} (LZW/未知格式)')
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
        raw = _decompress(data)
        if raw is None:
            return None
        tec, status = extract_tec_at_latlon(raw, LAT, LON)
        if tec is not None:
            return {'source': 'igrapid_uwm', 'tec': round(tec, 1), 'status': status}
    return None


# ═══ 源3: GLONASS-IAC (俄罗斯) ═══
# 2026-09-09 修复：GLONASS-IAC 无公开近实时真实接口, 取消伪造声明, 不再实现。
# 保留函数但明确返回 None, 避免被当作可用源。
def fetch_glonass():
    return None


# ═══ 已有数据推算 (回退, 但绝不编造) ═══
def compute_tec_local(s4=None, kp=None, f107=None):
    """2026-09-09 修复：S4/Kp/F10.7 与 TEC 无线性物理关系, 旧公式纯属编造, 已删除。
    无真实 TEC 源时一律返回不可用(None), 由调用方写入 null, 不混入真实源数据。"""
    return None, 'no_real_source', {}


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
    # 修复(2026-09-09): 旧代码 r=fetch_uwm() 取到真实数据却没赋值 tec/source,
    # 导致 UWM 真实源被丢弃, 退化到占位推算。此处正确回填。
    if tec is None:
        r = fetch_uwm()
        if r:
            result['sources']['uwm'] = r
            tec = r['tec']
            source = 'uwm'

    # 回退: 本地推算 (2026-09-09 修复：仅作占位, 无真实源必返回 None)
    if tec is None:
        tec, source, srcs = compute_tec_local()
        result['local_composite'] = srcs

    result['tec_kunming'] = tec
    result['lat'] = LAT
    result['lon'] = LON
    result['source'] = source
    # 2026-09-09 修复：明确标注 TEC 是否可用, 不再用编造值混淆
    result['validity'] = 'ok' if tec is not None else 'unavailable'
    result['time'] = now.isoformat()

    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    with open(OUT + '.tmp', 'w') as f:
        json.dump(result, f, indent=2, ensure_ascii=False)
    os.rename(OUT + '.tmp', OUT)
    print(f'[wt_fetch_tec] TEC={tec} TECU from {source} | {len(result["sources"])} sources')


if __name__ == '__main__':
    main()
