#!/usr/bin/env python3
"""
问天 LLM深度分析引擎 v1.0
=========================
推送前调用系统默认LLM(DeepSeek-V4-Flash-Thinking)分析多源气象数据。
"""

import os, sys, json, ssl, urllib.request, re
from datetime import datetime, timezone, timedelta
from typing import Optional, Dict, Any, List

FUSION_DIR = '/root/data/fusion'
LLM_ANALYSIS_OUT = f'{FUSION_DIR}/llm_enhanced_analysis.json'
WENTIAN_JSON = f'{FUSION_DIR}/wentian_latest.json'
TRACKING_FILE = f'{FUSION_DIR}/llm_accuracy_track.json'

LLM_API = 'https://llm-gateway.kmair.net/v1/chat/completions'
LLM_MODEL = 'CA/DeepSeek-V4-Flash'


def _get_llm_key() -> Optional[str]:
    try:
        with open('/root/.hermes/.env') as f:
            for line in f:
                if 'HERMES_CUSTOM_LLM_GATEWAY_KMAIR_NET_API_KEY' in line:
                    return line.split('=', 1)[1].strip()
    except OSError:
        pass
    return None


def _ctx() -> ssl.SSLContext:
    ctx = ssl.create_default_context()
    ctx.check_hostname = True
    ctx.verify_mode = ssl.CERT_REQUIRED
    return ctx


def _load_json(path: str, default: Any = None) -> Any:
    try:
        if os.path.exists(path):
            with open(path, encoding='utf-8') as f:
                return json.load(f)
    except (OSError, json.JSONDecodeError) as e:
        print(f'[load_json] {path}: {e}')
    return default


def _safe(v: Any, default: Any = None) -> Any:
    return v if v is not None else default


def collect_data() -> Dict[str, Any]:
    data = {}
    wt = _load_json(WENTIAN_JSON, {})
    data['wentian'] = wt.get('data', {}) if isinstance(wt, dict) else {}
    data['generated_at'] = wt.get('generated_at', '') if isinstance(wt, dict) else ''
    for fname in ['weathernext_forecast.json', 'imperial_enhancement.json',
                  'nowcast.json', 'multisrc_fusion.json', 'astral.json',
                  'ultimate_forecast.json', 'forecast.json', 'tec_multi.json',
                  'tec_realtime.json', 'roti.json', 'alert_state.json',
                  'analysis_full.json', 'kriging_result.json', 'chronos_result.json']:
        path = f'{FUSION_DIR}/{fname}'
        key = fname.replace('.json', '')
        data[key] = _load_json(path, {})
    return data


def build_prompt(data: Dict[str, Any]) -> str:
    wd = data.get('wentian', {})
    imp = data.get('imperial_enhancement', {})
    astral = data.get('astral', {})
    nowcast = data.get('nowcast', {})
    ms = wd.get('multi_source', {})
    outdoor = wd.get('outdoor', {})
    metar = wd.get('metar_zppp', {})
    swpc = wd.get('swpc', {})
    ext = wd.get('external', {})
    pwv = wd.get('pwv', {})
    local_iono = wd.get('local_iono', {})
    gnss = wd.get('local_gnss', {})
    evo = wd.get('evolution', [])

    sections = []
    sections.append(f"""你是一个专业的气象分析AI（璇玑AGI·钦天监增强版），负责在昆明长水机场(25.09917°N,102.92667°E,2103m)推送前深度分析多源气象数据。

📡 **数据源可靠性说明**
⚠ 所有实时气象数据来自权威专业源，非Open-Meteo：
· 当前实况: met.no (挪威气象局)/wttr.in(ZPPP METAR) → 经问天C引擎融合
· 短期预报: 5模型集成(ECMWF+GFS+WN2+ICON+GEM) → 加权平均
· 航空评估: CCAR-121标准 → 双跑道+13座备降场
· 传统文化: 钦天监(节气/五行/卦象) → 影响预测阈值

## 当前数据（{datetime.now().strftime('%Y-%m-%d %H:%M')} CST）""")
    
    sections.append(f"""### 1. 实时气象
温度: {_safe(outdoor.get('temperature'))}°C | 湿度: {_safe(outdoor.get('humidity'))}% | 气压MSL: {_safe(outdoor.get('pressure_msl'))}hPa
风速: {_safe(outdoor.get('wind_speed'))}km/h | 风向: {_safe(outdoor.get('wind_dir'))}° | 云量: {_safe(outdoor.get('cloud_cover'))}% | UV: {_safe(outdoor.get('uv'))}
天气: {_safe(outdoor.get('weather'))} | 降水: {_safe(outdoor.get('precip'))}mm""")
    
    metar_str = json.dumps(metar, ensure_ascii=False, default=str)[:400] if metar else 'N/A'
    sections.append(f"### 2. METAR (ZPPP)\n{metar_str}")
    
    sections.append(f"""### 3. 多源投票
最终天气: {_safe(ms.get('final_weather'))} | 级别: {_safe(ms.get('level'))} | S4最大: {_safe(ms.get('s4_max'))}
Zambretti: {_safe(ms.get('zambretti'))} | OM3h: {_safe(ms.get('openmeteo_3h'))} | METAR: {_safe(ms.get('metar_now'))}""")
    
    sections.append(f"""### 4. 短临Nowcast
预警: {_safe(nowcast.get('warning_level'))} | 预测: {_safe(nowcast.get('forecast'))} | 评分: {_safe(nowcast.get('score'))}/100
雷暴: {_safe(nowcast.get('thunder_score'))} | 飑线: {_safe(nowcast.get('squall_score'))} | 静止: {_safe(nowcast.get('stationary_score'))} | 风切: {_safe(nowcast.get('wind_shear_score'))}
降水1h: {_safe(nowcast.get('precip_1h_mm'))}mm""")
    
    # 5. 航空评估
    av_path = '/root/data/fusion/aviation_report.txt'
    av_text = open(av_path).read()[:800] if os.path.exists(av_path) else 'N/A'
    sections.append(f"### 5. 航空运行风险评估 (CCAR-121)\n{av_text}")
    
    sections.append(f"### 6. 空间天气\nKp={_safe(swpc.get('kp'))} ({_safe(swpc.get('kp_text'))}) | F10.7={_safe(wd.get('swpc_f107',{}).get('flux_sfu'))} sfu")
    sections.append(f"### 7. 电离层\nS4_GPS={_safe(local_iono.get('s4_gps'))} | S4_BDS={_safe(local_iono.get('s4_bds'))} | 活动={_safe(local_iono.get('activity'))}")
    sections.append(f"### 8. 外部\nNOAA Kp={_safe(ext.get('noaa_kp'))} | F107={_safe(ext.get('noaa_f107'))} | met.no T={_safe(ext.get('metno_temp'))}°C")
    sections.append(f"### 9. 钦天监\n节气: {_safe(imp.get('solar_term'))} | 五行: {_safe(imp.get('wuxing_quadrant'))} | 卦象: {_safe(imp.get('hexagram'))}")
    
    astral_str = json.dumps(astral, ensure_ascii=False, default=str)[:400] if astral else 'N/A'
    sections.append(f"### 10. 星象\n{astral_str}")
    
    wn_str = json.dumps(data.get('weathernext_forecast', {}), ensure_ascii=False, default=str)[:500]
    sections.append(f"### 11. WeatherNext\n{wn_str}")
    
    evo_str = json.dumps(evo[:3], ensure_ascii=False, default=str)[:300] if evo else 'N/A'
    sections.append(f"### 12. 自进化\n{evo_str}")

    prompt = '\n\n'.join(sections)
    
    prompt += '''

---

## 分析要求

请输出JSON对象（不含Markdown代码块），包含以下字段：

- ts: 当前unix时间戳 (int)
- time: 北京时间ISO格式 (str)
- cross_model_consensus: 跨模型共识分析
  - temperature: {consensus_celsius, range_celsius, models_agree (bool), detail}
  - pressure: {consensus_hpa, range_hpa, detail}
  - precipitation: {probability_percent (int 0-100), detail}
- anomaly_detection: 异常检测
  - has_anomaly (bool)
  - anomalies: [{source, field, value, expected_range, severity (low/medium/high), detail}]
- confidence_assessment: 置信度
  - overall_confidence, short_term_confidence, long_term_confidence (high/medium/low)
  - detail
- qintianjian_enhancement: 钦天监增强解读
  - current_term_interpretation (str)
  - wuxing_weather_impact (str)
  - hexagram_insight (str)
  - ancient_wisdom_note (str)
  - overall_assessment (str, 50字内)
- trend_analysis: 趋势研判
  - temperature_trend (rising/stable/falling)
  - pressure_trend (rising/stable/falling)
  - detail (str)
  - key_concern (str)
- alert_recommendation: 告警建议
  - should_alert (bool)
  - alert_level (none/watch/warning/alert)
  - reason (str)
- self_optimization: 自优化建议
  - suggested_weight_adjustments (str)
  - qintianjian_adjustment_suggestion (str)
  - forecast_tuning (str)
- aviation_analysis: 航空运行风险评估 (基于CCAR-121数据)  # ⚠ 修复(2026-09-11): 旧"|- "笔误破坏字段定义格式
  - flight_safety_level (safe/caution/warning/prohibited)
  - recommended_runway (str)
  - crosswind_risk (str)
  - alternate_airports_status (str)
  - qintianjian_aviation_note: 钦天监节气对航空运行的传统文化解读 (str)

注意: 所有分析基于真实数据，严禁编造。数据不足时在detail中诚实说明。
'''
    return prompt


def call_llm(prompt: str) -> Optional[str]:
    key = _get_llm_key()
    if not key:
        print('[LLM] API key not found')
        return None
    
    payload = json.dumps({
        'model': LLM_MODEL,
        'messages': [
            {'role': 'system', 'content': '你是一个严谨的气象分析AI。输出严格JSON，不含Markdown标记。基于给定数据，不编造。'},
            {'role': 'user', 'content': prompt}
        ],
        'temperature': 0.1,
        'max_tokens': 3000
    }).encode()
    
    req = urllib.request.Request(
        LLM_API, data=payload,
        headers={'Authorization': f'Bearer {key}', 'Content-Type': 'application/json'}
    )
    
    for attempt in range(3):
        try:
            # ⚠ 修复(2026-09-11): 120s×3重试最坏366s > 外层feishu预算 — 倒挂必炸服务。
            # 单次超时降到45s, 3次最坏~145s, 外层240s预算内。
            with urllib.request.urlopen(req, timeout=45, context=_ctx()) as resp:
                result = json.loads(resp.read())
                content = result.get('choices', [{}])[0].get('message', {}).get('content', '')
                if content:
                    return content
                print(f'[LLM] attempt {attempt+1} empty response')
        except Exception as e:
            print(f'[LLM] attempt {attempt+1} failed: {e}')
            import time; time.sleep(3)
    return None


def parse_analysis(llm_output: str) -> Dict[str, Any]:
    # try direct parse
    try:
        return json.loads(llm_output)
    except json.JSONDecodeError:
        pass
    # try extracting json block
    # ⚠ 修复(2026-09-11): 旧正则 r'```(?:json)?\\s*([\\s\\S]*?)```' 双重转义
    # (\\s是字面反斜杠+s) → markdown代码块提取分支永不匹配, 全靠大括号兜底
    m = re.search(r'```(?:json)?\s*([\s\S]*?)```', llm_output)
    if m:
        try:
            return json.loads(m.group(1))
        except json.JSONDecodeError:
            pass
    # try outermost braces
    start = llm_output.find('{')
    end = llm_output.rfind('}')
    if start >= 0 and end > start:
        try:
            return json.loads(llm_output[start:end+1])
        except json.JSONDecodeError:
            pass
    print('[LLM] JSON parse failed')
    return {'ts': int(datetime.now().timestamp()), 'parse_failed': True}


def merge_analysis(data: Dict[str, Any], analysis: Dict[str, Any]) -> Dict[str, Any]:
    now = datetime.now()
    if 'ts' not in analysis:
        analysis['ts'] = int(now.timestamp())
    if 'time' not in analysis:
        analysis['time'] = now.strftime('%Y-%m-%d %H:%M')
    analysis['data_sources'] = {
        'wentian_generated_at': data.get('generated_at', ''),
        'has_weathernext': bool(data.get('weathernext_forecast', {})),
        'has_imperial': bool(data.get('imperial_enhancement', {})),
    }
    analysis['version'] = 'llm-weather-analyst v1.0'
    analysis['model'] = LLM_MODEL
    return analysis


def update_accuracy_tracking(analysis: Dict[str, Any]):
    track = _load_json(TRACKING_FILE, {'entries': [], 'adjustments': []})
    if not isinstance(track, dict):
        track = {'entries': [], 'adjustments': []}
    entry = {
        'ts': analysis.get('ts', int(datetime.now().timestamp())),
        'time': analysis.get('time', datetime.now().strftime('%Y-%m-%d %H:%M')),
        'consensus_t': analysis.get('cross_model_consensus', {}).get('temperature', {}).get('consensus_celsius'),
        'consensus_p': analysis.get('cross_model_consensus', {}).get('pressure', {}).get('consensus_hpa'),
        'precip_prob': analysis.get('cross_model_consensus', {}).get('precipitation', {}).get('probability_percent'),
        'confidence': analysis.get('confidence_assessment', {}).get('overall_confidence', 'medium'),
        'verified': False, 'actual_t': None, 'actual_p': None, 'actual_precip': None
    }
    track['entries'].append(entry)
    if len(track['entries']) > 30:
        track['entries'] = track['entries'][-30:]
    with open(TRACKING_FILE, 'w') as f:
        json.dump(track, f, ensure_ascii=False, indent=2)
    print(f'[Track] saved ({len(track["entries"])} entries)')


def verify_past_entries():
    track = _load_json(TRACKING_FILE, {'entries': [], 'adjustments': []})
    if not isinstance(track, dict):
        return
    entries = track.get('entries', [])
    wd = collect_data().get('wentian', {})
    current_t = wd.get('outdoor', {}).get('temperature')
    current_p = wd.get('outdoor', {}).get('pressure_msl')
    now = datetime.now()
    verified = 0
    for entry in entries:
        if entry.get('verified'):
            continue
        entry_ts = entry.get('ts', 0)
        if entry_ts and (now - datetime.fromtimestamp(entry_ts)).total_seconds() >= 7200:
            if current_t is not None and entry.get('consensus_t') is not None:
                entry['actual_t'] = current_t
                entry['t_error'] = round(abs(entry['consensus_t'] - current_t), 2)
            if current_p is not None and entry.get('consensus_p') is not None:
                entry['actual_p'] = current_p
                entry['p_error'] = round(abs(entry['consensus_p'] - current_p), 2)
            entry['verified'] = True
            verified += 1
    if verified:
        print(f'[Track] verified {verified} entries')
        t_errors = [e.get('t_error') for e in entries if e.get('t_error') is not None]
        if len(t_errors) >= 3:
            avg = sum(t_errors) / len(t_errors)
            print(f'[Track] avg T error: {avg:.2f}C ({len(t_errors)} samples)')
            if avg > 5:
                print(f'[Track] WARNING: high T error {avg:.1f}C')
        with open(TRACKING_FILE, 'w') as f:
            json.dump(track, f, ensure_ascii=False, indent=2)


def main() -> bool:
    print('=== LLM Weather Analyst v1.0 ===')
    print('[Self-opt] verifying past...')
    verify_past_entries()
    
    print('[Data] collecting...')
    data = collect_data()
    if not data.get('wentian'):
        print('[Data] WARNING: wentian_latest.json unavailable')
    
    print('[LLM] building prompt...')
    prompt = build_prompt(data)
    print(f'[LLM] prompt length: {len(prompt)} chars')
    
    print(f'[LLM] calling {LLM_MODEL}...')
    llm_out = call_llm(prompt)
    if not llm_out:
        print('[LLM] FAILED')
        return False
    print(f'[LLM] response length: {len(llm_out)} chars')
    
    analysis = parse_analysis(llm_out)
    analysis = merge_analysis(data, analysis)
    
    with open(LLM_ANALYSIS_OUT, 'w') as f:
        json.dump(analysis, f, ensure_ascii=False, indent=2)
    print(f'[Output] saved to {LLM_ANALYSIS_OUT}')
    
    update_accuracy_tracking(analysis)
    
    conf = analysis.get('confidence_assessment', {}).get('overall_confidence', '?')
    anomaly = analysis.get('anomaly_detection', {}).get('has_anomaly', False)
    alert = analysis.get('alert_recommendation', {}).get('alert_level', 'none')
    print(f'[Summary] confidence: {conf} | anomaly: {"YES" if anomaly else "no"} | alert: {alert}')
    return True


if __name__ == '__main__':
    sys.exit(0 if main() else 1)
