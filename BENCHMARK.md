# 📊 问天 (WenTian) v2.3 — 行业 Benchmark 与升级路线

> 对标 GitHub 顶级项目 + X/Twitter 最新论文
> 日期: 2026-09-05

---

## 一、GitHub 顶级项目对标

### 1. RTKLIB (tomojitakasu, 6.2k+ ⭐)
- **能力**: ANSI C GNSS 精密定位, 支持 GPS/GLONASS/Galileo/BDS/QZSS
- **电离层**: Klobuchar / NeQuick-G / 估计TEC模式 / IONEX文件
- **问天差距**: 
  - ✅ S4已有 → 需补 ROTI (已完成)
  - ❌ 双频TEC反演 → 无硬件
  - ❌ DCB校准 → 可软件实现
  - ❌ SP3精密星历 → 无需求
- **学习点**: `sbas.c` 电离层修正, `iono.c` 多模型融合

### 2. OASIS (giorgiopicanco/OASIS, ⭐新)
- **能力**: Python 电离层TEC工具箱
- **指数**: ROTI / ΔTEC / SIDX
- **问天差距**:
  - ✅ ROTI 已参考实现
  - ❌ ΔTEC/SIDX → 等数据积累

### 3. scintill-ai (viventriglia, 8⭐)
- **能力**: CatBoost ML 电离层闪烁预测
- **状态**: ✅ 已集成到 ml/ 目录
- **条件**: 数据积累 >7 天自动训练

### 4. ITA GNSS Lab 闪烁模拟器
- **能力**: 相屏模型、Kalman信号追迹、多模型闪烁模拟
- **学习点**: 验证问天 S4 v2.1 算法正确性

### 5. GNSS-SDR (gnss-sdr/gnss-sdr, 1.8k+ ⭐)
- **能力**: C++ 全软件 GNSS 接收机
- **太重**: 不适合 RK3588 端侧

### 6. PPPx (pppx-dev/pppx)
- **能力**: SPP/PPP/RTK, 多系统, 2880 epoch/秒
- **学习点**: Kalman 滤波、整周模糊度解析

---

## 二、X/Twitter 最新论文对标 (2024-2025)

| 论文 | 方法 | 能力 | 问天状态 |
|------|------|------|----------|
| **ISNet** (Space Weather 2025) | 深度分解+动态图神经网络 | 1h前S4预测, RMSE=0.183(强) | ⏳ 可用作 ml/ 训练目标 |
| **LIFT** (arXiv 2025) | 混合线性+Transformer, 370K参数 | 24h foF2/hmF2/TEC预测 | ⏳ 适合RK3588本地推理 |
| **TEC-LLM** | LLM × TEC时空预测 | 全球TEC预测 | ⏳ 可集成 |
| **ionopy** (arXiv 2025) | Temporal Fusion Transformer | 24h vTEC, RMSE=3.33 TECU | ⏳ 多源输入参考 |
| **PCA融合单频TEC** (2025) | Klobuchar+NeQuick-G融合 | 优于单独模型 | ⏳ 问天可做类似 |
| **ConvGRU** (Remote Sens 2024) | 卷积GRU | S4时空预测, CC=0.94 | ⏳ 学习点 |

---

## 三、问天短板分析

| 能力 | 问天 v2.3 | 行业顶级 | 差距 | 优先级 |
|------|-----------|----------|------|--------|
| **S4算法** | ✅ v2.1 低SNR鲁棒 | Van Dierendonck标准 | 已对齐 | P0 ✅ |
| **ROTI** | ✅ v1.0 (等数据积累) | 行业标准 | 需>6条数据 | P0 ✅ |
| **TEC双频** | ~等效TEC | 双频STEC反演 | 需新硬件(双频GPS) | P2 |
| **DCB校准** | ❌ | CODE DCB产品 | 可软件实现(无硬件) | P3 |
| **XGBoost预测** | ⏳ ml/ 待训练 | ISNet/XGBoost | 数据>7天自动训练 | P1 |
| **Transformer** | ❌ | LIFT/ISNet | 370K参数, 适合RK3588 | P2 |
| **GPS信号捕获** | ❌ | gnss-sdr | 太重, 无需求 | P3 |

---

## 四、P0 立即补 — 已完成

### ROTI (Rate of TEC Index)
- **标准**: Pi et al. (1997)
- **实现**: `roti_calc.py` v1.0
- **公式**: ROTI = stddev(ROT, 5分钟窗口), ROT = Δ(TEC)/Δt
- **输出**: /root/data/fusion/roti.json
- **条件**: 等待>6条等效TEC数据

---

## 五、P1 下一步 — 自动训练策略

```
数据积累 >7天
  → ml/scintill-ai/ 自动触发 CatBoost 训练
  → 输入特征: 时间特征 + S4 + TEC + Kp + F10.7
  → 输出: S4严重度分类 (Low/Medium/High)
  → 精度目标: >= 76% (对标 XGBoost 论文)
```

---

## 六、数据积累目标

| 指标 | 当前 | 7天阈值 | 30天目标 |
|------|------|---------|----------|
| 等效TEC | 5条 | >1000 | >10000 |
| S4实测 | 持续 | >5000 | >50000 |
| GNSS SNR | 26478条/天 | 够了 | 够了 |
| 历法日志 | 5条 | >7 | >30 |

---

*问天 v2.3 — 行业 Benchmark v1.0*
*2026-09-05*