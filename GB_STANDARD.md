# 问天项目 · GB/T 35663-2017 国标术语对齐规范

> 标准号: GB/T 35663-2017《天气预报基本术语》
> 适用范围: 问天气象站 v2.1 全部代码、数据、架构、文档
> 目的: 确保术语统一、定义准确、与国际/国家标准一致

---

## 一、国标核心术语 ↔ 问天代码映射

### 4.1 天气现象 (Weather Phenomena)

| 国标术语 | GB/T编号 | 问天代码变量/字段 | 问天显示名称 | 对齐状态 |
|----------|----------|-------------------|-------------|----------|
| 雷暴 thunderstorm | 4.1.1 | `thunder_score` / `THUNDER` | ⛈雷暴 | ✅ 已对齐 |
| 飑线 squall line | 4.1.11 | `squall_score` / `SQUALL` | 🌪飑线 | ✅ 已对齐 |
| 冷锋 cold front | 4.2.7 | `false_cold_score` / `FALSE_COLD` | ❄假冷锋 | ⚠ 非国标术语，已加注释 |
| 准静止锋 quasi-stationary front | 4.2.9 | `stationary_score` / `STATIONARY` | 🌫准静止锋 | ✅ 已对齐 |
| 风切变 wind shear | 4.3.5 | `wind_shear_score` / `WIND_SHEAR` | 💨风切变 | ✅ 已对齐 |
| 降水 precipitation | 4.1.14 | `precipitation` (outdoor) | 降水 | ✅ 已对齐 |
| 暴雨 rainstorm | 4.1.16 | `RAINSTORM` / `precip_intensity` | 🌧暴雨 | ✅ 已对齐 |
| 小雨/中雨/大雨 light/moderate/heavy rain | 4.1.15-17 | `wt_metar_precip_level()` | 降水强度 | ✅ 已显式分级 |
| 雾 fog | 4.1.19 | METAR `FG` 标记 | 雾 | ✅ 已对齐 |
| 霾 haze | 4.2.20 | 空气质量AQI | 霾 | ✅ 已对齐 |

### 4.2 天气系统 (Weather Systems)

| 国标术语 | GB/T编号 | 问天代码 | 对齐状态 |
|----------|----------|----------|----------|
| 锋 front | 4.2.6 | `FALSE_COLD`/`STATIONARY` | ✅ |
| 低压 low pressure | 4.3.1 | METAR气压分析 | ✅ |
| 高压 high pressure | 4.3.2 | METAR气压分析 | ✅ |
| 低压槽 trough | 4.3.3 | 气压梯度分析 | ⚠ 未显式检测 |
| 高压脊 ridge | 4.3.4 | 气压梯度分析 | ⚠ 未显式检测 |

### 4.3 气象要素 (Meteorological Elements)

| 国标术语 | GB/T编号 | 问天代码变量 | 问天DB列 | 对齐状态 |
|----------|----------|-------------|----------|----------|
| 气温 air temperature | 5.1.1 | `temperature` / `temp_c` | `temperature` | ✅ |
| 气压 air pressure | 5.2.1 | `pressure_msl` / `press_hpa` | `pressure` | ✅ |
| 相对湿度 relative humidity | 5.4.1 | `humidity` / `humid_pct` | `humidity` | ✅ |
| 降水量 precipitation amount | 5.5.1 | `precipitation` | `precipitation` | ✅ |
| 能见度 visibility | 5.6.1 | `visibility` / `visibility_m` | `visibility` | ✅ |
| 风向 wind direction | 5.7.1 | `wind_dir` / `wd_arr` | `wind_dir` | ✅ |
| 风速 wind speed | 5.7.2 | `wind_speed` / `ws_arr` | `wind_speed` | ✅ |
| 可降水量 precipitable water | — | `pwv_mm` | `pwv_mm` | ✅ (扩展要素) |

### 4.4 预报时效 (Forecast Lead Time)

| 国标术语 | GB/T编号 | 问天实现 | 对齐状态 |
|----------|----------|----------|----------|
| 短时预报 very short range forecast | 6.1.2 | `api_nowcast.c` (0-30min) | ✅ 对应"临近预报" |
| 临近预报 nowcasting | 6.1.1 | `nowcast` 表/模块 | ✅ 完全对齐 |
| 短期预报 short range forecast | 6.1.3 | Open-Meteo 1-3h | ✅ (外部API) |
| 中期预报 medium range forecast | 6.1.4 | Open-Meteo 3-24h | ✅ (外部API) |

---

## 二、问天代码术语规范

### 2.1 命名铁律

**变量/字段命名** — 必须使用国标英文术语或拼音，禁止自造词：

| ✅ 正确 | ❌ 错误 | 原因 |
|---------|---------|------|
| `thunder_score` | `storm_score` | 国标"雷暴"≠泛义"风暴" |
| `squall_line` | `gust_front` | 国标"飑线"非"阵风锋" |
| `precipitation` | `rainfall` | 降水≠降雨(含雪/雹等) |
| `wind_shear` | `wind_diff` | 风切变有明确定义 |
| `false_cold_front` | `fake_cold` | "假冷锋"非"假冷" |

**天气型等级命名** — 对齐GB/T 28594-2012《临近预报》：

| 问天当前 | 国标建议 | 说明 |
|----------|----------|------|
| `CALM` | `NORMAL` | 国标无"平静"级，用"正常" |
| `WATCH` | `WATCH` | ✅ 保留(国际通用) |
| `WARNING` | `WARNING` | ✅ 保留 |
| `SEVERE` | `SEVERE` | ✅ 保留 |

### 2.2 检测阈值对齐国标

#### 雷暴 (GB/T 4.1.1)
国标定义: "伴有雷声和闪电的天气现象"
问天实现: PWV急升+气压降+温度降(间接指标)
⚠️ **问题**: 问天无直接雷电检测(无闪电传感器)
→ 建议: 增加METAR `TS`/`TSRA`标记作为雷暴确认条件

#### 飑线 (GB/T 4.1.11)
国标定义: "带状的雷暴群所构成的风向、风速突变的强对流天气"
问天实现: 气压骤升+风向突变+PWV骤降 ✅
⚠️ **问题**: 问天未检测"带状雷暴群"(需雷达/卫星)
→ 当前SDR+GNSS+UNO三路相干可间接检测，但需标注"间接推断"

#### 假冷锋 (非国标术语)
⚠️ **问题**: "假冷锋"不是GB/T标准术语
→ 国标对应: "冷锋" cold front (4.2.7) + "温度骤降"现象
→ 建议: 代码注释明确标注"非国标术语，指无降水伴随的温度骤降现象，类冷锋特征"

#### 准静止锋 (GB/T 4.2.9)
国标定义: "移动缓慢、很少移动的锋"
问天实现: 持续高湿+温度气压稳定+连续降水 ✅

#### 风切变 (GB/T 4.3.5)
国标定义: "风向和/或风速在短距离内的剧烈变化"
问天实现: 风向突变+风速差 ✅
⚠️ **问题**: 国标定义是"空间变化"，问天检测的是"时间变化"(METAR时序)
→ 建议: 标注"基于时序的风切变间接检测"

### 2.3 降水强度分级对齐 (GB/T 28592-2012)

| 等级 | 12h降水量(mm) | 问天当前 | 需更新 |
|------|---------------|----------|--------|
| 小雨 light rain | <5.0 | ✅ METAR RA | — |
| 中雨 moderate rain | 5.0-9.9 | ✅ METAR RA | — |
| 大雨 heavy rain | 10.0-24.9 | ✅ METAR +RA | — |
| 暴雨 rainstorm | 25.0-49.9 | ⚠️ 仅图标 | 需显式分级 |
| 大暴雨 heavy rainstorm | 50.0-99.9 | ❌ 未实现 | 建议加 |
| 特大暴雨 extraordinary rainstorm | ≥100.0 | ❌ 未实现 | 建议加 |

---

## 三、数据库术语规范

### 3.1 列名标准化

| 当前列名 | 建议改为 | 依据 |
|----------|----------|------|
| `temperature` | `air_temperature` | GB/T 5.1.1 气温 |
| `pressure` | `air_pressure` | GB/T 5.2.1 气压 |
| `humidity` | `relative_humidity` | GB/T 5.4.1 |
| `precipitation` | `precipitation_amount` | GB/T 5.5.1 |
| `wind_dir` | `wind_direction` | GB/T 5.7.1 |
| `wind_speed` | `wind_speed` | ✅ 保留 |
| `visibility` | `horizontal_visibility` | GB/T 5.6.1 |
| `nowcast` 表 `level` | `warning_level` | 避免与天气型混淆 |

### 3.2 数据字典

每个表必须有注释说明术语定义：

```sql
-- 可降水量 (precipitable water): 
-- 气柱中全部水汽凝结并降落到地面的水量(mm)
-- 参考: WMO Guide to Meteorological Instruments (CIMO)
CREATE TABLE local_pwv (
    ts INTEGER PRIMARY KEY,
    pwv_mm REAL NOT NULL,  -- 可降水量(mm)
    ...
);
```

---

## 四、架构术语一致性

### 4.1 模块命名

| 当前模块 | 国标对齐建议 | 说明 |
|----------|-------------|------|
| `api_nowcast.c` | ✅ 保留 | GB/T 6.1.1 nowcasting |
| `api_correlate.c` | `api_radar_correl.c` | 更明确"雷达相干" |
| `api_pwv.c` | ✅ 保留 | PWV是标准缩写 |
| `api_gnss_ion.c` | ✅ 保留 | 电离层ionosphere |

### 4.2 输出JSON字段

`nowcast.json` 当前字段需对齐国标：

| 当前字段 | 建议改为 | 原因 |
|----------|----------|------|
| `primary_type` | `primary_weather_type` | 更明确 |
| `level` | `warning_level` | 避免歧义 |
| `score` | `nowcast_score` | 明确是nowcast评分 |
| `thunder_score` | ✅ 保留 | — |
| `squall_score` | ✅ 保留 | — |
| `false_cold_score` | ⚠️ 加注释 | 非国标术语 |

---

## 五、已发现的术语bug及修复计划

| # | 位置 | 问题 | 严重度 | 修复 |
|---|------|------|--------|------|
| 1 | `api_nowcast.c` "假冷锋" | 非国标术语 | 中 | 加注释"非国标，类冷锋现象" |
| 2 | `push_alert.py` "准静止封" | 错别字(封→锋) | 高 | 立即修复 |
| 3 | `api_nowcast.c` `storm_score` | "风暴"泛义，国标用"雷暴" | 中 | 改为 `thunder_score` |
| 4 | `wentian.h` `wt_iono_t` | "iono"缩写不标准 | 低 | 保留(行业惯例) |
| 5 | `METAR` 解析 | 降水强度无显式分级 | 中 | 增加light/moderate/heavy/rainstorm判断 |
| 6 | `nowcast.json` `level`字段 | 与天气型`primary_type`混淆 | 中 | 改为 `warning_level` |
| 7 | `wind_dir` 风向 | 用0-360°但无"静风"标记 | 低 | 增加 `wind_calm` 标记 |
| 8 | `api_correlate.c` `matched_pattern` | 用数字ID无字符串 | 中 | 增加 `pattern_name` 字段(已存在) |

---

## 六、自优化/自进化机制

### 6.1 术语一致性自检脚本

```python
#!/usr/bin/env python3
"""问天术语一致性检查 — 每次代码变更后自动运行"""
import re, sys

TERMS_STANDARD = {
    'thunderstorm': ['雷暴', 'thunder'],
    'squall_line': ['飑线', 'squall'],
    'cold_front': ['冷锋', 'cold front'],
    'stationary_front': ['准静止锋', 'stationary'],
    'wind_shear': ['风切变', 'shear'],
    'precipitation': ['降水', 'precip'],
    'nowcast': ['临近预报', 'nowcast'],
}

def check_file(path):
    with open(path) as f: content = f.read()
    issues = []
    # 检查错别字
    if '准静止封' in content:
        issues.append(f"错别字: '准静止封'→'准静止锋'")
    if 'fake_cold' in content or '假冷' in content:
        issues.append(f"非标准术语: '假冷锋'需加国标注释")
    return issues
```

### 6.2 阈值自校准

厄尔尼诺期间自动调整检测阈值（已在`ENSO_MODE`宏中预留）：

```c
#if ENSO_MODE
  /* 超强厄尔尼诺期: 降低检测阈值15% */
  #define SQUALL_PRESS_RISE    1.7   /* 原2.0 hPa */
  #define PWV_SLOPE_WATCH      0.85  /* 原1.0 mm/15min */
#endif
```

---

## 七、执行清单

### 立即修复 (P0)
- [x] `push_alert.py`: "准静止封"→"准静止锋" (错别字) — 已修复
- [x] `api_nowcast.c`: `storm_score` → `thunder_score` (术语标准化) — 已修复
- [x] `nowcast.json`: `level` → `warning_level` (字段歧义) — 已修复

### 近期优化 (P1)
- [x] METAR降水强度显式分级 (light/moderate/heavy/rainstorm) — `wt_metar_precip_level()` 已实现
- [x] "假冷锋"添加国标注释说明 — 已加注释，标注非GB/T标准术语
- [x] 数据库列名标准化 — `warning_level`/`precip_intensity`/`precip_1h_mm`/`false_cold_note` 已添加
- [x] 雷暴检测增加METAR `TS`标记确认条件 — `score_thunderstorm()` 已接收METAR raw，TS/TSRA命中+25分

### 长期完善 (P2)
- [ ] 增加低压槽/高压脊检测
- [ ] 大暴雨/特大暴雨分级
- [ ] 术语一致性自检脚本集成CI
- [ ] 空间风切变检测(需多站数据)

---

*文档版本: v2.1 | 最后更新: 2026-09-04 | 状态: 全部P0/P1已完成，P2待迭代*