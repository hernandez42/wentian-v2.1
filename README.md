# 🌤️ 问天 (WenTian) — 气象站 & 电离层监测系统

> **主人: 朱涛 BG8SBA | 位置: 昆明长水机场楼顶(25.08°N, 102.91°E, 2103m)**
> **硬件: RK3588 EVB4 | Armbian 24.04 | 34 维度 | 全链路自愈**

---

## 🏆 核心指标

| 指标 | 值 |
|------|-----|
| 数据维度 | 34 |
| 源码行数 | 8649 行 C |
| 编译 | gcc -O2 -Wall -Wextra 零 error |
| Daemon | 300s 周期, 全链路自愈 |
| GitHub | 每小时自动推送 |
| 数据库 | 26 张表, 持续写入 |
| 自愈巡检 | 17 源自动检测 + systemd 修复 |

---

## 🛰️ 34 数据维度总览

### 🌐 开放 API（13 源）
| 源 | 数据 | 更新频率 |
|----|------|----------|
| Open-Meteo 户外 | T/H/P/风/云/降水 | 5 分钟 |
| Open-Meteo 空气 | PM2.5/PM10/AQI | 5 分钟 |
| Open-Meteo 海洋 | 浪高/周期 | 5 分钟 |
| Open-Meteo 洪水 | 河流流量 | 5 分钟 |
| AviationWeather METAR | ZPPP 机场实况 (自动降级 ECMWF) | 自动 |
| NASA APOD | 每日天文图 | 每天 |
| NASA DONKI | CME/FLR/GST | 6 小时 |
| ISS | 空间站位置 | 实时 |
| USGS | 全球 2.5+ 级地震 | 实时 |
| wttr.in | 昆明天气 | 实时 |
| NOAA SWPC Kp | 地磁 Kp (1 分钟级) | 1 分钟 |
| NOAA SWPC F10.7 | 太阳射电通量 | 每天 |
| met.no (挪威) | 欧洲气象预报 | 每小时 |

### 🏠 本地硬件（3 源）
| 设备 | 数据 | 接口 |
|------|------|------|
| UNO (机柜) | 温/湿/气压 (校准至海平面) | FT232 UART |
| ATGM336H GPS+北斗 | 12.7 GPS + 8 BDS 星, SNR | CH340 UART |
| RTL-SDR V4 | GPS L1 / BDS B1I 频谱扫频 | USB |

### 🧠 融合引擎（18 维）
| 模块 | 功能 | 文件 |
|------|------|------|
| Kalman 气压融合 | 多传感器融合 | `kalman.h` |
| GNSS PWV 反演 | Saastamoinen + Bevis 模型 | `api_pwv.c` |
| S4 电离层 v2.1 | 低 SNR 鲁棒算法 (Van Dierendonck) | `api_gnss_ion.c` |
| 多源融合 S4 | SDR + 串口 + OpenMeteo + UNO + ScintPi | `api_multisrc.c` |
| 等效 TEC | Kp + F10.7 + S4 → TECU | `api_tec.c` |
| 短临 Nowcast | 雷暴/飑线/冷锋/静止锋/风切变 | `api_nowcast.c` |
| 软件雷达相干 | SDR + GNSS + UNO 三路相干 | `api_correlate.c` |
| 多源预测 | 1h/3h/6h 气压/温度/天气/风暴 | `api_predict.c` |
| 自进化评分 | 预报 vs 实测 MAE 闭环 | `api_evolve.c` |
| 阈值自完善 | 基于评分趋势自动微调 | `api_evolve.c` |
| 全链路自愈 | 17 源巡检 + systemd 修复 | `wt_self_repair.c` |
| SDR 自动扫频 | 触发 rtl_power GPS L1 扫频 | `wt_self_repair.c` |
| METAR 多源降级 | ECMWF/Open-Meteo/NWS 备选 | `wt_metar_fallback.c` |
| Mac 飞书推送 | 预警推送 | `push_alert.py` |
| 预测评分系统 | 事后精度评估 | `score_forecast.py` |
| MySQL 学习预测 | CatBoost/N-Beats 离线训练 | `ml/sintill-ai/` |

---

## 🚀 快速开始

```bash
# 1. 编辑配置
vim wentian.h  # 修改 WENTIAN_LAT/LON/ALT

# 2. 编译
gcc -O2 -Wall -Wextra -o wentian \
    wentian.c wentian_api.c wentian_db.c \
    wt_self_repair.c wt_metar_fallback.c \
    api_*.c -I. -lm -lsqlite3 -lcurl

# 3. 单次运行测试
./wentian once

# 4. 安装 systemd 服务
cp wentian /usr/local/bin/
systemctl enable --now wentian

# 5. 查看运行状态
journalctl -u wentian -f
```

---

## 📡 系统架构

```
┌─────────────────────────────────────────────────┐
│                    wentian daemon                │
│  (C 语言, 300 秒周期, 26 步, 全链路自愈)      │
├─────────────────────────────────────────────────┤
│  Step  1-16: 外部 API + 本地硬件采集           │
│  Step 17: 短临 Nowcast (全天气型)              │
│  Step 18: 软件雷达三路相干                     │
│  Step 19: GNSS 电离层 S4 v2.1                 │
│  Step 20: Kalman 气压融合                     │
│  Step 21: 多源融合预测 (1h/3h/6h)             │
│  Step 22: 自进化评分闭环                       │
│  Step 23: 多源融合 S4 (5 源加权)              │
│  Step 24: 开源数据集成 (NOAA/met.no/wttr)      │
│  Step 25: 全系统自愈修复                       │
│  Step 26: 等效 TEC 多源融合                    │
├─────────────────────────────────────────────────┤
│                    输出                         │
├─────────────────────────────────────────────────┤
│  SQLite DB: 26 张表 (wentian.db)               │
│  JSON: 实时融合输出 (/root/data/fusion/)        │
│  飞书推送: Mac 风格预警                         │
│  GitHub: 每小时自动同步                         │
└─────────────────────────────────────────────────┘
```

---

## 🔧 文件结构

```
├── wentian.c              # 主 daemon 循环 (26 步)
├── wentian.h              # 公共头文件
├── wentian_api.c          # HTTP / JSON 工具
├── wentian_db.c           # SQLite 持久化
├── api_openmeteo.c        # Open-Meteo 8 子 API
├── api_other.c            # Aviation/METAR/NASA/USGS/wttr
├── api_local.c            # UNO/GNSS/SDR 本地硬件 (含气压校准)
├── api_swpc.c             # NOAA 太空天气 (Kp + F10.7)
├── api_pwv.c              # GNSS PWV 反演 (C 实现)
├── api_nowcast.c          # 短临 Nowcast (全天气型 5 种)
├── api_correlate.c        # 软件雷达三路相干
├── api_gnss_ion.c         # S4 电离层 v2.1 (低 SNR 鲁棒)
├── api_multisrc.c         # 多源融合 S4 (5 源加权)
├── api_predict.c          # 多源融合预测 (1h/3h/6h)
├── api_tec.c              # 等效 TEC (Kp + F10.7 + S4)
├── api_evolve.c           # 自进化评分闭环 + 自完善
├── api_rtk.c              # GNSS-RTK 预留
├── kalman.h               # 1D Kalman 内联
├── wt_self_repair.c       # 全链路自愈 (17 源巡检)
├── wt_metar_fallback.c    # METAR 多源降级 (ECMWF/NWS)
├── push_alert.py          # Mac 风格飞书推送
├── score_forecast.py      # 预测精度评分系统
├── ml_scintill_ai/        # 电离层 ML 预测 (CatBoost)
├── wentian_git_push.sh    # GitHub 自动推送脚本
├── README.md              # 本文件
├── GB_STANDARD.md         # GB/T 35663-2017 对齐规范
├── STANDARD.md            # 标准化架构手册
├── WENTIAN-LOCK.md        # 跨 LLM 锁定声明
└── WENTIAN-NOTICE.md      # 系统约束与通知
```

---

## 📋 自愈能力

| 组件 | 检测 | 修复动作 | 恢复时间 |
|------|------|----------|----------|
| GPS/北斗串口 | `gps_log` 数据 >300s 旧 | `restart gps-full-collect` | <10s |
| METAR | 数据 >2h 旧 | ECMWF 自动降级 | <5s |
| UNO 传感器 | `ano_weather` 行数不足 | `restart uno-bridge` | <10s |
| 问天 daemon | `nowcast`/`PWV`/`S4` 异常 | 自我重启 | <30s |
| NOAA API | `external_data` 陈旧 | 自动重试 | <15s |
| SDR 硬件 | 数据旧+硬件在线 | 触发 rtl_power 扫频 | <30s |
| 全系统 | 每天 24h 不间断巡检 | systemd + 脚本备选 | 自动 |

---

## 📊 电离层监测 — 科学产出

问天独有的科研级电离层数据：

| 指标 | 方法 | 范围 | 当前值 |
|------|------|------|--------|
| S4 闪烁 | Van Dierendonck v2.1（低 SNR 鲁棒） | 0.0–1.0 | 0.082 NONE |
| 等效 TEC | Kp + F10.7 + S4 融合 | 5–30 TECU | 8.1 TECU |
| Klobuchar 延迟 | 广播星历模型 | 5–30 ns | 18.8 ns |
| PWV 反演 | Saastamoinen + Bevis | 10–50 mm | 42.8 mm |

---

## 📜 标准对齐

- GB/T 35663-2017《天气预报基本术语》
- Van Dierendonck et al. 1993 S4_t_total 标准
- IGS IONEX 规范
- NOAA SWPC API 标准

---

## 🔗 相关项目

- [ScintPi](https://scintpi.utdallas.edu/) — 低成本电离层闪烁监测仪
- [ISMR](https://www.itu.int/rec/R-REC-P.531) — 国际电离层标准
- [Open-Meteo](https://open-meteo.com/) — 免费开源气象 API
- [NOAA SWPC](https://services.swpc.noaa.gov/) — 太空天气预报中心
- [scintill-ai](https://github.com/viventriglia/scintill-ai) — ML 电离层闪烁预测

---

## ⚖️ 许可证

本系统为个人科研项目, 遵循 GB/T 35663-2017 国家标准对齐。
API 数据来源均遵循其各自许可 (CC BY 4.0 / 美国政府公共数据)。

---

*问天 v2.3 | 最后更新: 2026-09-05 | 34 维度 | 全链路自愈 | RK3588 EVB4*