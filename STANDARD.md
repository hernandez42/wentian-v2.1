# 问天 v2.0 · 标准化架构手册

> 主人: 朱涛 BG8SBA | 位置: 昆明长水机场楼顶(25.08°N, 102.91°E, 2103m)
> 硬件: RK3588 EVB4 / Armbian 24.04 / aarch64
> 模式: 厄尔尼诺战备模式 (超强厄尔尼诺, 持续至2027年2月)

---

## 一、数据库标准

### 1.1 核心库: `/root/data/wentian.db` (SQLite3)

**命名规范**:
- 表名: `前缀_含义`，前缀区分数据源
  - `local_` = 本机传感器采集
  - `remote_` = API远程数据
  - `derived_` = 计算衍生数据
- 列名: `snake_case`，单位后缀明确
  - `_c` = °C, `_hpa` = hPa, `_mm` = mm, `_pct` = %, `_m/s` = m/s
- 时间戳: **统一 Unix epoch (int64)**，禁止秒-日格式
- 主键: 所有表必须有 `ts` (时间戳) 或 `id` (自增)

**核心表清单**:

| 表名 | 用途 | 更新频率 | 保留天数 |
|------|------|----------|----------|
| `outdoor` | Open-Meteo户外数据 | 5min | 30 |
| `metar` | ZPPP机场METAR | 10min | 90 |
| `local_uno` | UNO传感器(柜内) | 30s | 7 |
| `local_gnss` | GNSS原始观测 | 1min | 30 |
|| `local_pwv` | **C版PWV反演** | 60s | 30 |
|| `local_iono` | **C版电离层闪烁** | 60s | 30 |
| `local_sdr` | SDR频谱快照 | 手动 | 30 |
| `nowcast` | 短临评分(全天气型) | 60s | 90 |
| `radar_correl` | 软件雷达相干 | 60s | 30 |
| `kalman_fused` | Kalman气压融合 | 60s | 30 |

**Schema变更流程**:
```
1. 停daemon → 2. PRAGMA table_info(表名) 查现有列 → 3. ALTER TABLE ADD COLUMN → 4. 重启
```
SQLite不支持 `IF NOT EXISTS COLUMN`，必须先查再加。

### 1.2 CSV文件 (仅用于历史备份)

- `/root/data/fusion/pwv_history.csv` — **已弃用**，数据源改为DB
- 如需导出: `sqlite3 wentian.db ".mode csv" "SELECT * FROM local_pwv" > pwv_history.csv`

### 1.3 zvec向量数据库 (未来预留)

**当前决策**: 不使用。理由:
- zvec是向量相似度搜索引擎，不是时序数据库
- 问天当前数据量(METAR 10min + PWV 1min) SQLite完全胜任
- **启用条件**: 当引入GNSS 1Hz高频数据或SDR流式数据，或需要历史天气模式向量匹配时再评估

---

## 二、代码标准

### 2.1 语言分层

| 层级 | 语言 | 职责 | 文件 |
|------|------|------|------|
| **核心层** | C | 物理模型、评分算法、数据融合、DB操作 | `api_*.c` |
| **胶水层** | Python | HTTP推送、离线评分、串口桥接 | `push_alert.py`, `score_forecast.py`, `uno_bridge.py` |
| **配置层** | Shell/JSON | 启动脚本、cron、API配置 | `*.sh`, `*.json` |

**铁律**: 核心物理模型(PWV反演、Nowcast评分、雷达相干)必须用C，Python只做IO和调度。

### 2.2 C编码规范

**头文件**:
```c
/* 文件头注释: 功能、版本、作者、修改历史 */
#ifndef WENTIAN_XXX_H
#define WENTIAN_XXX_H

/* 只放: struct定义、函数声明、宏常量 */
/* 不放: 函数实现、全局变量定义 */

#endif
```

**函数命名**: `wt_模块_动作_对象()`
- `wt_nowcast_run()` — nowcast模块运行
- `wt_pwv_revert()` — PWV反演
- `wt_radar_correlate()` — 雷达相干

**内存安全**:
- 禁止 `strcpy`/`strcat`/`sprintf` → 用 `strncpy`/`strncat`/`snprintf`
- 所有 `snprintf` 用 `SAFE_SNPRINTF` 宏包裹
- 栈上数组必须初始化: `double buf[32] = {0}`
- 动态分配: 检查返回值，用完 `free()`

**编译要求**:
```
gcc -O2 -Wall -Wextra -o wentian ...
```
**零警告**是硬指标。新文件必须零警告，旧文件警告逐步消除。

**错误处理**:
- SQLite: 检查返回值，`sqlite3_errmsg()` 记录错误
- 文件: `fopen` 后检查 `NULL`
- 网络: `curl_easy_perform` 检查 `CURLE_OK`
- 失败时 `return -1` 或 `errno`，不静默吞掉

### 2.3 注释规范

每段代码必须有"为什么"的注释，不只是"做什么":
```c
/* 坏: 计算PWV */
pwv = zwd * lambda;

/* 好: Saastamoinen模型 λ=6.5, 将天顶湿延迟转为可降水量;
 * 长水海拔2103m, 此λ值经本地METAR标定(原模型λ=6.0偏差12%) */
pwv = zwd * PWV_LAMBDA;
```

---

## 三、架构标准

### 3.1 问天24维度数据流

```
[外部API]                          [本地硬件]
  ├─ Open-Meteo (户外预报)          ├─ UNO (Temp/Hum/Press)
  ├─ SWPC (空间天气)                ├─ GNSS ATGM336H (PWV/S4/SNR)
  ├─ METAR ZPPP (机场实况)          ├─ SDR RTL2838U (频谱)
  └─ Chronos/Kriging (预测)         └─ GPS北斗串口 (NMEA)
         │                              │
         ▼                              ▼
    ┌─────────────────────────────────────────┐
    │           wentian daemon (C)             │
    │  Step 1-16: 数据采集+Kalman融合          │
    │  Step 17: GNSS PWV实时反演(C)  ← api_pwv.c
    │  Step 18: 短临Nowcasting(全天气型) ← api_nowcast.c
    │  Step 19: 软件雷达三路相干         ← api_correlate.c
    │  Step 20: DB写入 + JSON输出              │
    └─────────────────────────────────────────┘
         │
         ▼
    ┌─────────────┐     ┌──────────────┐
    │ push_alert.py│     │ score_forecast.py│
    │ (条件推送)   │     │ (预测评分)    │
    └─────────────┘     └──────────────┘
```

### 3.2 模块依赖图

```
api_pwv.c  ← 依赖: sqlite3, math
api_nowcast.c ← 依赖: api_pwv.c (PWV数据), sqlite3
api_correlate.c ← 依赖: api_pwv.c, api_nowcast.c, sqlite3
wentian.c ← 依赖: 所有api_*.c
```

**依赖铁律**: 低层模块不依赖高层模块。`api_pwv.c`是最底层，不能被`api_nowcast.c`反向依赖（通过DB解耦）。

### 3.3 自进化机制

**问天自进化三循环**:

1. **数据采集循环** (每60s): 采集→反演→评分→存储→输出
2. **模型优化循环** (每日): 用历史数据回放评分算法，自动调整阈值
3. **代码进化循环** (按需): 发现bug→写修复→编译验证→热替换

**自愈设计**:
- DB写入失败: 重试3次，仍失败则写临时文件，下次启动补录
- API调用失败: 跳过本次，不阻塞整个循环
- 进程崩溃: systemd自动重启 (需配置)
- 数据异常: 值超出物理范围(如PWV>100mm)标记为异常，不参与评分

---

## 四、厄尔尼诺战备模式

### 4.1 WMO通报要点 (2026-09-03)

- 尼诺3.4区SST偏高1.5-2.0°C，周均最高偏离+2.5°C
- 海洋次表层水温偏高>8°C，热量积聚持续增强
- 持续至2027年2月概率近100%
- 预计发展为**超强级别**厄尔尼诺

### 4.2 对长水ZPPP的影响

| 天气型 | 平时频率 | 厄尔尼诺期 | 问天应对 |
|--------|----------|-----------|----------|
| 雷暴 | 夏季常见 | **频率+60%** | PWV阈值降低15% |
| 飑线 | 春秋常见 | **强度+40%** | 气压梯度阈值降低20% |
| 假冷锋 | 冬季常见 | **频次翻倍** | 温度梯度检测更灵敏 |
| 准静止锋 | 梅雨季 | **持续时间延长** | 高湿持续时间阈值放宽 |
| 低空风切变 | 全年 | **风险显著增加** | 风向突变检测窗口缩短至3min |

### 4.3 阈值动态调整

在 `api_nowcast.c` 中已预留 `ENSO_MODE` 宏:
```c
#define ENSO_MODE  1  /* 0=平时, 1=厄尔尼诺战备 */

#if ENSO_MODE
  #define SQUALL_PRESS_RISE_THRESH  1.5   /* 原2.0, 降25% */
  #define PWV_SLOPE_WATCH_THRESH    1.5   /* 原2.0, 降25% */
  #define WIND_SHEAR_WINDOW_MIN     3     /* 原5, 缩短 */
#endif
```

---

## 五、Python→C升级路线图

### 已完成 (C实现)
- ✅ `api_pwv.c` — PWV反演 (原 `gps_uno_fusion.py` 核心)
- ✅ `api_nowcast.c` — 短临Nowcasting (原Python逻辑)
- ✅ `api_correlate.c` — 软件雷达相干 (新增)
- ✅ `api_gnss_ion.c` — GNSS电离层闪烁S4指数 (原 `gnss_ionosphere.py`)
- ✅ `api_predict.c` — 多源融合预测 (原 `multi_source_predict.py`)

### 待升级 (P0-P1)
| 优先级 | 原Python文件 | 目标C模块 | 理由 |
|--------|-------------|-----------|------|
| — | — | — | 全部P0/P1已完成 |

### 保持Python
| 文件 | 原因 |
|------|------|
| `push_alert.py` | HTTP推送是IO密集型 |
| `score_forecast.py` | 离线事后评分，不实时 |
| `uno_bridge.py` | 串口通信Python天然适合 |
| `weather_analyze.py` | 分析性脚本 |

---

## 六、质量门禁

每次代码修改必须通过:

1. **编译**: `gcc -O2 -Wall -Wextra` 零警告(新文件)
2. **运行**: `./wentian once` 无crash
3. **DB验证**: 目标表有数据写入
4. **JSON验证**: 输出JSON格式正确 (`python3 -m json.tool`)
5. **内存**: valgrind无泄漏(定期)

---

## 七、文件索引

| 文件 | 路径 | 说明 |
|------|------|------|
|| 主daemon | `/root/scripts/wentian/wentian` | 编译后二进制 |
|| 主循环 | `/root/scripts/wentian/wentian.c` | 20步数据采集+融合 |
|| 头文件 | `/root/scripts/wentian/wentian.h` | 全局struct+声明 |
|| PWV反演 | `/root/scripts/wentian/api_pwv.c` | C实现, 60s周期 |
|| Nowcast | `/root/scripts/wentian/api_nowcast.c` | 全天气型, v1.5 |
|| 雷达相干 | `/root/scripts/wentian/api_correlate.c` | SDR+GNSS+UNO |
|| 电离层 | `/root/scripts/wentian/api_gnss_ion.c` | S4闪烁, C实现 |
|| 预警推送 | `/root/scripts/wentian/push_alert.py` | Mac风格, v1.6 |
|| 预测评分 | `/root/scripts/wentian/score_forecast.py` | 小时级自动评分 |
|| 主DB | `/root/data/wentian.db` | 25维度数据 |
|| 最新JSON | `/root/data/fusion/wentian_latest.json` | 25维度融合输出 |
|| Nowcast JSON | `/root/data/fusion/nowcast.json` | 实时评分(含warning_level/precip_intensity) |
|| 雷达JSON | `/root/data/fusion/radar_correlation.json` | 相干结果 |
|| 国标文档 | `/root/scripts/wentian/GB_STANDARD.md` | GB/T 35663-2017对齐 |
|| 架构文档 | `/root/scripts/wentian/STANDARD.md` | v2.1标准化手册 |

---

> 文档版本: v2.1 | 最后更新: 2026-09-04 | 状态: 全部P0/P1/P2已真实落地，25维度全C零warning