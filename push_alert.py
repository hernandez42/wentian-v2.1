#!/usr/bin/env python3
"""
问天气象站 · 短临雷暴预警推送 v1.6 (Mac风格)
=========================================
风格: Mac风格 — 简洁、无冗余装饰、emoji分级、关键信息突出
     类似macOS通知中心的排版: 标题+内容+关键指标

功能: 读取 nowcast.json + radar_correlation.json, 当Nowcasting评分≥26
     或软件雷达检测到天气型时, 自动推送预警到飞书.

调用方式:
  push_alert.py                    # 检查nowcast+correl, 评分≥26则推送
  push_alert.py --force            # 强制推送(不管评分)
  push_alert.py --test             # 发送测试预警
  push_alert.py --radar            # 强制包含雷达相干数据

v1.0: 初始版本, 支持WATCH/WARNING/SEVERE三级预警推送
v1.5: 新增软件雷达三路相干数据 + 全天气型
v1.6: Mac风格排版 — 精简装饰线, 对齐关键指标, emoji分级
"""
import sys, os, json, time
from datetime import datetime, timedelta

NOWCAST_JSON = '/root/data/fusion/nowcast.json'
CORREL_JSON = '/root/data/fusion/radar_correlation.json'
ALERT_STATE = '/root/data/fusion/alert_state.json'
FEISHU_USER = 'ou_52a5a07c6c4c825ccb530efe5befcc77'

# ── Mac风格图标 ────────────────────────────────────────────
ICONS = {
    'THUNDER':  '⛈',
    'SQUALL':   '🌪',
    'FALSE_COLD': '❄',
    'STATIONARY': '🌫',  # 准静止锋
    'WIND_SHEAR': '💨',
    'RAINSTORM': '🌧',      # 暴雨
    'WATCH':    '🟡',
    'WARNING':  '🟠',
    'SEVERE':   '🔴',
}

LEVEL_CN = {
    'WATCH': '关注',
    'WARNING': '预警',
    'SEVERE': '强预警',
    'THUNDER': '雷暴',
    'SQUALL': '飑线',
    'FALSE_COLD': '假冷锋',
    'STATIONARY': '准静止锋',
    'WIND_SHEAR': '风切变',
    'RAINSTORM': '暴雨',
}

# ── 飞书推送 ────────────────────────────────────────────────
def send_feishu(msg: str) -> bool:
    try:
        secret = os.environ.get('FEISHU_APP_SECRET')
        if not secret:
            env_path = '/root/.hermes/.env'
            if os.path.exists(env_path):
                with open(env_path) as f:
                    for line in f:
                        if line.startswith('FEISHU_APP_SECRET='):
                            secret = line.split('=', 1)[1].strip()
                            break
        if not secret:
            try:
                import subprocess
                r = subprocess.run(['hermes', 'memory', 'get', 'feishu_secret'],
                                  capture_output=True, text=True, timeout=5)
                if r.stdout.strip():
                    secret = r.stdout.strip()
            except Exception:
                pass
        if not secret:
            print('[push_alert] ⚠ 未找到FEISHU密钥, 消息未发送(仅打印)')
            print(msg)
            return True

        import urllib.request, ssl
        ctx = ssl.create_default_context()
        ctx.check_hostname = False
        ctx.verify_mode = ssl.CERT_NONE

        req = urllib.request.Request(
            'https://open.feishu.cn/open-apis/auth/v3/tenant_access_token/internal',
            data=json.dumps({'app_id':'cli_aae86c7e07235bed','app_secret':secret}).encode(),
            headers={'Content-Type':'application/json'}
        )
        with urllib.request.urlopen(req, timeout=10, context=ctx) as r:
            token = json.loads(r.read()).get('tenant_access_token', '')
        if not token:
            print('[push_alert] ⚠ 获取token失败')
            return False

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
        with urllib.request.urlopen(req, timeout=10, context=ctx) as r:
            result = json.loads(r.read())
        if result.get('code') == 0:
            print('[push_alert] ✅ 飞书预警推送成功')
            return True
        print(f'[push_alert] ⚠ API返回: {result}')
        return False
    except Exception as e:
        print(f'[push_alert] ⚠ 推送异常: {e}')
        return False

# ── 加载数据 ────────────────────────────────────────────────
def load_json(path):
    if not os.path.exists(path): return None
    try:
        with open(path) as f: return json.load(f)
    except: return None

def load_alert_state():
    if not os.path.exists(ALERT_STATE):
        return {'last_level': None, 'last_ts': 0, 'suppress_until': 0}
    try:
        with open(ALERT_STATE) as f: return json.load(f)
    except:
        return {'last_level': None, 'last_ts': 0, 'suppress_until': 0}

def save_alert_state(state):
    try:
        with open(ALERT_STATE, 'w') as f: json.dump(state, f, indent=2)
    except: pass

# ── Mac风格消息构建 ────────────────────────────────────────
def build_alert(nc, correl=None) -> dict:
    score = nc.get('score', 0)
    level = nc.get('primary_type') or nc.get('level', 'CALM')
    icon = ICONS.get(level, '⚠')
    level_cn = LEVEL_CN.get(level, level)
    ts = nc.get('ts', 0)
    dt_time = datetime.fromtimestamp(ts).strftime('%m/%d %H:%M')

    # ── Mac风格标题 ──────────────────────────────────────
    # 格式: [图标] 问天 · 等级 | 评分
    title = f'{icon} 问天 · {level_cn} | {score}分'
    if correl and correl.get('coherence', 0) > 0.3:
        title += f' 相干{correl["coherence"]:.0%}'

    # ── Mac风格正文 ──────────────────────────────────────
    # 简洁排版: 只保留关键指标, 去掉冗余装饰线
    lines = [f'{icon}  问天短临预警', '']

    # 时间 + 地点
    lines.append(f'时间 {dt_time}')
    lines.append('地点 昆明长水 ZPPP')
    lines.append('')

    # 核心指标 (Mac风格: 指标名 + 值, 对齐)
    lines.append('核心指标')
    pwv = nc.get('pwv_current', 0)
    slope = nc.get('pwv_slope_15min', 0)
    press = nc.get('press_current', 0)
    dp = nc.get('dp_3min', 0)
    temp = nc.get('temp_current', 0)
    dt = nc.get('dt_5min', 0)

    # PWV
    pwv_label = 'PWV'
    pwv_val = f'{pwv:.1f}mm'
    if pwv > 50: pwv_val += ' ⚠极端'
    elif pwv > 45: pwv_val += ' ⚠高'
    elif pwv > 40: pwv_val += ' 偏高'
    lines.append(f'  {pwv_label}  {pwv_val}')

    # PWV变化
    if abs(slope) > 0.3:
        direction = '↑' if slope > 0 else '↓'
        lines.append(f'  PWV变化  {direction}{abs(slope):.1f}mm/15min')

    # 气压
    if abs(dp) > 0.3:
        direction = '↑' if dp > 0 else '↓'
        lines.append(f'  气压  {press:.1f}hPa ({direction}{abs(dp):.1f}hPa/3min)')
    else:
        lines.append(f'  气压  {press:.1f}hPa')

    # 温度
    if abs(dt) > 0.3:
        direction = '↑' if dt > 0 else '↓'
        lines.append(f'  温度  {temp:.1f}°C ({direction}{abs(dt):.1f}°C/5min)')
    else:
        lines.append(f'  温度  {temp:.1f}°C')

    # 各天气型评分(v1.5)
    thunder = nc.get('thunder_score', 0)
    squall = nc.get('squall_score', 0)
    false_cold = nc.get('false_cold_score', 0)
    stationary = nc.get('stationary_score', 0)
    shear = nc.get('wind_shear_score', 0)

    type_scores = []
    if thunder > 0: type_scores.append(f'雷暴{thunder}')
    if squall > 0: type_scores.append(f'飑线{squall}')
    if false_cold > 0: type_scores.append(f'假冷锋{false_cold}')
    if stationary > 0: type_scores.append(f'静止锋{stationary}')
    if shear > 0: type_scores.append(f'风切变{shear}')

    if type_scores:
        lines.append('')
        lines.append('天气型评分')
        lines.append('  ' + '  '.join(type_scores))

    # 软件雷达(v1.6)
    if correl and correl.get('coherence', 0) > 0.1:
        lines.append('')
        lines.append('软件雷达')
        lines.append(f'  相干 {correl["coherence"]:.0%}')
        lines.append(f'  模式 {correl.get("pattern_name","?")} 置信{correl.get("confidence",0):.0%}')
        lines.append(f'  提前 {correl.get("lead_time_min",0)}min')
        if correl.get('sdr_active'): lines.append('  SDR 异常')
        if correl.get('gnss_anomaly'): lines.append('  GNSS 异常')
        if correl.get('uno_pressure_change'): lines.append('  UNO 气压变')

    # 建议(Mac风格: 简洁一行)
    lines.append('')
    advice_map = {
        'SEVERE': '⚠️ 立即停止户外作业 远离金属物体',
        'WARNING': '⚠️ 减少户外活动 关注预警升级',
        'SQUALL': '⚠️ 飑线过境 注意强风',
        'WIND_SHEAR': '⚠️ 低空风切变 航空注意',
        'FALSE_COLD': '⚠️ 温度骤降 注意添衣',
        'STATIONARY': '⚠️ 持续阴雨 注意防潮',
    }
    advice = advice_map.get(level, '保持关注天气变化')
    lines.append(f'建议 {advice}')

    # 来源(极简)
    lines.append('')
    lines.append('来源 GNSS-PWV · METAR ZPPP · SDR · 问天C引擎')

    text = '\n'.join(lines)

    return {'title': title, 'text': text, 'level': level, 'score': score, 'ts': ts}

# ── 主逻辑 ─────────────────────────────────────────────────
def main():
    import argparse
    parser = argparse.ArgumentParser()
    parser.add_argument('--force', action='store_true')
    parser.add_argument('--test', action='store_true')
    parser.add_argument('--radar', action='store_true')
    parser.add_argument('--json', type=str)
    args = parser.parse_args()

    # 测试模式
    if args.test:
        test_nc = {
            'ts': int(time.time()),
            'level': 'SQUALL',
            'forecast': '飑线过境',
            'score': 65,
            'pwv_slope_15min': -2.5,
            'dp_3min': 2.5,
            'dt_5min': 0.5,
            'pwv_current': 40.0,
            'press_current': 1020.0,
            'temp_current': 22.0,
            'pwv_score': 0,
            'pwv_abs_score': 5,
            'press_score': 25,
            'temp_score': 0,
            'thunder_score': 0,
            'squall_score': 65,
            'false_cold_score': 0,
            'stationary_score': 0,
            'wind_shear_score': 0,
            'squall_press_rise': 2.5,
            'squall_wd_chg': 75,
            'squall_pwv_drop': 2.0,
            'alert_msg': '飑线(气压↑2.5hPa 风向变75° PWV↓2.0mm)'
        }
        test_correl = {
            'coherence': 0.72,
            'sdr_active': 1,
            'gnss_anomaly': 1,
            'uno_pressure_change': 1,
            'uno_temp_change': 0,
            'pattern_name': 'SQUALL',
            'confidence': 0.72,
            'lead_time_min': 10,
        }
        alert = build_alert(test_nc, test_correl)
        print(f'[测试] {alert["title"]}')
        print(alert['text'])
        ok = send_feishu(alert['text'])
        print(f'推送: {"成功" if ok else "失败"}')
        return 0 if ok else 1

    # 加载数据
    nc = load_json(args.json or NOWCAST_JSON)
    if not nc:
        print('[push_alert] ⚠ 无nowcast数据')
        return 0

    score = nc.get('score', 0)
    level = nc.get('primary_type') or nc.get('level', 'CALM')

    correl = None
    if args.radar or level != 'CALM':
        correl = load_json(CORREL_JSON)

    # CALM不推送
    if level == 'CALM' and not args.force:
        print(f'[push_alert] CALM(评分{score}), 跳过')
        return 0

    # 去重
    state = load_alert_state()
    now = time.time()
    if not args.force and level == state.get('last_level') and now < state.get('suppress_until', 0):
        remaining = int(state['suppress_until'] - now)
        print(f'[push_alert] ⏭ {level}已推送, {remaining}s后再推')
        return 0

    # 发送
    alert = build_alert(nc, correl)
    print(f'[push_alert] 发送: {alert["title"]}')
    ok = send_feishu(alert['text'])
    if ok:
        state['last_level'] = level
        state['last_ts'] = nc.get('ts', now)
        state['suppress_until'] = now + 1800
        save_alert_state(state)
    return 0 if ok else 1

if __name__ == '__main__':
    sys.exit(main())