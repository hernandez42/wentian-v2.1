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
import sys, os, json, time, subprocess
from datetime import datetime, timedelta

NOWCAST_JSON = '/root/data/fusion/nowcast.json'
CORREL_JSON = '/root/data/fusion/radar_correlation.json'
GOOGLE_VAL_JSON = '/root/data/fusion/google_validation.json'
ALERT_STATE = '/root/data/fusion/alert_push_state.json'  # 独立去重状态, 勿与其他进程共享alert_state.json
FEISHU_USER = os.environ.get('FEISHU_USER_ID', 'ou_52a5a07c6c4c825ccb530efe5befcc77')

# ── LLM 深度分析 (2026-09-09 R7) ──────────────────────────────────
# 用 hermes chat 调本机默认 LLM, 做真深度分析, 插在"核心句"与"预测"之间。
# 失败/超时/分数过低 → 优雅跳过, 不阻塞推送。
LLM_CACHE_PATH = '/root/data/fusion/alert_llm_cache.json'
LLM_TIMEOUT = 30  # 秒 (实测本机 LLM ~19s, 给 30s 留余量)
LLM_MIN_SCORE = 20  # 关注级及以上才调 LLM (稳定级免打扰)

# ── Mac风格图标 ────────────────────────────────────────────
ICONS = {
    'THUNDER':  '⛈', '雷暴': '⛈',
    'SQUALL':   '🌪', '飑线': '🌪',
    'FALSE_COLD': '❄', '假冷锋': '❄',
    'STATIONARY': '🌫', '准静止锋': '🌫', '静止锋': '🌫',  # 准静止锋
    'WIND_SHEAR': '💨', '风切变': '💨',
    'RAINSTORM': '🌧', '暴雨': '🌧',      # 暴雨
    'WATCH':    '🟡', '关注': '🟡',
    'WARNING':  '🟠', '预警': '🟠',
    'SEVERE':   '🔴', '强预警': '🔴',
    '稳定': '✅',
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
        ctx.check_hostname = True
        ctx.verify_mode = ssl.CERT_REQUIRED

        req = urllib.request.Request(
            'https://open.feishu.cn/open-apis/auth/v3/tenant_access_token/internal',
            data=json.dumps({'app_id':os.environ.get('FEISHU_APP_ID','cli_aae86c7e07235bed'),'app_secret':secret}).encode(),
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

# ── LLM 深度分析 (R7) ──────────────────────────────────────────
def _llm_cache_get(key):
    """读 LLM 缓存 (cooldown 内复用, 节省 19s LLM 推理)。"""
    if not os.path.exists(LLM_CACHE_PATH):
        return None
    try:
        with open(LLM_CACHE_PATH) as f:
            cache = json.load(f)
        entry = cache.get(key)
        if entry and time.time() < entry.get('exp', 0):
            return entry.get('text', '')
        return None
    except Exception:
        return None

def _llm_cache_put(key, text, ttl_sec=1800):
    """写 LLM 缓存 (默认 30 分钟, 与推送 cooldown 同)。"""
    try:
        cache = {}
        if os.path.exists(LLM_CACHE_PATH):
            try:
                with open(LLM_CACHE_PATH) as f:
                    cache = json.load(f)
            except Exception:
                cache = {}
        cache[key] = {'text': text, 'exp': time.time() + ttl_sec, 'ts': int(time.time())}
        with open(LLM_CACHE_PATH, 'w') as f:
            json.dump(cache, f, ensure_ascii=False)
    except Exception as e:
        print(f'[push_alert] ⚠ 写LLM缓存失败: {e}')

def analyze_with_llm(nc, correl, google_val):
    """调 hermes chat 本机默认 LLM 做深度分析。

    返回: str (1-3 句深度分析, 如'下击暴流冷池出流...'), 或空串(跳过得优雅)。
    失败: 任何异常 → 返回空串, 不影响推送链路。
    """
    score = nc.get('score', 0)
    if score < LLM_MIN_SCORE:
        return ''

    # 缓存 key: 严重等级 + 主天气型, 相同形态复用 (避免 19s 重复推理)
    main_type = max(
        [('雷暴', nc.get('thunder_score', 0)),
         ('飑线', nc.get('squall_score', 0)),
         ('冷锋', nc.get('false_cold_score', 0)),
         ('静止锋', nc.get('stationary_score', 0)),
         ('风切变', nc.get('wind_shear_score', 0))],
        key=lambda kv: kv[1]
    )[0]
    cache_key = f'{score//10}_{main_type}_{nc.get("warning_level","")}'

    cached = _llm_cache_get(cache_key)
    if cached:
        return cached

    # 拼 prompt: 给 LLM 看所有传感器, 要求写"机理+趋势+风险"三段, ≤200字
    pwv = nc.get('pwv_current') or 0
    pwv_sl = nc.get('pwv_slope_15min', 0)
    press = nc.get('press_current') or 0
    dp = nc.get('dp_3min', 0)
    temp = nc.get('temp_current') or 0
    dt = nc.get('dt_5min', 0)

    cross = []
    if (correl or {}).get('sdr_active'): cross.append('SDR')
    if (correl or {}).get('gnss_anomaly'): cross.append('GNSS')
    if (correl or {}).get('uno_pressure_change'): cross.append('UNO气压')
    if (correl or {}).get('uno_temp_change'): cross.append('UNO温度')

    prompt = f"""你是问天气象站短临分析专家。**只输出一段分析**（1-3句, 总字数≤200汉字, 不要换行, 不带emoji, 不带列表符号, 不重述"建议行动"）。

地点: 昆明长水 ZPPP
评分: {score} / 主型: {main_type} / 等级: {nc.get("warning_level","")}
PWV: {pwv:.1f}mm ({('↑' if pwv_sl>0 else '↓')}{abs(pwv_sl):.1f}mm/15min)
气压: {press:.1f}hPa (3min {('↑' if dp>0 else '↓')}{abs(dp):.1f}hPa)
温度: {temp:.1f}°C (5min {('↑' if dt>0 else '↓')}{abs(dt):.1f}°C)
多源异常: {','.join(cross) if cross else '无'}

请直接写出深度分析, 第一句说**机理**(为什么会出现这种天气型), 第二句说**接下来 30 分钟趋势), 如可能第三句说**最大风险点**。直接开始, 不要寒暄。"""

    try:
        proc = subprocess.run(
            ['hermes', 'chat', '--query-file', '-', '--oneshot'],
            input=prompt, capture_output=True, text=True,
            timeout=LLM_TIMEOUT, env={**os.environ, 'NO_COLOR': '1'}
        )
        if proc.returncode != 0:
            print(f'[push_alert] ⚠ LLM返回码{proc.returncode}: {proc.stderr[:200]}')
            return ''
        # hermes chat 输出含装饰边框, 提取 ╭─ ... ╰─ 之间的内容
        out = proc.stdout
        # 取最后一块 ╭ ... ╰ 块, 去掉边框
        text = out
        for block_marker in ['╭─', '╰─']:
            pass
        # 简单提取: 找 ╭ 与 ╰ 之间, 去掉首尾 ╭/╰ 行
        if '╭' in out and '╰' in out:
            start = out.find('╭')
            end = out.find('╰', start)
            if start >= 0 and end > start:
                block = out[start:end+1]  # 含 ╰ 结尾
                lines = []
                for line in block.split('\n'):
                    s = line.strip()
                    # 跳过 ╭/╰ 边框行 (含 ⚕ Hermes 标题)
                    if s.startswith('╭') or s.startswith('╰'):
                        continue
                    # 去掉 │ 边框
                    if s.startswith('│'):
                        s = s[1:].rstrip('│').strip()
                    if s:
                        lines.append(s)
                text = ' '.join(lines).strip()
        # 兜底: 找空行后的内容 (hermes 输出格式: 装饰 / 空行 / 分析 / 空行 / Resume)
        if not text or 'Resume this session' in text:
            # 去掉所有 metadata 行 (含 Resume / Session / Duration / Messages)
            lines = []
            for line in out.split('\n'):
                s = line.strip().strip('│').strip()
                if not s or any(k in s for k in ('Resume', 'Session:', 'Duration:', 'Messages:', 'Query:', 'Initializing', '─', '╭', '╰')):
                    continue
                lines.append(s)
            text = ' '.join(lines).strip()
        # 限制长度, 避免卡片爆长
        text = text.strip().strip('。').strip()
        if len(text) > 400:
            text = text[:400] + '…'
        if not text:
            print(f'[push_alert] ⚠ LLM输出为空')
            return ''
        _llm_cache_put(cache_key, text)
        return text
    except subprocess.TimeoutExpired:
        print(f'[push_alert] ⚠ LLM超时({LLM_TIMEOUT}s)')
        return ''
    except Exception as e:
        print(f'[push_alert] ⚠ LLM异常: {e}')
        return ''

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
def build_alert(nc, correl=None, google_val=None, llm_analysis='') -> dict:
    score = nc.get('score', 0)
    level = nc.get('primary_type') or nc.get('warning_level') or nc.get('level', 'CALM')
    icon = ICONS.get(level, '⚠')
    level_cn = LEVEL_CN.get(level, level)
    ts = nc.get('ts', 0)
    dt_time = datetime.fromtimestamp(ts).strftime('%m/%d %H:%M')

    # ── Mac风格标题 ──────────────────────────────────────
    # 格式: [图标] 问天 · 等级 | 评分
    title = f'{icon} 问天 · {level_cn} | {score}分'
    if correl and correl.get('coherence', 0) > 0.3:
        title += f' 相干{correl["coherence"]:.0%}'

    # ════════════════════════════════════════════════════════
    # 分析预测式卡片 (2026-09-09 R7 重构)
    #
    # 设计原则:
    #   旧版堆砌: PWV/气压/温度/各天气型分/SDR/GNSS/UNO 全罗列 → "无用"
    #   新版只做: 1) 是什么  2) 会怎样(预测)  3) 核心证据(数字仅这里)  4) 怎么应对
    #   **核心规则: 每个事实/数字 全卡片只出现一次**
    #     - 核心句: 定性"是什么" (不带数字)
    #     - 预测句: 走势 (不带具体观测值)
    #     - 依据句: 全部数字/证据集中
    #     - 行动句: 只说"做什么"
    # ════════════════════════════════════════════════════════

    # ── ① 判断主天气型(决定整条消息的主语) ────────────
    type_scores = {
        '雷暴': nc.get('thunder_score', 0),
        '飑线': nc.get('squall_score', 0),
        '假冷锋': nc.get('false_cold_score', 0),
        '静止锋': nc.get('stationary_score', 0),
        '风切变': nc.get('wind_shear_score', 0),
    }
    main_type, main_score = max(type_scores.items(), key=lambda kv: kv[1]) if any(type_scores.values()) else ('雷暴', 0)

    # ── ② 接下来 30 分钟会怎样(预测, 不含具体观测值) ─────
    pwv_now = nc.get('pwv_current') or 0
    pwv_slope = nc.get('pwv_slope_15min', 0)
    press_now = nc.get('press_current') or 0
    dp_3 = nc.get('dp_3min', 0)
    precip = nc.get('precip_intensity', '')
    # ★R10(2026-09-10): 静止锋雨势自适应 — 主人发现"已开始下雨但预测持续阴雨"硬编码滞后
    # 数据源: Open-Meteo hourly precipitation → wentian C 写入 precip_1h_mm
    # 规则: 1h降水>2mm=峰值期, 0.5~2=持续中, <0.5=减弱收尾
    precip_1h = nc.get('precip_1h_mm', 0) or 0
    if precip_1h > 2.0:
        precip_phase = 'peak'    # 峰值期: 雨强, 持续
    elif precip_1h > 0.5:
        precip_phase = 'mid'     # 持续期: 阵雨
    elif precip_1h > 0.0:
        precip_phase = 'tail'    # 减弱期: 毛毛雨快停
    else:
        precip_phase = 'none'    # 已停

    forecast_lines = []
    if main_type == '飑线' and main_score >= 26:
        lead_min = (correl or {}).get('lead_time_min', 8)
        forecast_lines.append(f'未来{lead_min}~{lead_min+10}分钟处于过境高峰, 30分钟后减弱转多云')
    elif main_type == '雷暴' and main_score >= 26:
        if pwv_slope > 0.3 and pwv_now >= 45:
            forecast_lines.append('未来30分钟雷暴发展增强, 1小时内达到峰值')
        elif pwv_slope < -0.3:
            forecast_lines.append('雷暴正在过境, 30分钟后减弱, 1小时内雨停转多云')
        else:
            forecast_lines.append('未来30分钟雷暴维持, 暂无显著加强或减弱')
    elif main_type == '假冷锋' and main_score >= 26:
        forecast_lines.append('未来1小时气温持续走低, 锋面过境后回升')
    elif main_type == '风切变' and main_score >= 26:
        forecast_lines.append('低空风切变将持续 30~60 分钟')
    elif main_type == '静止锋' and main_score >= 26:
        # ★R10: 静止锋 — 按实时雨势自适应, 不再硬塞"持续阴雨不转晴"
        if precip_phase == 'peak':
            forecast_lines.append('未来1小时雨势维持, 雨强较大注意排水')
        elif precip_phase == 'mid':
            forecast_lines.append('未来1小时阵雨持续, 强度无显著变化')
        elif precip_phase == 'tail':
            forecast_lines.append('未来30分钟雨势减弱, 1小时内逐步转阴')
        else:  # none
            forecast_lines.append('静止锋减弱, 未来1小时阴到多云, 不再降雨')
    elif precip and precip not in ('无降水', '无数据'):
        forecast_lines.append(f'未来30分钟{precip}持续')
    else:
        forecast_lines.append('未来30分钟天气基本稳定')

    # ── ③ 核心证据(集中所有数字, 每数字只写一次) ─────────
    evidences = []

    # 证据 A: PWV
    if pwv_now and pwv_now > 0.5:
        if abs(pwv_slope) > 0.3:
            tag = '骤升' if pwv_slope > 0 else '骤降'
            direction = '↑' if pwv_slope > 0 else '↓'
            evidences.append(f'PWV {pwv_now:.0f}mm {tag}{direction}{abs(pwv_slope):.1f}mm/15min — 大气水汽{tag}, 强对流{"积蓄中" if pwv_slope>0 else "正在释放"}')
        elif pwv_now >= 40:
            # ★R10: 静止锋减弱期不说"有利对流", 而说"残余水汽"
            if main_type == '静止锋' and precip_phase in ('tail', 'none'):
                evidences.append(f'PWV {pwv_now:.0f}mm 残余水汽 — 降水正在减弱收尾')
            else:
                evidences.append(f'PWV {pwv_now:.0f}mm 偏高 — 水汽充沛, 有利对流')
        else:
            evidences.append(f'PWV {pwv_now:.0f}mm — 水汽中等')

    # 证据 B: 气压突变
    if abs(dp_3) > 0.3:
        if dp_3 > 0.5:
            evidences.append(f'气压 3 分钟内骤升 {dp_3:.1f}hPa — 飑线/锋面过境典型特征')
        else:
            evidences.append(f'气压 3 分钟内骤降 {abs(dp_3):.1f}hPa — 强对流下曳气流/锋前减压')

    # 证据 C: 多源印证
    cross = []
    if (correl or {}).get('sdr_active'): cross.append('SDR')
    if (correl or {}).get('gnss_anomaly'): cross.append('GNSS')
    if (correl or {}).get('uno_pressure_change'): cross.append('UNO气压')
    if (correl or {}).get('uno_temp_change'): cross.append('UNO温度')
    if len(cross) >= 2:
        evidences.append(f'{"+".join(cross)} 同步异常 — 多源交叉印证')

    # 证据 D: Google 印证
    if google_val and google_val.get('confidence') == 'HIGH':
        evidences.append('Google DeepMind 同步检测到一致异常')

    # 证据 E: 软件雷达模式
    if correl and correl.get('matched_pattern') and correl.get('coherence', 0) > 0.5:
        pattern = correl['matched_pattern']
        coh = correl['coherence']
        evidences.append(f'软件雷达识别 {pattern}, 跨源一致率 {coh:.0%}')

    evidences = evidences[:4]  # 截断到 4 条

    # ── ④ 行动建议(纯行动, 不重复事实) ──────────────────
    advice_map = {
        'SEVERE': '立即停止户外作业, 远离金属物/独立树, 进入室内等待过境',
        'WARNING': '暂停户外作业, 关闭易遭雷击设备, 准备应急照明',
        'THUNDER': '雷暴活跃, 暂停露天作业, 关闭电源插座',
        '雷暴':    '雷暴活跃, 暂停露天作业, 关闭电源插座',
        'SQUALL':  '飑线过境, 远离临时建筑/广告牌, 固定户外设备',
        '飑线':     '飑线过境, 远离临时建筑/广告牌, 固定户外设备',
        'WIND_SHEAR': '低空风切变, 航空起降暂停或改航线',
        '风切变':   '低空风切变, 航空起降暂停或改航线',
        'FALSE_COLD': '温度骤降, 注意添衣, 防止冷应激',
        '假冷锋':   '温度骤降, 注意添衣, 防止冷应激',
        'STATIONARY': '持续阴雨, 防潮防霉, 注意排水',
        '静止锋':   '持续阴雨, 防潮防霉, 注意排水',
    }
    advice = advice_map.get(level)
    if not advice:
        if main_score >= 40:
            advice = '天气明显变化, 提前固定户外物品, 关注后续预警'
        elif main_score >= 20:
            advice = '天气存在变化, 保持关注'
        else:
            advice = '保持关注'

    # ── ⑤ 拼装新卡片(每事一述, 不重复) ──────────────────
    lines = [f'{icon}  问天 · {level_cn}', '']
    lines.append(f'{dt_time} · 昆明长水 ZPPP')
    lines.append('')

    # 第 1 段: 核心判断(纯定性, 不含数字)
    if main_type == '飑线' and main_score >= 26:
        core_msg = '飑线正在过境'
    elif main_type == '雷暴' and main_score >= 26:
        if pwv_slope > 0.3 and pwv_now >= 45:
            core_msg = '雷暴正在发展增强'
        elif pwv_slope < -0.3:
            core_msg = '雷暴正在过境减弱'
        else:
            core_msg = '雷暴活动维持中'
    elif main_type == '假冷锋' and main_score >= 26:
        core_msg = '冷空气南下过境'
    elif main_type == '风切变' and main_score >= 26:
        core_msg = '低空风切变'
    elif main_type == '静止锋' and main_score >= 26:
        # ★R10: 静止锋核心句也按实时雨势自适应
        if precip_phase in ('peak', 'mid'):
            core_msg = '静止锋维持'
        elif precip_phase == 'tail':
            core_msg = '静止锋雨势减弱'
        else:  # none
            core_msg = '静止锋减弱转好'
    elif precip and precip not in ('无降水', '无数据'):
        core_msg = f'{precip}持续'
    else:
        core_msg = '天气存在变化'
    lines.append(f'◆ {core_msg}')
    lines.append('')

    # 第 1.5 段: LLM 深度分析 (R7 新增) — 插在核心句与预测之间
    # 这是真正"分析预测"的关键段: 机理 + 趋势 + 风险, 由本机 LLM 实时生成
    if llm_analysis:
        lines.append(f'◇ 分析: {llm_analysis}')
        lines.append('')

    # 第 2 段: 预测(走势, 不含具体观测值)
    for fl in forecast_lines:
        lines.append(f'▸ 预测: {fl}')
    lines.append('')

    # 第 3 段: 核心证据(所有数字集中)
    if evidences:
        lines.append('▸ 依据:')
        for ev in evidences:
            lines.append(f'  · {ev}')
        lines.append('')

    # 第 4 段: 行动建议(纯行动)
    lines.append(f'◆ 行动: {advice}')
    lines.append('')

    # 第 5 段: 来源(极简一行)
    src_parts = ['问天 C']
    if cross: src_parts.append('+'.join(cross))
    if google_val and google_val.get('confidence') == 'HIGH': src_parts.append('+Google')
    if (correl or {}).get('coherence', 0) > 0.5: src_parts.append('+雷达')
    lines.append('来源 ' + ' · '.join(src_parts))

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
            'warning_level': 'SQUALL',
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
            'warning_level': '预警',
            'precip_intensity': '暴雨',
            'false_cold_note': '',
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
        llm_text = analyze_with_llm(test_nc, test_correl, None)
        alert = build_alert(test_nc, test_correl, None, llm_text)
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
    warning_lv = nc.get('warning_level', '无')
    # ⚠ 修复(2026-09-09 R6): warning_level="无" 视为CALM跳过推送。
    # 旧逻辑 `level = warning_level or level`, "无"是非空串(truthy)→ 不当CALM,
    # 会推送一条"无"级垃圾告警。现在先按 warning_level 判定是否真要推。
    if warning_lv == '无' and not args.force:
        print(f'[push_alert] CALM(评分{score}, 无预警), 跳过')
        return 0
    level = nc.get('primary_type') or nc.get('level') or warning_lv

    correl = None
    if args.radar or level != 'CALM':
        correl = load_json(CORREL_JSON)

    # CALM不推送
    if level == 'CALM' and not args.force:
        print(f'[push_alert] CALM(评分{score}), 跳过')
        return 0

    # 去重: 硬性30分钟冷却(不管level是否变化), 满足主人要求"30分钟一次"
    state = load_alert_state()
    now = time.time()
    if not args.force and now < state.get('suppress_until', 0):
        remaining = int(state['suppress_until'] - now)
        print(f'[push_alert] ⏭ 冷却中, {remaining}s后再推(最近已是{state.get("last_level")})')
        return 0

    # Google印证数据
    google_val = load_json(GOOGLE_VAL_JSON)

    # LLM 深度分析 (R7 新增): score >= 20 才调本机 LLM (hermes chat --oneshot)
    # 失败/超时/分数低 → 优雅跳过, 走纯模板卡片
    print(f'[push_alert] 调用LLM深度分析...')
    llm_text = analyze_with_llm(nc, correl, google_val)
    if llm_text:
        print(f'[push_alert] ✓ LLM返回{len(llm_text)}字分析')

    # 发送
    alert = build_alert(nc, correl, google_val, llm_text)
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