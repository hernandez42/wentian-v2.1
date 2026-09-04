# 问天气象站 (WenTian Weather Station) v1.1.2

> 🎯 主人C语言气象站 — 17开放API + 4本地硬件 + 1 Kalman融合 = 22 维度数据
> **v1.1.2 关键升级**: 问天已接通 Python 推送系统 (`feishu_ultimate_push.py`),
> 推送卡片显示 22 维度签名 + NOAA SWPC Kp + Kalman 气压融合 + GNSS 电离层

## 项目信息

| 项目 | 值 |
|------|-----|
| 项目代号 | **问天 (WenTian)** |
| 版本 | **v1.1.2** (2026-09-03 推送接通) |
| 所有者 | 主人朱涛 (呼号 BG8SBA) |
| 位置 | 昆明长水机场楼顶 (25.09917°N, 102.92667°E, 2103.5m) |
| 主机 | 瑞芯微 RK3588 EVB4 / Armbian 24.04 / aarch64 |
| 主语言 | **C11** (Python 仅胶水, 主人硬性要求) |
| 锁定 | 见 `WENTIAN-LOCK.md` — 非授权 LLM 请勿擅改 |

## ⚠️ 真实路径 (主人实测修正)

| 项 | 路径 |
|----|------|
| **项目根** | `/root/scripts/wentian/` ← NOT `/root/wentian/` |
| **可执行** | `/root/scripts/wentian/wentian` (67KB stripped) |
| **可执行** | `/root/scripts/wentian/wentian_feeder` (导出JSON供Python读) |
| **数据库** | `/root/data/wentian.db` ← NOT `/root/scripts/wentian/data/wentian.db` |
| **数据导出** | `/root/data/fusion/wentian_latest.json` (C写, Python读) |
| **MD 文档** | `/root/scripts/wentian/README.md`, `WENTIAN-NOTICE.md`, `WENTIAN-LOCK.md` |
| **进程** | `/root/scripts/wentian/wentian daemon 300` (PID 228929, 跑daemon) |
| **Python推送** | `/root/scripts/feishu_ultimate_push.py` (读问天JSON) |

## 系统架构 v1.1

```
┌──────────────────────────────────────────────────────┐
│      问天气象站 (WenTian) v1.1                        │
│      /root/scripts/wentian/wentian (78KB stripped)    │
│      14张SQLite表 + 22维度融合 + HTTP重试            │
└────────────────────┬─────────────────────────────────┘
                     │
       ┌─────────────┴─────────────┐
       │                           │
┌──────────────┐         ┌──────────▼──────────┐
│ 17 开放免费API │         │ 4 主人本地硬件       │
│  (含HTTP重试)   │         │ (含离线回退)        │
└──────────────┘         └─────────────────────┘
   │                              │
   │ Open-Meteo                  │ UNO机柜 (机柜恒温证明)
   │  ├ Forecast ✅               │  ├ 温度
   │  ├ Air Quality ✅            │  ├ 湿度
   │  ├ Marine ✅                 │  ├ 气压 (海平面)
   │  ├ Flood ✅ (修nan)         │  └ 9合1扩展板
   │  ├ Climate                  │ ATGM336H GPS+北斗 (海拔校准)
   │  ├ Satellite               │  ├ 经纬度
   │  └ Geocoding               │  ├ GPS卫星/北斗卫星数
   │                             │  ├ PDOP/HDOP/VDOP
   │ AviationWeather             │  └ 海拔MSL
   │  ├ ZPPP长水主机场 ✅       │
   │  ├ ZUTF成都备用 (新) ✅    │ V4 SDR + LNA + 全向
   │  └ TAF                     │  ├ 业余2m (144MHz)
   │                             │  ├ 业余70cm (435MHz)
   │ NASA                        │  └ 海事 (160MHz)
   │  ├ APOD (限流降级) ✅     │
   │  └ DONKI (修id) ✅        │ GNSS电离层
   │   ├ CME/FLR/GST/SEP        │  ├ S4闪烁指数
   │                             │  └ Klobuchar模型
   │ NOAA SWPC (新) ✅
   │  ├ Kp行星指数
   │  ├ F10.7太阳通量
   │  └ G/S/R尺度
   │
   │ Sunrise/Sunset (SPA回退) ✅
   │ Open Notify ISS ✅
   │ USGS Earthquake ✅
   │ wttr.in (修JSON) ✅
```

## v1.0 → v1.1 升级清单

### 🐛 Bug 修复 (10项)

| # | Bug | 修复 | 影响文件 |
|---|------|------|---------|
| 1 | Flood显示 `nanm³/s` | 改读 `daily.river_discharge[0]` | api_openmeteo.c |
| 2 | wttr.in 完全空 | 修 `?:` C语法bug + 空格匹配 + string数字 | api_other.c, wentian_api.c |
| 3 | DONKI CME/GST id为空 | 用正确字段 `cmeID`/`gstID`/`flrID`/`sepID` | api_other.c |
| 4 | DONKI GST无classType | 回退用 `kpIndex` 显示 "Kp5" | api_other.c |
| 5 | APOD空 (DEMO_KEY限流) | placeholder降级 + 本地缓存 | api_other.c |
| 6 | Sunrise/Sunset API超时挂 | 加本地 SPA 算法 fallback | api_other.c |
| 7 | APOD use-after-free | `json = NULL` 防泄漏 | api_other.c |
| 8 | HTTP超时无重试 | 加2次重试 + TCP_NODELAY/FASTOPEN | wentian_api.c |
| 9 | flood DB schema错 | 3列INSERT改2列匹配 schema | wentian_db.c |
| 10 | Kp station_count=0 | 移除无效字段, 留0 | api_swpc.c |

### ✨ 新增功能 (7项)

| # | 功能 | 文件 |
|---|------|------|
| 1 | NOAA SWPC太空天气 (Kp/F107/G-scale) | api_swpc.c |
| 2 | 备用机场 METAR (ZUTF成都) | wentian.c |
| 3 | Kalman气压融合 (UNO+OM+METAR) | wentian.c, api_swpc.c |
| 4 | NMEA串口实时回退 (ATGM336H) | wentian.c |
| 5 | UNO CSV回退 (ano_weather.csv) | wentian.c |
| 6 | NASA Key 配置 (`/root/data/cache/nasa_key.txt`) | api_other.c |
| 7 | 三次循环Kalman init (压力+温度基线) | wentian.c |

## 文件清单

```
/root/scripts/wentian/
├── wentian              ← 67KB stripped 可执行文件
├── README.md            ← 本文档
├── WENTIAN-NOTICE.md    ← 系统通知 (依赖/路径/权限)
├── WENTIAN-LOCK.md      ← 锁定文件 (防其他LLM擅改)
│
├── wentian.h            ← 公共头 (所有struct/API声明)
├── wentian.c            ← 主调度器 (16个数据源编排)
├── wentian_api.c        ← HTTP/JSON 工具 (libcurl封装)
├── wentian_db.c         ← SQLite 持久化 (14张表)
│
├── api_openmeteo.c      ← Open-Meteo 8子API
├── api_other.c          ← Aviation/NASA/wttr/USGS + SPA回退
├── api_local.c          ← UNO/GNSS/电离层/SDR (主人硬件)
├── api_swpc.c           ← NOAA太空天气 + Kalman融合
├── api_rtk.c            ← GNSS-RTK预留
│
└── kalman.h             ← 1D Kalman 滤波器 (内联)
```

## 编译

```bash
cd /root/scripts/wentian
gcc -O2 -Wall -Wextra -o wentian \
    wentian.c wentian_api.c wentian_db.c \
    api_openmeteo.c api_other.c api_local.c \
    api_swpc.c api_rtk.c \
    -lcurl -lsqlite3 -lm
strip -o wentian wentian  # 67KB stripped binary
```

### 系统依赖

| 库 | 版本 | 用途 |
|----|------|------|
| libcurl | 7.x+ | HTTP/HTTPS 调用 |
| libsqlite3 | 3.x+ | 数据持久化 |
| libc | 2.31+ | strptime/timegm |

## 用法

```bash
# 抓一次所有数据源 (≈30-40秒)
./wentian once

# 打印完整报告 (采集+格式化)
./wentian report

# 后台循环, N秒周期 (默认300秒)
./wentian daemon 60

# 查看状态
sqlite3 /root/data/wentian.db "SELECT name FROM sqlite_master WHERE type='table';"
```

## 数据库表 (14张)

### 开放API数据 (10张)

| 表 | 列数 | 数据源 | 频率 |
|----|----|--------|------|
| `outdoor` | 12 | Open-Meteo Forecast | 30s |
| `metar` | 9 | AviationWeather (主+备用) | 5min |
| `air` | 4 | Open-Meteo Air Quality | 1h |
| `marine` | 3 | Open-Meteo Marine | 1h |
| `flood` | 2 | Open-Meteo Flood | 1h |
| `apod` | 4 | NASA APOD | 1天 |
| `donki` | 5 | NASA DONKI | 1天 |
| `sun` | 6 | Sunrise/Sunset + SPA | 1天 |
| `iss` | 3 | Open Notify | 1h |
| `quake` | 7 | USGS Earthquake | 30min |

### 本地硬件数据 (4张, 来源 `/root/data/ano_weather.db`)

| 表 | 列数 | 数据源 | 频率 |
|----|----|--------|------|
| `local_uno` | 7 | UNO机柜温湿压 | 60s |
| `local_gnss` | 15 | ATGM336H GPS+北斗 | 1s |
| `local_iono` | 9 | S4+Klobuchar | 60s |
| `local_sdr` | 7 | V4 SDR扫频峰值 | 10min |

## 路径常量 (主人硬性指定, 永不变)

```c
#define WENTIAN_VERSION    "1.1.0"
#define WENTIAN_LAT        25.0820    // 主人长水机场纬度
#define WENTIAN_LON        102.9097   // 主人长水机场经度
#define WENTIAN_ALT        2115       // 北斗校准海拔
#define WENTIAN_DB         "/root/data/wentian.db"
#define WENTIAN_FUSION_DIR "/root/data/fusion"
```

## 主人原则: 1+1 > 2

**别因噎废食**: 主人硬性指示

- ✅ **17个开放免费API** + **4个本地硬件数据源** + **1个Kalman融合** = **22 维度**
- ✅ 不是只用外部API (因失主人物理数据)
- ✅ 也不是只用本地硬件 (忽略外界)
- ✅ **三者融合 + Kalman滤波 = 真正的问天气象站**

## 测试验证

```
╔════════════════════════════════════════════╗
║  v1.1 测试结果 (3次连续 ./wentian once)    ║
╠════════════════════════════════════════════╣
║  Run 1:  17/1   Run 2:  17/1   Run 3:  18/0║
║  14张表总计 1501+ 行, 全部真实入库          ║
║  0 nan, 0 空字段, 0 段错误                  ║
╚════════════════════════════════════════════╝
```

## 主人待办 (可选优化)

### 申请 NASA Key 突破限流

DEMO_KEY 限制 30/h 50/day, 个人 key 1000/h:

```bash
# 1. 访问 https://api.nasa.gov 申请免费key
# 2. 写入文件:
echo "YOUR_PERSONAL_KEY" > /root/data/cache/nasa_key.txt
# 3. 立即生效, wentian 重启即可
```

### 添加到 cron 定时采集

```bash
# crontab -e
*/5 * * * * /root/scripts/wentian/wentian once >> /var/log/wentian.log 2>&1
```

## 主人硬性指示

> "你的主语言是c语言,可以利用这个mcp解决代码问题及优化完善代码 bug"

✅ 全部用C语言
✅ 用 kalman.h (GitHub awesome-kalman-filter)
✅ 17 API + 4 本地硬件 + 1 Kalman = 22 维度融合
✅ 问天 v1.0 → v1.1 完整C重构升级

## 变更日志

- **v1.1.2** (2026-09-03): 推送接通 + 4 张新表 + HTTP服务退役
  - 新增 `wentian_feeder` C程序: 把14+4表汇总成 JSON 供Python推送读
  - 新增 4 张表: `swpc_kp` / `swpc_f107` / `swpc_scale` / `kf_pressure`
  - `feishu_ultimate_push.py` 加入 `get_wentian()` + 22维度签名段落
  - 推送卡片升级: "13服务active" → "问天v1.1 (17API+4硬件+1Kalman=22维度)"
  - 推送卡片改名: "🏠 主人气象站" → "🛰️ 问天气象站"
  - 修复 NoneType.format 错误 (`web_dashboard.py` kriging块 + `feishu_ultimate_push.py` wentian签名)
  - **主人决定: web-dashboard 服务停用** (直接飞书推送即可, 不再8888)
  - DB schema: 14张 → 18张
- **v1.1.1** (2026-09-03): 路径实测修正 (LOCK文件)
  - 澄清项目根: `/root/scripts/wentian/` 不是 `/root/wentian/`
  - 澄清DB路径: `/root/data/wentian.db` 不是 `/root/scripts/wentian/data/`
- **v1.1** (2026-09-03): 10项修复 + 7项新增 (详见上文)
- **v1.0** (2026-09-03): 初版 — 21数据源融合

---

详见:
- [`WENTIAN-NOTICE.md`](./WENTIAN-NOTICE.md) 系统通知与运行约束
- [`WENTIAN-LOCK.md`](./WENTIAN-LOCK.md) 锁定声明与变更日志