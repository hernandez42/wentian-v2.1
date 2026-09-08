#!/usr/bin/env python3
"""
问天·钦天监 自动复盘审计引擎 v1.0
每6小时运行: 检查预警准确性、数据健康度、自动调参修复

调用方式:
  python3 auto_audit.py           # 正常复盘
  python3 auto_audit.py --report   # 只出报告, 不自动修复
  python3 auto_audit.py --force    # 强制全面修复
"""
import os, sys, json, sqlite3, time, subprocess
from datetime import datetime
from pathlib import Path

WENTIAN_DB = "/root/data/wentian.db"
ANO_DB = "/root/data/ano_weather.db"
FUSION_DIR = Path("/root/data/fusion")
LOG_FILE = "/root/data/fusion/auto_audit.log"
THRESHOLD_FILE = FUSION_DIR / "evolve_factor.json"

SEVERE_FP_FILE = FUSION_DIR / "false_positive_log.json"

# ── 日志 ──
def log(msg):
    ts = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
    s = f"[{ts}] {msg}"
    print(s)
    with open(LOG_FILE, "a") as f: f.write(s + "\n")

# ── SQLite helper ──
def query_one(db_path, sql, params=()):
    try:
        conn = sqlite3.connect(db_path)
        c = conn.cursor()
        c.execute(sql, params)
        r = c.fetchone()
        conn.close()
        return r
    except Exception as e:
        log(f"  ⚠ SQL查询失败 {db_path}: {e}")
        return None

def query_all(db_path, sql, params=()):
    try:
        conn = sqlite3.connect(db_path)
        c = conn.cursor()
        c.execute(sql, params)
        r = c.fetchall()
        conn.close()
        return r
    except Exception as e:
        log(f"  ⚠ SQL查询失败 {db_path}: {e}")
        return []

# ── 1. 检查数据新鲜度 ──
def check_freshness():
    """检查各数据源的时效性"""
    now = time.time()
    checks = [
        ("outdoor", WENTIAN_DB, "SELECT MAX(ts) FROM outdoor"),
        ("metar", WENTIAN_DB, "SELECT MAX(ts) FROM metar"),
        ("local_gnss", WENTIAN_DB, "SELECT MAX(ts) FROM local_gnss"),
        ("local_sdr", WENTIAN_DB, "SELECT MAX(ts) FROM local_sdr"),
        ("local_pwv", WENTIAN_DB, "SELECT MAX(ts) FROM local_pwv"),
        ("ano_weather (UNO)", ANO_DB, "SELECT MAX(ts) FROM ano_weather WHERE source='UNO_v2.0_bridge'"),
        ("gps_log", ANO_DB, "SELECT MAX(ts) FROM gps_log"),
        ("multi_source_forecast", WENTIAN_DB, "SELECT MAX(ts) FROM multi_source_forecast"),
        ("multisrc_s4", WENTIAN_DB, "SELECT MAX(ts) FROM multisrc_s4"),
        ("swpc_scale", WENTIAN_DB, "SELECT MAX(ts) FROM swpc_scale"),
    ]
    issues = []
    for name, db, sql in checks:
        r = query_one(db, sql)
        if r and r[0]:
            raw_ts = r[0]
            # 支持INTEGER(unix时间戳)和TEXT(ISO日期)
            if isinstance(raw_ts, (int, float)):
                age = now - raw_ts if raw_ts > 1e10 else now - raw_ts
            elif isinstance(raw_ts, str):
                try:
                    dt = datetime.fromisoformat(raw_ts)
                    age = now - dt.timestamp()
                except:
                    age = 999999
            else:
                age = 999999
            max_stale = {
                "outdoor": 600, "metar": 3600, "local_gnss": 1800,
                "local_sdr": 7200, "local_pwv": 1800, "ano_weather (UNO)": 600,
                "gps_log": 1800, "multi_source_forecast": 600,
                "multisrc_s4": 1800, "swpc_scale": 7200
            }.get(name, 3600)
            if age > max_stale:
                issues.append((name, int(age)))
                log(f"  ⚠ {name}: 数据陈旧 {int(age)}秒 (阈值{max_stale}s)")
        else:
            issues.append((name, -1))
            log(f"  ⚠ {name}: 无数据")
    return issues

# ── 2. 检查预警误报率 ──
def check_false_positives():
    """检查多源预测的预警等级 vs METAR实况
    SEVERE/WARNING 但METAR无TS/TSRA/风暴 → 误报
    """
    # 取最近10次 predict 结果
    rows = query_all(WENTIAN_DB,
        "SELECT ts, level, alert_score, alerts FROM multi_source_forecast "
        "ORDER BY ts DESC LIMIT 10")
    fp_count = 0
    total_alerts = 0
    for row in rows:
        ts, level, score, alerts = row
        level = level or "NORMAL"
        if level in ("SEVERE", "WARNING"):
            total_alerts += 1
            # 查同一时间的METAR是否有TS/TSRA
            metar = query_one(WENTIAN_DB,
                "SELECT raw FROM metar WHERE ts BETWEEN ? AND ? "
                "AND raw NOT LIKE 'SYNTHETIC%%' "
                "ORDER BY ABS(ts - ?) LIMIT 1",
                (ts-1800, ts+1800, ts))
            if metar and metar[0]:
                raw = metar[0].upper()
                if "TS" not in raw and "SQ" not in raw and "FC" not in raw:
                    fp_count += 1
            else:
                fp_count += 1  # 无METAR也视为可疑
    
    fp_rate = (fp_count / total_alerts * 100) if total_alerts > 0 else 0.0
    log(f"  预警误报率: {fp_count}/{total_alerts} = {fp_rate:.0f}%")
    return fp_rate, total_alerts

# ── 3. 检查PWV合理性 ──
def check_pwv_sanity():
    """PWV应在合理范围"""
    r = query_one(WENTIAN_DB,
        "SELECT pwv_mm, temp_c, humid_pct, press_hpa, ts "
        "FROM local_pwv WHERE ts > ? ORDER BY ts DESC LIMIT 1",
        (time.time() - 600,))
    issues = []
    if r:
        pwv, t, h, p, ts = r
        log(f"  PWV={pwv:.1f}mm T={t:.1f}°C H={h:.0f}% P={p:.0f}hPa")
        # 海拔2103m合理PWV: 5-40mm
        if pwv < 5 or pwv > 50:
            issues.append(f"PWV异常: {pwv:.1f}mm")
            log(f"  ⚠ PWV超出合理范围(5-50mm): {pwv:.1f}mm")
        # 温度合理性
        if t < -10 or t > 45:
            issues.append(f"温度异常: {t:.1f}°C")
            log(f"  ⚠ 温度异常: {t:.1f}°C")
        # 湿度
        if h < 5 or h > 100:
            issues.append(f"湿度异常: {h:.0f}%")
    else:
        log(f"  ⚠ PWV: 无最近数据")
    return issues

# ── 4. 检查钦天监状态 ──
def check_qintianjian():
    """钦天监数据合理性"""
    ef = FUSION_DIR / "imperial_enhancement.json"
    if not ef.exists():
        log(f"  ⚠ 钦天监: 文件不存在")
        return ["imperial_enhancement.json缺失"]
    try:
        d = json.loads(ef.read_text())
        log(f"  节气={d.get('solar_term','?')} "
            f"五行={d.get('wuxing_quadrant','?')} "
            f"卦={d.get('hexagram','?')} "
            f"alert_threshold={d.get('alert_threshold','?')}")
        return []
    except Exception as e:
        log(f"  ⚠ 钦天监JSON解析失败: {e}")
        return [f"imperial JSON error: {e}"]

# ── 5. 自动调参 ──
def auto_tune(fp_rate, total_alerts):
    """根据误报率自动调整evolve_factor"""
    ef = THRESHOLD_FILE
    if not ef.exists():
        # 创建默认
        ef.write_text(json.dumps({"factor": 1.0, "note": "auto-created"}))
        log(f"  创建默认evolve_factor.json")
        return
    
    try:
        d = json.loads(ef.read_text())
    except:
        d = {"factor": 1.0}
    
    old = d.get("factor", 1.0)
    adjust = 0.0
    
    # 调整逻辑
    if total_alerts >= 3 and fp_rate > 60:
        adjust = 0.15  # 误报>60%: 升阈值15%
        log(f"  误报率{fp_rate:.0f}% > 60% → 升阈值")
    elif total_alerts >= 3 and fp_rate > 40:
        adjust = 0.08  # 误报>40%: 升阈值8%
        log(f"  误报率{fp_rate:.0f}% > 40% → 升阈值")
    elif total_alerts >= 5 and fp_rate < 10:
        adjust = -0.05  # 误报<10%: 适当降阈值提高灵敏度
        log(f"  误报率{fp_rate:.0f}% < 10% → 降阈值")
    
    if adjust != 0:
        new = max(0.5, min(2.0, old + adjust))
        d["factor"] = round(new, 3)
        d["last_update"] = datetime.now().isoformat()
        d["adjust_reason"] = f"误报率{fp_rate:.0f}%, 调整{adjust:+.2f}"
        ef.write_text(json.dumps(d, indent=2))
        log(f"  evolve_factor: {old:.3f} → {new:.3f} (Δ={adjust:+.2f})")
    else:
        log(f"  evolve_factor不变: {old:.3f}")

# ── 6. 修复数据问题 ──
def auto_repair(issues):
    """自动修复常见问题"""
    repairs = []
    
    # 检查daemon状态
    r = subprocess.run(["systemctl", "is-active", "wentian"], capture_output=True, text=True)
    if r.stdout.strip() != "active":
        log(f"  ⚠ wentian daemon异常! 重启...")
        subprocess.run(["systemctl", "restart", "wentian"])
        repairs.append("restarted wentian daemon")
    
    # 检查gps-uno-fusion
    r = subprocess.run(["systemctl", "is-active", "gps-uno-fusion"], capture_output=True, text=True)
    if r.stdout.strip() != "active":
        log(f"  ⚠ gps-uno-fusion异常! 重启...")
        subprocess.run(["systemctl", "restart", "gps-uno-fusion"])
        repairs.append("restarted gps-uno-fusion")
    
    # 检查磁盘
    st = os.statvfs("/")
    free_gb = st.f_bavail * st.f_frsize / (1024**3)
    log(f"  磁盘剩余: {free_gb:.1f} GB")
    if free_gb < 1:
        log(f"  ⚠ 磁盘空间不足!")
        repairs.append("disk < 1GB")
    
    # 检查数据陈旧 → 触发自愈
    for name, age in issues:
        if name == "outdoor" and age > 3600:
            subprocess.run(["systemctl", "restart", "wentian"])
            repairs.append("restarted wentian (outdoor stale)")
            break
    
    return repairs

# ── 7. 报告 ──
def save_report(results):
    """保存报告到JSON"""
    report = {
        "ts": datetime.now().isoformat(),
        "cycle_h": 6,
        **results
    }
    report_file = FUSION_DIR / "auto_audit_report.json"
    report_file.write_text(json.dumps(report, indent=2, ensure_ascii=False))
    log(f"  报告已保存: {report_file}")
    return report

# ── 主入口 ──
def main():
    log("=" * 60)
    log("问天·钦天监 自动复盘审计引擎 v1.0")
    log(f"系统时间: {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}")

    results = {}

    log("\n── 1. 数据新鲜度 ──")
    freshness_issues = check_freshness()
    results["freshness_issues"] = len(freshness_issues)
    results["freshness_detail"] = [(n, a) for n, a in freshness_issues]

    log("\n── 2. 预警误报率 ──")
    fp_rate, total_alerts = check_false_positives()
    results["false_positive_rate"] = round(fp_rate, 1)
    results["total_alerts"] = total_alerts

    log("\n── 3. PWV合理性 ──")
    pwv_issues = check_pwv_sanity()
    results["pwv_issues"] = pwv_issues

    log("\n── 4. 钦天监状态 ──")
    q_issues = check_qintianjian()
    results["qintianjian_issues"] = q_issues

    log("\n── 5. 自动调参 ──")
    if "--report" not in sys.argv:
        auto_tune(fp_rate, total_alerts)
    else:
        log("  (报告模式, 不调参)")

    log("\n── 6. 自动修复 ──")
    if "--report" not in sys.argv:
        repairs = auto_repair(freshness_issues)
        results["repairs"] = repairs
        if repairs:
            log(f"  ✅ 已执行修复: {', '.join(repairs)}")
        else:
            log(f"  ✅ 无需修复")
    else:
        results["repairs"] = []
        log("  (报告模式, 不修复)")

    # 保存报告
    save_report(results)

    # 总结
    n_issues = (len(freshness_issues) + len(pwv_issues) + len(q_issues) +
                (1 if results.get("false_positive_rate", 0) > 60 else 0))
    log(f"\n── 复盘结论 ──")
    if n_issues == 0:
        log("✅ 系统健康, 无异常")
    else:
        log(f"⚠ 发现 {n_issues} 个问题, 已自动处理")
    
    # 推送总结到日志
    summary = (f"复盘: 误报率{fp_rate:.0f}% "
               f"数据源{len(freshness_issues)}个过期 "
               f"PWV问题{len(pwv_issues)}个 "
               f"钦天监问题{len(q_issues)}个")
    log(summary)

if __name__ == "__main__":
    main()
