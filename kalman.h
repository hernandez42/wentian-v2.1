/* ============================================================
 * kalman.h - 1D Kalman 滤波器 (内联静态函数) v1.1
 * ============================================================
 * 项目: 问天 v1.1 (WenTian Weather Station)
 * 锁定: 详见 WENTIAN-LOCK.md, 非授权LLM请勿擅改
 *
 * 来源: GitHub awesome-kalman-filter (mintisan)
 *       简化版 1D 标量 Kalman
 *
 * 用法: 全部内联 static inline, 编译时展开
 *   kf1d_t kf;
 *   kf1d_init(&kf, 初始值, 初始不确定性, 过程噪声, 观测噪声);
 *   while (有新观测) {
 *       double 新值 = 传感器读数();
 *       double 估计 = kf1d_update(&kf, 新值);
 *   }
 *
 * 应用:
 *   - 气压Kalman平滑 (机柜+海平面气压校准)
 *   - 多源温度Kalman融合 (Open-Meteo + METAR + wttr)
 *   - GNSS坐标Kalman平滑 (抗多路径)
 *   - S4闪烁指数Kalman预测
 *
 * 优点: 纯C99, 静态内联, 零依赖, 比线性回归更平滑
 *
 * 警告: 这是 1D 版本, 2D/3D需另写矩阵版本
 * ============================================================ */
#ifndef KALMAN_H
#define KALMAN_H

#include <math.h>
#include <stdint.h>

/* 一维标量Kalman滤波器 (用于气压/温度/坐标等) */
typedef struct {
    double  x;       /* 状态估计 */
    double  p;       /* 估计协方差 (不确定性) */
    double  q;       /* 过程噪声协方差 (模型不确定性) */
    double  r;       /* 观测噪声协方差 (传感器噪声) */
    double  k;       /* Kalman增益 */
} kf1d_t;

/* 初始化 */
static inline void kf1d_init(kf1d_t *kf, double x0, double p0, double q, double r) {
    kf->x = x0;
    kf->p = p0;
    kf->q = q;
    kf->r = r;
    kf->k = 0;
}

/* 更新一步: z = 新观测值 */
static inline double kf1d_update(kf1d_t *kf, double z) {
    /* 预测步 */
    /* x_k|k-1 = x_k-1 (假设常速/常值模型) */
    /* p_k|k-1 = p_k-1 + q */
    kf->p = kf->p + kf->q;

    /* 更新步 */
    kf->k = kf->p / (kf->p + kf->r);  /* Kalman增益 */
    kf->x = kf->x + kf->k * (z - kf->x);  /* 状态更新 */
    kf->p = (1.0 - kf->k) * kf->p;  /* 协方差更新 */

    return kf->x;
}

/* 仅预测 (无观测) */
static inline double kf1d_predict(kf1d_t *kf) {
    kf->p = kf->p + kf->q;
    return kf->x;
}

/* 获取当前估计 */
static inline double kf1d_get(const kf1d_t *kf) { return kf->x; }

/* 获取不确定性 (标准差) */
static inline double kf1d_uncertainty(const kf1d_t *kf) { return sqrt(kf->p); }

#endif /* KALMAN_H */