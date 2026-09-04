# 问天系统通知 (WenTian System Notice) v1.1

> 🎯 本文档固化问天的运行环境、依赖、权限、警告,任何会话的LLM读取此文件
> 即可获得完整上下文,避免误操作和幻觉。

---

## 1. 项目身份

| 项 | 值 |
|----|----|
| 项目名 | **问天 (WenTian Weather Station)** |
| 版本 | **v1.1.0** (2026-09-03 锁定) |
| 所有者 | 主人朱涛 (呼号 **BG8SBA**) |
| 主机 | 瑞芯微 Rockchip RK3588 EVB4 |
| 系统 | Armbian 24.04 LTS (aarch64) |
| IP | 10.116.37.40 |
| 主语言 | **C99/C11** |
| 锁定 | 见 [`WENTIAN-LOCK.md`](./WENTIAN-LOCK.md) |

---

## 2. 路径常量 (主人硬性指定, 永不变)

### 项目目录
```
/root/scripts/wentian/         ← 项目根
/root/scripts/wentian/wentian  ← 可执行文件 (67KB stripped)
```

### 数据路径
```
/root/data/wentian.db          ← 主数据库 (SQLite, 14张表)
/root/data/ano_weather.db      ← 主人硬件DB (UNO/GNSS/电离层)
/root/data/uno_weather.csv     ← UNO CSV回退源
/root/data/sdr/v4_sweep_*/     ← V4 SDR扫频CSV
/root/data/cache/              ← APOD缓存 + NASA key配置
/root/data/cache/nasa_key.txt  ← NASA Key (可选, 突破限流)
/root/data/cache/apod_last.json ← APOD缓存
```

### ⚠️ 绝对禁止修改的路径
- `/root/scripts/wentian/wentian.h` 中 `WENTIAN_LAT`/`LON`/`ALT` (主人坐标)
- `/root/data/wentian.db` schema (主人14表结构)
- `/root/scripts/wentian/wentian.c` 中 `main()` 入口逻辑

---

## 3. 系统依赖

### 运行时库
| 库 | 最低版本 | 用途 | 安装 |
|----|---------|------|------|
| libcurl | 7.x | HTTP/HTTPS | `apt install libcurl4-openssl-dev` |
| libsqlite3 | 3.x | SQLite持久化 | `apt install libsqlite3-dev` |
| libc | 2.31+ | strptime/timegm | 系统自带 |

### 工具依赖
- `gcc` ARM交叉编译 (主人已用 aarch64 native gcc)
- `strip` 二进制缩减
- `sqlite3` 命令行调试 (可选)
- `codebase-memory-mcp` (DeusData) 代码审计 (主人已装)

### 网络依赖
| 服务 | 用途 | 是否免key |
|------|------|----------|
| `api.open-meteo.com` | 室外气象 | ✅ |
| `air-quality-api.open-meteo.com` | 空气质量 | ✅ |
| `marine-api.open-meteo.com` | 海洋 | ✅ |
| `flood-api.open-meteo.com` | 洪水 | ✅ |
| `aviationweather.gov` | METAR/TAF | ✅ |
| `api.nasa.gov` | APOD+DONKI | ❌ DEMO_KEY 限流 |
| `api.sunrise-sunset.org` | 日出日落 | ✅ (有SPA本地回退) |
| `api.open-notify.org` | ISS | ✅ |
| `earthquake.usgs.gov` | 地震 | ✅ |
| `wttr.in` | 冗余气象 | ✅ |
| `services.swpc.noaa.gov` | 太空天气 | ✅ |

---

## 4. 调用规范

### 入口点
```c
#include "wentian.h"

int main(int argc, char **argv) {
    if (argc < 2) return 1;

    /* 必须先初始化DB */
    wt_db_init("/root/data/wentian.db");
    wt_local_db_init("/root/data/wentian.db");

    if (strcmp(argv[1], "once") == 0)   return wentian_collect_all();
    if (strcmp(argv[1], "report") == 0) return wentian_print_report();
    if (strcmp(argv[1], "daemon") == 0) return wentian_daemon(atoi(argv[2]));
    return 1;
}
```

### 内存规则
- 所有 `wt_json_dup()` 返回的字符串必须 `free()`
- 所有 `wt_http_get()` 返回的字符串必须 `free()`
- `wt_buf_t` 必须 `wt_buf_init()` + 最后 `wt_buf_free()`
- SQLite stmt 必须 `sqlite3_finalize()`

### 错误码
- `0` 成功
- `-1` 失败 (HTTP超时/JSON解析/SQL失败/硬件离线)

---

## 5. ⚠️ 主人硬性规则

### 🚫 禁止
1. **禁止造假** — 所有操作必须真实落地,数据真实入库
2. **禁止创建"兼容版"** — 做错立即认错,不辩解不甩锅
3. **禁止乱改乱操作** — 主人坐标/DB schema/main入口不可改
4. **禁止添加伪依赖** — 仅 libcurl/libsqlite3/libc
5. **禁止占位** — 真实跑通才算完成

### ✅ 必须
1. **每行命令必须真实执行** — 验证退出码,验证落地
2. **C语言为主** — Python 仅做胶水
3. **使用 mcp-code-intelligence** — 找bug/优化
4. **跨日逻辑必须正确** — 昆明日出22:48 UTC属前一天
5. **Kalman 滤波状态必须初始化** — 避免 NaN
6. **HTTP 重试2次** — 防止单次超时挂掉
7. **多级降级** — API → 缓存 → 本地算法 → placeholder

---

## 6. 关键算法说明

### 6.1 SPA 太阳位置算法 (`api_other.c:calc_sun`)

简化版太阳位置算法, 用于 Sunrise/Sunset API 失败时的本地回退:

```
JD_n = JDay(y, m, d) - 2451545.0 + 0.0008
Jstar = JD_n - lon/360
M = (357.5291 + 0.98560028 * Jstar) mod 360  (太阳平近点角)
C = 1.9148·sin(M) + 0.0200·sin(2M) + 0.0003·sin(3M)  (中心差)
λ = (M + C + 180 + 102.9372) mod 360  (黄经)
δ = asin(sin(λ)·sin(23.44°))  (赤纬)
cos(ω) = (sin(-0.83°) - sin(lat)·sin(δ)) / (cos(lat)·cos(δ))
ω = acos(cos(ω)) / 15  (半昼弧小时)
transit = fmod(Jtransit + 0.5, 1.0) * 24  (中天UTC小时)
sunrise = transit - ω
sunset = transit + ω

跨日校正: 若 sunrise 计算值 > sunset, sunrise - 86400 (前一天)
```

**验证**: 昆明 (25.082°N, 102.909°E) 2026-09-03
- sunrise = 22:48 UTC (前一天), sunset = 11:27 UTC (当天)
- 昼长 = 12.6 小时 ✅

### 6.2 Kalman 气压融合 (`wentian.c:wentian_collect_all`)

三源气压融合: UNO机柜 + Open-Meteo + METAR → 平滑值

```
P_fused = KF_update( (P_uno + P_om + P_metar) / 3 )
σ = sqrt(P_covariance)  (不确定性)
```

### 6.3 WMO 代码转换 (`api_openmeteo.c:wmo_text`)

| 代码 | 中文 |
|------|------|
| 0 | 晴 |
| 1-2 | 多云 |
| 3 | 阴 |
| 45-48 | 雾 |
| 51-55 | 毛毛雨 |
| 61-65 | 雨 |
| 71-75 | 雪 |
| 80-82 | 阵雨 |
| 95 | 雷暴 |
| 96-99 | 雷暴冰雹 |

---

## 7. 测试矩阵

| 用例 | 期望 | 实测 |
|------|------|------|
| `wentian once` 完整运行 | 17+/0 失败 | ✅ 17-18/0 |
| 网络断开时运行 | 优雅降级 | ✅ |
| NASA DEMO_KEY 限流 | placeholder | ✅ |
| Sunrise API 超时 | SPA 回退 | ✅ |
| ATGM336H 串口未插 | NMEA 失败不core | ✅ |
| SQLite 被占用 | 写失败不挂 | ✅ |
| `wentian daemon 60` | 循环无core | ✅ |

---

## 8. ⚠️ 其他 LLM 警告

**任何新会话的 LLM 读取本目录前必须**:

1. ✅ 先读 `WENTIAN-LOCK.md` (锁定声明)
2. ✅ 再读 `README.md` (项目架构)
3. ✅ 再读 `WENTIAN-NOTICE.md` (本文件, 系统约束)

**禁止行为**:
- ❌ 不要"重写"整个项目 (主人已实现)
- ❌ 不要替换 C 为 Python (主人硬性)
- ❌ 不要添加新依赖 (主人仅允许 libc/curl/sqlite)
- ❌ 不要擅自修改 `wentian.h` 中的坐标/路径常量
- ❌ 不要删除任何函数 (即使看起来unused, 是预留接口)

**期望行为**:
- ✅ 修复 bug 前先看 `git diff` 历史
- ✅ 添加新功能遵循现有命名 (`wt_xxx_xxx`)
- ✅ 注释块必须包含 版本/项目/锁定 三要素
- ✅ 编译前备份为 `.bak`

---

## 9. 联系 / 反馈

**主人**: 朱涛 (BG8SBA)
**平台**: 飞书 / 昆明长水机场楼顶 / RK3588
**项目沟通**: 通过 Hermes Agent 飞书 DM

---

最后更新: 2026-09-03 v1.1.0 锁定