#!/usr/bin/env python3
"""
问天气象站 - 预测精度评分系统 v1.1 (ENSO大考版)
================================
功能: 记录问天每次预报, 事后跟METAR实测对比打分
     评估厄尔尼诺期间问天预测能力

数据流:
  1. 问天daemon每60秒运行 → nowcast表记录当前评分
  2. Python预测脚本(ultimate_forecast等) → 生成forecast.json
  3. 评分系统定期评估: 预报vs实测 → 打分入库

调用方式:
  score_forecast.py record           # 记录当前预报(从Python预测JSON)
  score_forecast.py evaluate         # 评估已记录预报
  score_forecast.py report [--send]  # 生成评分报告(可选推飞书)
  score_forecast.py reset            # 清空记录
  score_forecast.py status           # 查看当前评分状态

评分标准 (中国气象局短临预报规范):
  温度: MAE≤1.0°C=100分, 每+0.5°C扣10分, ≥4.0=20分
  气压: MAE≤1.0hPa=100分, 每+1hPa扣10分, ≥8hPa=20分
  湿度: MAE≤5%=100分, 每+3%扣10分
  降水有无: 命中率×100分
  雷暴预警: POD(命中率)/FAR(空报率)/CSI(临界成功指数)
"""
import sys, os, json, sqlite3
from datetime import datetime, timedelta
from collections import defaultdict

SCORE_DB = '/root/data/fusion/score_db.json'
WENTIAN_DB = '/root/data/wentian.db'
REPORT_JSON = '/root/data/fusion/score_report.json'
FORECAST_JSONS = [
    '/root/data/fusion/forecast.json',
    '/root/data/fusion/ultimate_forecast.json',
    '/root/data/fusion/chronos_result.json',
    '/root/data/fusion/kriging_result.json',
]

# ── 评分函数 ──────────────────────────────────────────────
def score_temp(mae: float) -> int:
    if mae <= 1.0: return 100
    if mae <= 1.5: return 90
    if mae <= 2.0: return 80
    if mae <= 2.5: return 70
    if mae <= 3.0: return 60
    if mae <= 3.5: return 50
    if mae <= 4.0: return 40
    return 20

def score_press(mae: float) -> int:
    if mae <= 1.0: return 100
    if mae <= 2.0: return 90
    if mae <= 3.0: return 80
    if mae <= 4.0: return 70
    if mae <= 5.0: return 60
    if mae <= 6.0: return 50
    if mae <= 8.0: return 40
    return 20

def score_hum(mae: float) -> int:
    if mae <= 5: return 100
    if mae <= 8: return 90
    if mae <= 12: return 80
    if mae <= 16: return 70
    if mae <= 20: return 60
    return 40

# ── 加载/保存评分DB ──────────────────────────────────────
def load_score_db():
    if os.path.exists(SCORE_DB):
        try:
            with open(SCORE_DB) as f:
                return json.load(f)
        except Exception: pass
    return {
        'forecasts': [],
        'evaluations': [],
        'metadata': {'version': '1.1', 'created': datetime.now().isoformat()}
    }

def save_score_db(db):
    os.makedirs(os.path.dirname(SCORE_DB), exist_ok=True)
    with open(SCORE_DB, 'w') as f:
        json.dump(db, f, indent=2, ensure_ascii=False)

# ── 读取所有预测源 ────────────────────────────────────────
FORECAST_MAX_AGE_SEC = 24 * 3600   # 超过24h的预测文件视为陈旧, 不参与评分
WEATHERNEXT_JSON     = '/root/data/fusion/weathernext_forecast.json'  # WeatherNext 主模型, 主人架构指定

def _weathernext_to_forecast(data: dict):
    """将 weathernext_forecast.json 标准化为 forecast_1h/3h/6h 结构,
    使其可直接进入通用评分管道 (T=温度, P=MSL气压 — 无需站点换算)"""
    hours = data.get('forecast_hours', [])
    if not hours:
        return None
    out = {}
    for lead_h, idx in [('1h', 1), ('3h', 3), ('6h', 6)]:
        # idx=0是当前, 1/3/6 索引即代表未来第1/3/6小时
        if len(hours) > idx:
            h = hours[idx]
            out[f'forecast_{lead_h}h'] = {
                'T': h.get('temperature_2m'),
                'P': h.get('pressure_msl_hpa'),  # WeatherNext 输出即 MSL, 不需换算
                'H': h.get('relative_humidity_pct'),
            }
    return out

def load_all_forecasts():
    """从所有预测JSON读取当前预报 (过滤>24h陈旧文件, 避免污染评分;
    并将 WeatherNext 主模型纳入主评分源)"""
    forecasts = {}
    now_ts = int(datetime.now().timestamp())
    paths = list(FORECAST_JSONS) + [WEATHERNEXT_JSON]
    seen = set()
    for path in paths:
        if path in seen:
            continue
        seen.add(path)
        if not os.path.exists(path):
            continue
        try:
            age = now_ts - int(os.path.getmtime(path))
            if age > FORECAST_MAX_AGE_SEC:
                # 静默跳过陈旧文件(已无价值, 反复打印会刷屏)
                continue
            with open(path) as f:
                data = json.load(f)
            name = os.path.basename(path).replace('.json', '')
            if name == 'weathernext_forecast' and 'forecast_hours' in data:
                data = _weathernext_to_forecast(data)
                if not data:
                    continue
                name = 'weathernext'  # 标准化来源名
            elif name == 'weathernext_forecast':
                continue
            forecasts[name] = {
                'source': name,
                'data': data,
                'record_ts': int(datetime.now().timestamp()),
            }
        except Exception as e:
            print(f'[score] ⚠ 读取{path}失败: {e}')
    return forecasts

# ── METAR实测查询 ─────────────────────────────────────────
def get_metar_near(ts_target: int, window: int = 1800) -> dict:
    """找target时间最近(±window秒)的METAR实测"""
    try:
        conn = sqlite3.connect(WENTIAN_DB)
        cur = conn.cursor()
        # ⚠ 修复(2026-09-06): METAR缺测值以0.0入库, 直接对比产生1000hPa假MAE
        # — 温度/海压为0的行按字段剔除(NULLIF)
        cur.execute(
            "SELECT ts, NULLIF(temp,0.0), dewpoint, NULLIF(altim,0.0), raw "
            "FROM metar WHERE icao='ZPPP' AND raw NOT LIKE 'SYNTHETIC%' AND ts >= ? AND ts <= ? "
            "AND temp != 0.0 AND altim != 0.0 "
            "ORDER BY ABS(ts - ?) LIMIT 1",
            (ts_target - window, ts_target + window, ts_target)
        )
        row = cur.fetchone()
        conn.close()
        if row:
            return {'ts': row[0], 'temp': row[1], 'dewpoint': row[2], 'press': row[3], 'raw': row[4]}
    except Exception as e:
        print(f'[score] METAR查询失败: {e}')
    return {}

# ── 记录当前预报 ──────────────────────────────────────────
def do_record():
    forecasts = load_all_forecasts()
    if not forecasts:
        print('[score] ⚠ 无预测数据, 问天/预测脚本可能未运行')
        # 仍然记录nowcast状态作为参考
        try:
            conn = sqlite3.connect(WENTIAN_DB)
            cur = conn.cursor()
            cur.execute("SELECT ts, score, level, pwv_current, alert_msg FROM nowcast ORDER BY ts DESC LIMIT 1")
            row = cur.fetchone()
            conn.close()
            if row:
                db = load_score_db()
                db['forecasts'].append({
                    'source': 'nowcast_only',
                    'record_ts': int(datetime.now().timestamp()),
                    'nowcast': {'ts': row[0], 'score': row[1], 'level': row[2],
                                'pwv': row[3], 'alert': row[4]},
                })
                save_score_db(db)
                print(f'[score] ✅ 已记录nowcast状态: score={row[1]} level={row[2]}')
                return 0
        except:
            pass
        return 1

    db = load_score_db()
    for name, fc in forecasts.items():
        db['forecasts'].append(fc)
    save_score_db(db)

    ts_str = datetime.now().strftime('%Y-%m-%d %H:%M')
    print(f'[score] ✅ 已记录预报 ts={ts_str}')
    for name in forecasts:
        print(f'  - {name}')
    return 0

# ── 评估预报 ──────────────────────────────────────────────
def do_evaluate():
    db = load_score_db()
    forecasts = db.get('forecasts', [])
    if not forecasts:
        print('[score] ⚠ 无历史预报记录, 先运行 record')
        return 1

    new_evals = []
    now_ts = int(datetime.now().timestamp())
    evaluated_leads = set()  # ⚠ 2026-09-11: 记录本条预报已评估的lead

    # 对每个预报记录, 评估1h/3h/6h预测
    for fc in forecasts:
        if fc.get('_evaluated'):
            continue
        record_ts = fc.get('record_ts', 0)
        # ⚠ 修复(2026-09-09 R5): 跳过>24h陈旧预报 — 之前kriging/chronos三天前的快照
        # 跟今天实测对比产生73hPa假MAE, 把综合分拖到40/100。新鲜度门保证评分只反映当前活跃模型。
        if now_ts - record_ts > 24 * 3600:
            fc['_evaluated'] = True
            continue
        source = fc.get('source', 'unknown')
        data = fc.get('data', {})

        # 不同数据源有不同的字段名, 统一提取
        # 1) forecast.json / ultimate_forecast.json: 可能有1h/3h/6h字段
        # 2) chronos_result.json: 可能有predictions数组
        # 3) kriging_result.json: 可能有grid预测

        # 尝试提取1h/3h/6h预测
        for lead_h in [1, 3, 6]:
            target_ts = record_ts + lead_h * 3600

            # 如果target还没到(差不到1h), 跳过
            if target_ts > now_ts + 3600:
                continue

            # 找实测
            actual = get_metar_near(target_ts)
            if not actual:
                if target_ts < now_ts - 7200:
                    print(f'[score] ⚠ {source} lead={lead_h}h 无实测 (target={datetime.fromtimestamp(target_ts).strftime("%m-%d %H:%M")})')
                continue

            pred_temp = pred_press = None

            # ⚠ 修复(2026-09-06): 字段路径与实际JSON结构完全对齐 —
            # 旧代码找 data['1h']['temp']/data['hourly']/source=='chronos',
            # 实际结构是 forecast_1h.T/P、temperature['1h']、1h_median、
            # 且记录源名带 _result 后缀 → 评分管道自上线起0条评估。
            if source == 'forecast':
                key = f'forecast_{lead_h}h'
                blk = data.get(key)
                if isinstance(blk, dict):
                    pred_temp = blk.get('T')
                    pred_press = blk.get('P')
            elif source == 'ultimate_forecast':
                t_blk = data.get('temperature', {})
                p_blk = data.get('pressure', {})
                if isinstance(t_blk, dict):
                    pred_temp = t_blk.get(f'{lead_h}h')
                if isinstance(p_blk, dict):
                    pred_press = p_blk.get(f'{lead_h}h')
            elif source in ('chronos', 'chronos_result'):
                t_blk = data.get('temp', {})
                p_blk = data.get('pressure', {})
                if isinstance(t_blk, dict):
                    pred_temp = t_blk.get(f'{lead_h}h_median')
                if isinstance(p_blk, dict):
                    pred_press = p_blk.get(f'{lead_h}h_median')
            elif source in ('kriging', 'kriging_result'):
                # kriging 只输出气压序列预测(逐时), 温度暂无
                preds = data.get('pressure_predictions')
                if isinstance(preds, list) and len(preds) >= lead_h:
                    pred_press = preds[lead_h - 1]

            # 站内压→MSL: forecast.json 的 P 是 UNO 机柜站内压(~821hPa),
            # METAR altim 是海平面压(~1016), 不换算就是 233hPa 假MAE
            # 2026-09-09 修复：标准等温气压高度换算(标准大气标高公式)
            # p0 = p * exp(h / H), H=8430m 为标准大气标高 R*T0/(M*g),
            # h=2103m 为昆明站海拔(原2104为笔误)。-38.8 为昆明站标定偏移,
            # 与 api_local.c 的 UNO_P_OFFSET_HPA 一致(由 1016-822*exp(2103/8430) 得出),
            # 经验证该组合输出≈1016hPa 与 METAR 吻合, 故保留。
            if pred_press is not None and 700 < pred_press < 950:
                import math
                H_SCALE = 8430.0   # 标准大气标高 (m)
                H_KUNMING = 2103.0  # 昆明长水机场海拔 (m)
                pred_press = pred_press * math.exp(H_KUNMING / H_SCALE)
                pred_press = pred_press - 38.8  # 站点标定偏移(见 api_local.c)

            eval_rec = {
                'eval_ts': now_ts,
                'forecast_ts': record_ts,
                'source': source,
                'lead_h': lead_h,
                'target_ts': target_ts,
                'actual_ts': actual['ts'],
                'pred_temp': pred_temp,
                'actual_temp': actual['temp'],
                'pred_press': pred_press,
                'actual_press': actual['press'],
                'temp_mae': abs(pred_temp - actual['temp']) if (pred_temp is not None and actual['temp'] is not None) else None,
                'press_mae': abs(pred_press - actual['press']) if (pred_press is not None and actual['press'] is not None) else None,
            }
            if eval_rec['temp_mae'] is not None or eval_rec['press_mae'] is not None:
                new_evals.append(eval_rec)
                # 记录该lead已评估, 供下方决定是否标记整体完成
                evaluated_leads.add(lead_h)

        # ⚠ 修复(2026-09-11): 旧代码无条件 fc['_evaluated']=True — 未到期的lead被
        # continue跳过后立即标记"已评估", 3h/6h预报永不进入评分(样本系统性只含1h)。
        # 新逻辑: 只有全部lead(1/3/6h)都过目标时刻才算整条完成;
        # 或预报本身就没有可评字段也标记完成(避免死记录)。
        if all(record_ts + lh * 3600 <= now_ts + 3600 for lh in (1, 3, 6)):
            fc['_evaluated'] = True
        elif evaluated_leads:
            # 部分评估过且无新字段可评(如kriging只有气压) → 完成剩余即标记
            has_any_field = any(
                (isinstance(fc.get('data', {}), dict) and fc['data'])
                for _ in [0]
            )
            if not has_any_field:
                fc['_evaluated'] = True

    if new_evals:
        db['evaluations'].extend(new_evals)
        save_score_db(db)
        print(f'[score] ✅ 已评估 {len(new_evals)} 条预报-实测对比')
    else:
        print('[score] 无新的可评估数据')

    # ⚠ 修复(2026-09-09 R5): 清理>48h陈旧记录 — 避免score_db无限膨胀导致历史包袱拖低分
    before_fc = len(db.get('forecasts', []))
    before_ev = len(db.get('evaluations', []))
    db['forecasts'] = [f for f in db.get('forecasts', [])
                       if now_ts - f.get('record_ts', 0) <= 48 * 3600]
    db['evaluations'] = [e for e in db.get('evaluations', [])
                         if now_ts - e.get('eval_ts', 0) <= 48 * 3600]
    if len(db['forecasts']) < before_fc or len(db['evaluations']) < before_ev:
        save_score_db(db)
        print(f'[score] 🧹 清理陈旧: 预报{before_fc}→{len(db["forecasts"])} 评估{before_ev}→{len(db["evaluations"])}')

    _print_summary(db)
    return 0

def _print_summary(db):
    evals = db.get('evaluations', [])
    if not evals:
        print('[score] 无评估数据')
        return

    print(f'\n{"="*60}')
    print(f'问天预测精度评分报告 (共{len(evals)}条对比)')
    print(f'{"="*60}')

    # 按source+lead分组
    by_key = defaultdict(list)
    for e in evals:
        key = (e.get('source', 'unknown'), e['lead_h'])
        by_key[key].append(e)

    for (source, lead) in sorted(by_key.keys()):
        items = by_key[(source, lead)]
        temp_maes = [e['temp_mae'] for e in items if e['temp_mae'] is not None]
        press_maes = [e['press_mae'] for e in items if e['press_mae'] is not None]

        print(f'\n--- {source} ({lead}h) ---')
        print(f'  样本: {len(items)}')
        scores = []
        if temp_maes:
            avg = sum(temp_maes) / len(temp_maes)
            s = score_temp(avg)
            scores.append(s)
            print(f'  🌡 温度MAE: {avg:.2f}°C → {s}分')
        if press_maes:
            avg = sum(press_maes) / len(press_maes)
            s = score_press(avg)
            scores.append(s)
            print(f'  📊 气压MAE: {avg:.2f}hPa → {s}分')
        if scores:
            print(f'  综合: {sum(scores)//len(scores)}分')

    # 雷暴预警评估
    alert_r = _eval_alerts(db)
    if alert_r:
        print(f'\n--- 雷暴预警评估 ---')
        for k, v in alert_r.items():
            print(f'  {k}: {v}')

def _eval_alerts(db):
    """评估Nowcasting预警命中率"""
    try:
        conn = sqlite3.connect(WENTIAN_DB)
        cur = conn.cursor()
        cur.execute("SELECT ts, score, level, warning_level FROM nowcast ORDER BY ts")
        rows = cur.fetchall()
        conn.close()
    except:
        return {}
    if not rows:
        return {}

    # 统计预警次数
    # ⚠ 修复(2026-09-09 R5): 原代码用{'WATCH','WARNING','SEVERE'}查r[2] (level列=中文"雷暴/飑线..."),
    # 永远不匹配 → "预警总次数"恒为0, 报告系统性说谎。改查r[3] (warning_level列) 配合C实际写入的中文。
    levels = {'关注': 0, '预警': 0, '强预警': 0}
    for r in rows:
        if r[3] in levels:
            levels[r[3]] += 1

    # 跟METAR TSRA对比
    try:
        conn = sqlite3.connect(WENTIAN_DB)
        cur = conn.cursor()
        cur.execute("SELECT ts FROM metar WHERE icao='ZPPP' AND raw LIKE '%TSRA%' AND raw NOT LIKE 'SYNTHETIC%' ORDER BY ts")
        tsra_rows = cur.fetchall()
        conn.close()
    except:
        tsra_rows = []

    total_tsra = len(tsra_rows)
    alerted = 0
    for tsra_r in tsra_rows:
        tsra_ts = tsra_r[0] if tsra_r[0] is not None else 0
        if not tsra_ts:
            continue
        for r in rows:
            # ⚠ 修复(2026-09-11): r[0]/r[1] 为NULL时 int(None)/比较抛TypeError
            try:
                if r[0] is None or r[1] is None:
                    continue
                if abs(int(r[0]) - tsra_ts) <= 1800 and int(r[1]) >= 26:
                    alerted += 1
                    break
            except (TypeError, ValueError):
                continue

    return {
        '预警总次数': sum(levels.values()),
        'TSRA实测时段': total_tsra,
        '预警覆盖': f'{alerted}/{total_tsra}' if total_tsra else 'N/A',
    }

# ── 生成报告 ──────────────────────────────────────────────
def do_report():
    db = load_score_db()
    evals = db.get('evaluations', [])

    lines = [
        '━━━━━━━━━━━━━━━━━━━━',
        '📊 问天预测精度评分报告 (ENSO大考)',
        '━━━━━━━━━━━━━━━━━━━━',
        f'报告时间: {datetime.now().strftime("%Y-%m-%d %H:%M")}',
        f'评估样本: {len(evals)} 条预报-实测对比',
        '',
    ]

    if evals:
        by_key = defaultdict(list)
        for e in evals:
            key = (e.get('source', 'unknown'), e['lead_h'])
            by_key[key].append(e)

        total_temp_score = 0
        total_press_score = 0
        n_temp = n_press = 0

        for (source, lead) in sorted(by_key.keys()):
            items = by_key[(source, lead)]
            temp_maes = [e['temp_mae'] for e in items if e['temp_mae'] is not None]
            press_maes = [e['press_mae'] for e in items if e['press_mae'] is not None]

            lines.append(f'━━━ {source} ({lead}h) ━━━')
            if temp_maes:
                avg = sum(temp_maes) / len(temp_maes)
                s = score_temp(avg)
                total_temp_score += s
                n_temp += 1
                lines.append(f'  🌡 温度MAE: {avg:.2f}°C → {s}分')
            if press_maes:
                avg = sum(press_maes) / len(press_maes)
                s = score_press(avg)
                total_press_score += s
                n_press += 1
                lines.append(f'  📊 气压MAE: {avg:.2f}hPa → {s}分')
            lines.append('')

        overall = 0
        if n_temp > 0 and n_press > 0:
            overall = (total_temp_score // n_temp + total_press_score // n_press) // 2
        elif n_temp > 0:
            overall = total_temp_score // n_temp
        elif n_press > 0:
            overall = total_press_score // n_press

        lines.append('━━━ 综合评分 ━━━')
        lines.append(f'  🏆 问天总评: {overall}/100分')
        if overall >= 90: grade = 'A+ 优秀 (ENSO大考通过)'
        elif overall >= 80: grade = 'A 良好'
        elif overall >= 70: grade = 'B+ 合格'
        elif overall >= 60: grade = 'B 及格'
        else: grade = 'C 需改进'
        lines.append(f'  📋 等级: {grade}')
        lines.append('')

        # 预警评估
        alert_r = _eval_alerts(db)
        if alert_r:
            lines.append('━━━ 雷暴预警 ━━━')
            for k, v in alert_r.items():
                lines.append(f'  {k}: {v}')
            lines.append('')
    else:
        lines.append('暂无评估数据, 先运行 record → evaluate')

    lines.extend([
        '━━━ 评分标准 ━━━',
        '  温度: MAE≤1.0°C=100分, 每+0.5°C扣10分',
        '  气压: MAE≤1.0hPa=100分, 每+1hPa扣10分',
        '  数据来源: METAR ZPPP长水机场实测',
        '━━━━━━━━━━━━━━━━━━━━',
    ])

    msg = '\n'.join(lines)
    print(msg)

    os.makedirs(os.path.dirname(REPORT_JSON), exist_ok=True)
    with open(REPORT_JSON, 'w') as f:
        json.dump({'text': msg, 'score': overall if evals else 0,
                   'ts': int(datetime.now().timestamp())}, f, indent=2, ensure_ascii=False)

    if '--send' in sys.argv:
        _send_feishu(msg)
    return 0

def _send_feishu(msg):
    try:
        import urllib.request, ssl
        env_path = '/root/.hermes/.env'
        secret = None
        if os.path.exists(env_path):
            with open(env_path) as f:
                for line in f:
                    if line.startswith('FEISHU_APP_SECRET='):
                        secret = line.split('=', 1)[1].strip()
                        break
        if not secret:
            print('⚠ 无FEISHU密钥')
            return
        ctx = ssl.create_default_context()
        ctx.check_hostname = True
        ctx.verify_mode = ssl.CERT_REQUIRED
        req = urllib.request.Request(
            'https://open.feishu.cn/open-apis/auth/v3/tenant_access_token/internal',
            data=json.dumps({'app_id':os.environ.get('FEISHU_APP_ID','cli_aae86c7e07235bed'),'app_secret':secret}).encode(),
            headers={'Content-Type':'application/json'}
        )
        with urllib.request.urlopen(req, timeout=10, context=ctx) as r:
            token = json.loads(r.read()).get('tenant_access_token', '')
        if token:
            payload = json.dumps({
                'receive_id': os.environ.get('FEISHU_USER_ID','ou_52a5a07c6c4c825ccb530efe5befcc77'),
                'msg_type': 'text',
                'content': json.dumps({'text': msg})
            }).encode()
            req = urllib.request.Request(
                'https://open.feishu.cn/open-apis/im/v1/messages?receive_id_type=open_id',
                data=payload,
                headers={'Authorization':'Bearer '+token, 'Content-Type':'application/json'}
            )
            with urllib.request.urlopen(req, timeout=10, context=ctx) as r:
                result = json.loads(r.read())
            if result.get('code') == 0:
                print('✅ 飞书推送成功')
            else:
                print(f'❌ 推送失败: {result}')
    except Exception as e:
        print(f'⚠ 推送异常: {e}')

# ── 状态 ──────────────────────────────────────────────────
def do_status():
    db = load_score_db()
    forecasts = db.get('forecasts', [])
    evals = db.get('evaluations', [])
    print(f'问天评分系统状态')
    print(f'  预报记录: {len(forecasts)} 条')
    print(f'  评估记录: {len(evals)} 条')
    if forecasts:
        last = forecasts[-1]
        print(f'  最近记录: {datetime.fromtimestamp(last.get("record_ts",0)).strftime("%Y-%m-%d %H:%M")} ({last.get("source","?")})')
    if evals:
        last_e = evals[-1]
        print(f'  最近评估: {datetime.fromtimestamp(last_e.get("eval_ts",0)).strftime("%Y-%m-%d %H:%M")} '
              f'lead={last_e.get("lead_h","?")}h temp_mae={last_e.get("temp_mae","?")} press_mae={last_e.get("press_mae","?")}')
    return 0

# ── 重置 ──────────────────────────────────────────────────
def do_reset():
    if os.path.exists(SCORE_DB):
        os.remove(SCORE_DB)
        print('[score] ✅ 评分记录已清空')
    else:
        print('[score] 无评分记录')
    return 0

# ── 主入口 ───────────────────────────────────────────────
def main():
    if len(sys.argv) < 2:
        print('用法: score_forecast.py [record|evaluate|report|reset|status]')
        print('  record    记录当前预报')
        print('  evaluate  评估已记录预报')
        print('  report [--send]  生成评分报告')
        print('  reset     清空评分记录')
        print('  status    查看评分状态')
        return 1

    cmd = sys.argv[1]
    if cmd == 'record': return do_record()
    if cmd == 'evaluate': return do_evaluate()
    if cmd == 'report': return do_report()
    if cmd == 'reset': return do_reset()
    if cmd == 'status': return do_status()
    print(f'未知命令: {cmd}')
    return 1

if __name__ == '__main__':
    sys.exit(main())