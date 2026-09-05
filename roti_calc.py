#!/usr/bin/env python3
"""ROTI计算 v1.0 - Pi et al. (1997)"""
import json, os, sqlite3, time
from datetime import datetime
from statistics import stdev

WENTIAN_DB = '/root/data/wentian.db'
OUT_DIR = '/root/data/fusion'
TEC_JSON = os.path.join(OUT_DIR, 'tec_multi.json')

def roti_run():
    ts = int(time.time())
    now = datetime.now()
    
    # 从tec_multi.json收集历史数据
    tec_vals = []
    lishu_dir = os.path.join(OUT_DIR, 'lishu')
    for fname in sorted(os.listdir(lishu_dir))[-7:]:
        if fname.endswith('.json'):
            try:
                entries = json.load(open(os.path.join(lishu_dir, fname)))
                for e in entries:
                    if 'scientific_data' in e:
                        t = e['scientific_data'].get('tec_total_electron_content', 0)
                        if t and t > 0:
                            tec_vals.append((e['ts'], t))
            except: pass
    
    if os.path.exists(TEC_JSON):
        try:
            d = json.load(open(TEC_JSON))
            if d.get('fused_tec', 0) > 0:
                tec_vals.append((d['ts'], d['fused_tec']))
        except: pass
    
    tec_vals.sort()
    
    if len(tec_vals) < 6:
        roti, status = 0, '数据不足'
    else:
        rots = []
        for i in range(1, len(tec_vals)):
            dt_v = tec_vals[i][0] - tec_vals[i-1][0]
            dtec = tec_vals[i][1] - tec_vals[i-1][1]
            if dt_v > 0:
                rots.append(abs(dtec) / dt_v * 60)
        recent = rots[-12:] if len(rots) >= 12 else rots[-6:] if rots else [0]
        roti = round(stdev(recent), 4) if len(recent) >= 3 else 0
        if roti < 0.5: status = '平静'
        elif roti < 1.0: status = '弱扰动'
        elif roti < 2.0: status = '中等扰动'
        else: status = '强扰动'
    
    output = {'ts': ts, 'time': now.strftime('%Y-%m-%d %H:%M:%S'),
              'roti': roti, 'status': status,
              'samples': len(tec_vals),
              'version': 'ROTI v1.0 (Pi et al. 1997)',
              'note': '基于等效TEC时间序列, 参考标准ROTI算法'}
    with open(os.path.join(OUT_DIR, 'roti.json'), 'w') as f:
        json.dump(output, f, indent=2)
    print(f'  ROTI={roti} | {status} | samples={len(tec_vals)}')
    return output

if __name__ == '__main__':
    roti_run()
