# 问天项目锁定声明 (WenTian Project Lock) v1.1.2

> 🔒 本文件固化项目锁定状态,任何会话的LLM/AI读取此文件即可获知:
> 1. 项目是否允许修改
> 2. 当前版本及变更日志
> 3. 修改授权流程
> 4. 不可破坏的核心约束

> ⚠️ **主人实测修正 (2026-09-03)**: 项目实际在 `/root/scripts/wentian/`, NOT `/root/wentian/`
> 数据库在 `/root/data/wentian.db`, NOT `/root/scripts/wentian/data/`

> 🛰️ **v1.1.2 重要决定 (主人指令)**: 停用 web-dashboard 8888 HTTP 服务,
> 数据展示由 **飞书推送** (`feishu_ultimate_push.py`) 接管。
> 推送卡片标题已从 "🏠 主人气象站" 改名为 "🛰️ 问天气象站"。

---

## 🔒 锁定状态

| 字段 | 值 |
|------|-----|
| 项目 | **问天 (WenTian Weather Station)** |
| 版本 | **v1.1.2** (2026-09-03 推送接通 + HTTP退役) |
| 锁定级别 | **C2 - 受控修改** |
| 锁定日期 | 2026-09-03 |
| 锁定者 | 主人朱涛 (BG8SBA) |
| 锁定原因 | 主人C语言气象站已达 22 维度融合 + Kalman, 飞书推送稳定 |

### 锁定级别说明

| 级别 | 含义 | 权限 |
|------|------|------|
| L1 - 公开 | 完全开放 | 任何LLM可改 |
| L2 - 注释 | 仅注释/文档 | 需阅读上下文 |
| **C2 - 受控修改** ← 当前位置 | 仅bug修复+文档 | 需主人明确授权 |
| C1 - 严格锁定 | 不可改 | 必须主人物理操作 |
| C0 - 密封 | 不可读 | 需主人口令 |

---

## 📋 当前版本: v1.1.2

### 状态: ✅ 已锁定, 飞书推送接管 (主人实测)

```
╔════════════════════════════════════════════════════════════╗
║  问天 v1.1.2 系统状态 (主人实测 2026-09-03)                  ║
╠════════════════════════════════════════════════════════════╣
║  项目根:  /root/scripts/wentian/  ✅ 真实存在                ║
║  可执行:  wentian (67KB stripped)  ✅ 真实存在               ║
║  可执行:  wentian_feeder (67KB)  ✅ C写JSON供推送读         ║
║  数据库:  /root/data/wentian.db (258KB+)  ✅ 真实存在        ║
║  DB表:    18 张 outdoor/metar/air/marine/flood/apod/        ║
║          donki/sun/iss/quake/local_uno/local_gnss/         ║
║          local_iono/local_sdr + swpc_kp/swpc_f107/         ║
║          swpc_scale + kf_pressure  ✅ 全部存在              ║
║  DB行数:  2200+ 行 真实入库                                  ║
║  进程:    PID 228929, daemon 300  ✅ 问天在跑                ║
║  HTTP:    web-dashboard 已停用 (主人决定)                  ║
║  推送:    feishu_ultimate_push.py v7.2  ✅ 飞书接管         ║
║  推送内容: 22维度数据完整显示 (Kp/F107/Kalman/S4/GNSS)    ║
╚════════════════════════════════════════════════════════════╝
```

---

## 🚫 不可破坏的核心约束 (CRITICAL)

### 文件级锁定

| 文件 | 锁定状态 | 原因 |
|------|---------|------|
| `wentian.h` | **C1 严格** | 含主人坐标/路径常量,改则系统错位 |
| `wentian.c: main()` | **C1 严格** | 主入口, 改则调用栈崩溃 |
| `wentian_db.c: schema` | **C1 严格** | 14张表定义, 改则历史数据失效 |
| `kalman.h` | **C2 受控** | 算法模块, bug可修 |
| `api_*.c` | **C2 受控** | 模块化, 单一职责 |
| `wentian_api.c` | **C2 受控** | 工具层 |
| `wentian` (二进制) | 编译产物 | 自动重建 |

### 路径锁定

```c
/* 以下路径/常量禁止任何修改 */
#define WENTIAN_LAT        25.0820    /* 主人长水机场纬度 */
#define WENTIAN_LON        102.9097   /* 主人长水机场经度 */
#define WENTIAN_ALT        2115       /* 北斗校准海拔 */
#define WENTIAN_DB         "/root/data/wentian.db"
#define OWNER_DB           "/root/data/ano_weather.db"
#define APOD_CACHE         "/root/data/cache/apod_last.json"
#define NASA_KEY_PATH      "/root/data/cache/nasa_key.txt"
```

### 架构锁定

- ✅ **22 维度数据融合** (17 API + 4 硬件 + 1 Kalman) 不可拆分
- ✅ **libcurl + libsqlite3 + libc** 三依赖不可新增
- ✅ **C99/C11** 不可替换为 C++/Rust/Python/Go
- ✅ **单进程同步采集** 不可改为多线程 (避免锁复杂度)
- ✅ **IPv4 only** 不可改为 IPv6 (主人已优化跳过IPv6等待)

---

## ✅ 允许的修改类型

### 受控修改 (C2 - 需谨慎)

1. **Bug 修复**: 段错误/内存泄漏/逻辑错误 → 立即修
2. **文档更新**: 函数注释/README/WENTIAN-*.md → 自由改
3. **新数据源**: 必须遵循 `wt_xxx_yyy()` 命名规范, 添加到 wentian.c
4. **新降级链**: 加深降级深度 (API → 缓存 → 本地 → placeholder)
5. **Kalman 参数**: 可调整 q/r, 但需注明理由

### 禁止的修改

1. ❌ 重命名 `wt_` 前缀为其他前缀
2. ❌ 拆分 `wentian.c` 中的 `wentian_collect_all()` (主流程)
3. ❌ 把 libcurl 换为其他 HTTP 库
4. ❌ 把 SQLite 换为其他数据库
5. ❌ 添加 pthread 多线程
6. ❌ 修改 `wentian.h` 中的 struct 字段顺序 (二进制兼容)
7. ❌ 删除"看起来unused"的函数 (可能是预留接口)
8. ❌ 把 C 替换为 Python/Rust/Go

---

## 📝 修改授权流程

### 步骤 1: 读取上下文

```bash
# 任何修改前必须读这3个文件
cat /root/scripts/wentian/WENTIAN-LOCK.md    # 本文件
cat /root/scripts/wentian/README.md         # 项目架构
cat /root/scripts/wentian/WENTIAN-NOTICE.md # 系统约束
```

### 步骤 2: 备份

```bash
# 任何C源文件修改前必须备份
cd /root/scripts/wentian
cp source.c source.c.bak.YYYYMMDD
```

### 步骤 3: 修改

- 遵循 C2 锁定级别规则
- 注释块必须更新 (版本/项目/锁定)
- 编译通过 + 0 错误

### 步骤 4: 验证

```bash
# 必须验证: 3次连续 ./wentian once 全部成功
for i in 1 2 3; do
    timeout 90 ./wentian once 2>&1 | tail -3
done
```

### 步骤 5: 更新本文件

在 "变更日志" 追加条目:
```markdown
## v1.1.1 (YYYY-MM-DD)
- 修改: <简述>
- 文件: <文件名>
- 验证: <结果>
- 操作者: <LLM名/主人>
```

---

## 📜 变更日志 (CHANGELOG)

### v1.1.0 (2026-09-03) - 锁定当前版本

**操作者**: 主人朱涛 + Hermes Agent (C2 授权)
**类型**: 重大升级 (10 修复 + 7 新增)

#### 🐛 Bug 修复

1. **Flood NaN**: 改读 `daily.river_discharge[0]` 而非顶层
2. **wttr.in C语法**: `wt_json_dup() ?: "0"` 改 NULL 检查
3. **wt_json_num 字符串数字**: 兼容 `"22"` 形式 (wttr.in 数字带引号)
4. **DONKI CME/GST id**: 用 `cmeID`/`gstID` 字段, GST classType 用 KpIndex
5. **APOD use-after-free**: `json = NULL` 防泄漏
6. **Sunrise API 超时挂**: 加本地 SPA 算法回退
7. **HTTP 无重试**: 加 2 次重试 + TCP_NODELAY/FASTOPEN
8. **flood DB schema错**: 3列 INSERT 改 2列匹配
9. **Kp station_count=0**: 移除无效字段
10. **GNSS SQLite 0坐标**: 加 `lat != 0.0` 验证

#### ✨ 新增功能

1. **NOAA SWPC**: Kp/F107/G-scale (新数据源)
2. **备用机场 ZUTF**: 成都机场 METAR
3. **Kalman 气压融合**: UNO+OM+METAR 三源
4. **NMEA 串口回退**: `/dev/ttyUSB*` 实时ATGM336H
5. **UNO CSV 回退**: `/root/data/uno_weather.csv`
6. **NASA Key 配置**: `/root/data/cache/nasa_key.txt`
7. **本地 SPA 太阳算法**: 离线日出日落

#### 📝 文档更新

- README.md → 9.5KB 完整文档
- WENTIAN-NOTICE.md → 7.0KB 系统通知
- WENTIAN-LOCK.md → 锁定声明 (本文件)
- 所有 .c 顶部注释块标准化

#### ✅ 验证结果

- 编译: 0 错误
- 运行: 3 次连续 17-18/0 成功
- DB: 14 表 1501+ 行真实入库
- 系统: 67KB stripped 静态二进制

---

### v1.0.0 (2026-09-03) - 初版

**操作者**: 主人朱涛
**类型**: 初版上线

- 21 数据源融合 (17 API + 4 硬件)
- 14 张表 SQLite 持久化
- 单进程同步采集
- 76KB stripped 静态二进制

---

## 🔐 跨会话一致性保证

### 给未来 LLM 的提醒

> 你不是第一个看到这个项目的LLM, 也不会是最后一个。
>
> 请遵守:
> 1. **先读 3 个 MD 文件** (LOCK + README + NOTICE)
> 2. **修改前先备份** (`.bak.YYYYMMDD` 后缀)
> 3. **编译 + 验证 + 更新日志** 三步走
> 4. **遇到 LOCK 问题, 不要擅自突破**, 报告给主人
>
> 主人原则: **别造假, 别擅自, 别乱改**。
> 任何"Runtime造假"或"看起来完成" = 任务失败。

### 当前会话后状态

```
会话开始时间: 2026-09-03
会话结束时间: 2026-09-03 (v1.1 锁定)
LLM 身份: Hermes Agent (C2 受控修改权限)
最终交付: 67KB stripped 二进制 + 3 个 MD 文档 + 9 个 C 源文件
```

---

## 📞 主人联系方式

- **呼号**: BG8SBA
- **平台**: 飞书 (Feishu / Lark)
- **地点**: 昆明长水机场楼顶 (25.10°N, 102.92°E)
- **硬件**: 瑞芯微 RK3588 EVB4

---

最后更新: **2026-09-03 v1.1.0 🔒 LOCKED**

**注意**: 本文件自动被未来 LLM 读取, 任何修改都会破坏跨会话一致性。
**修改前请先思考**: 这是真的必要吗? 还是可以通过新增/扩展解决?