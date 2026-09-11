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


/* ============================================================
 * 2D Kalman 滤波器 (气压+温度联合) + 自适应Q/R v1.2
 * ============================================================
 * 扩展: 2D状态向量 x[2] = [气压(hPa), 温度(°C)]
 *       自适应Q参数 (基于新息滑动窗口动态放大)
 *
 * 物理背景:
 *   气压和温度联合滤波可利用两者变化的相关性
 *   (夏季对流天气时气压骤降+温度骤升, 协方差非零).
 *   自适应Q使滤波器在天气剧烈变化时自动提高响应速度.
 *
 * 所有2x2矩阵运算手工展开, 零矩阵库依赖, 纯C99.
 *
 * 用法:
 *   kf2d_t kf;
 *   kf2d_init(&kf, 1013.0, 25.0, 0.05, 0.05, 0.5, 0.5, 1.0);
 *   kf2d_predict(&kf, 300.0);        // 5分钟预测步
 *   kf2d_update(&kf, 1012.8, 25.3);  // 观测更新
 *   kf2d_adaptive_q(&kf, 20);        // 基于最近20步自适应
 * ============================================================ */

/* 最大滑动窗口大小 (避免动态内存分配, 纯C99) */
#define KF2D_MAX_WINDOW 64

/* 二维Kalman滤波器 (气压+温度联合) */
typedef struct {
    /* --- 状态与协方差 --- */
    double x[2];       /* 状态向量
                        * x[0] = 气压 (hPa)
                        * x[1] = 温度 (\u2103) */
    double P[4];       /* 估计协方差矩阵 P (2x2, 行优先排列)
                        * P[0]=P00(气压方差), P[1]=P01(气压-温度协方差)
                        * P[2]=P10(温度-气压协方差), P[3]=P11(温度方差)
                        * P非对角元体现气压-温度变化的相关性 */
    double Q[4];       /* 过程噪声协方差 Q (2x2, 行优先, 对角占优)
                        * 反映模型不确定性 (kg^2/s^4 量纲对应各状态量)
                        * Q越大, 滤波器越信任观测, 响应越快 */
    double R[4];       /* 观测噪声协方差 R (2x2, 行优先, 通常对角)
                        * 反映传感器噪声方差 (来自手册或经验标定)
                        * R越大, 滤波器越信任预测, 平滑越强 */
    double K[4];       /* Kalman增益矩阵 (2x2, 行优先)
                        * 决定观测修正的权重分配 */

    /* --- 自适应Q参数 --- */
    double  q_scale;           /* 过程噪声缩放因子
                                * 1.0 = 正常天气
                                * >1  = 夏季对流活跃 (气压温度变化剧烈) */
    double  innov_buf[KF2D_MAX_WINDOW][2]; /* 新息(innovation)滑动窗口环形缓冲
                                            * innov_buf[i][0] = 气压新息
                                            * innov_buf[i][1] = 温度新息
                                            * 新息 = 观测值 - 预测值 = z - H*x */
    int     innov_head;        /* 环形缓冲写入位置 (下一个覆盖索引) */
    int     innov_count;       /* 当前积累的新息个数 (0 ~ window_size) */
    int     window_size;       /* 自适应滑动窗口大小 (0=关闭自适应) */
} kf2d_t;

/* 初始化二维Kalman滤波器
 * 参数:
 *   kf        - 滤波器指针
 *   p0        - 气压初始估计 (hPa), 如最后一帧有效值
 *   t0        - 温度初始估计 (\u2103), 如最后一帧有效值
 *   p_pnoise  - 气压过程噪声标准差 (典型0.01~0.1 hPa)
 *               物理含义: 每步气压可能变化的程度
 *   t_pnoise  - 温度过程噪声标准差 (典型0.01~0.1 \u2103)
 *   p_onoise  - 气压观测噪声标准差 (典型0.1~1.0 hPa)
 *               如BMP280手册: 0.12 hPa (高精度模式)
 *   t_onoise  - 温度观测噪声标准差 (典型0.1~1.0 \u2103)
 *   q_scale   - 过程噪声缩放因子
 *               1.0 = 正常天气
 *               1.5~3.0 = 夏季午后强对流/台风逼近
 *               物理含义: 对流活动加强时大气变化速度加快 */
static inline void kf2d_init(kf2d_t *kf,
                              double p0, double t0,
                              double p_pnoise, double t_pnoise,
                              double p_onoise, double t_onoise,
                              double q_scale)
{
    /* ---- 初始状态 ---- */
    kf->x[0] = p0;          /* 气压初始值 */
    kf->x[1] = t0;          /* 温度初始值 */

    /* ---- 初始协方差 P: 取观测噪声平方 (高不确定性起步) ---- */
    /* P00 = sigma_p^2: 气压初始不确定性 */
    kf->P[0] = p_onoise * p_onoise;
    /* P01 = P10 = 0: 初始假设气压-温度不相关 */
    kf->P[1] = 0.0;
    kf->P[2] = 0.0;
    /* P11 = sigma_t^2: 温度初始不确定性 */
    kf->P[3] = t_onoise * t_onoise;

    /* ---- 过程噪声 Q: 对角矩阵, q_scale整体放大 ---- */
    /* 物理: 夏季对流活跃时(q_scale>1), 大气变化更快,
     * 需放大过程噪声使滤波器更快跟踪变化 */
    kf->Q[0] = q_scale * p_pnoise * p_pnoise;  /* Q00: 气压过程噪声 */
    kf->Q[1] = 0.0;                               /* Q01: 假设气压-温度过程不相关 */
    kf->Q[2] = 0.0;                               /* Q10: 对称 */
    kf->Q[3] = q_scale * t_pnoise * t_pnoise;  /* Q11: 温度过程噪声 */

    /* ---- 观测噪声 R: 对角矩阵 ---- */
    /* 来自传感器手册或经验标定, 通常固定 */
    kf->R[0] = p_onoise * p_onoise;  /* R00: 气压传感器噪声方差 */
    kf->R[1] = 0.0;                    /* R01: 观测间不相关 */
    kf->R[2] = 0.0;
    kf->R[3] = t_onoise * t_onoise;  /* R11: 温度传感器噪声方差 */

    /* Kalman增益初始化为零 */
    kf->K[0] = 0.0;
    kf->K[1] = 0.0;
    kf->K[2] = 0.0;
    kf->K[3] = 0.0;

    /* ---- 自适应参数初始化 ---- */
    kf->q_scale     = q_scale;
    kf->innov_head  = 0;
    kf->innov_count = 0;
    kf->window_size = 0;  /* 默认关闭自适应, 由kf2d_adaptive_q()开启 */
}

/* 预测步: 基于时间步长dt的状态预测
 * 参数:
 *   kf - 滤波器指针
 *   dt - 时间步长 (秒), 典型300 (5分钟)
 *
 * 物理模型: 常值模型, F = I (单位矩阵)
 *   短时间(5~30分钟)内气压和温度变化视为零均值随机游走,
 *   即当前最佳估计就是上一时刻的估计值.
 *
 * 协方差传播: P(k|k-1) = F * P(k-1) * F^T + Q(dt)
 *   F=I 简化为: P(k|k-1) = P(k-1) + Q(dt)
 *
 * Q随dt线性缩放: 时间步越长, 模型不确定性越大
 *   Q(dt) = Q * (dt / 300) */
static inline void kf2d_predict(kf2d_t *kf, double dt)
{
    double dt_scale;

    /* dt归一化到基准步长300秒(5分钟) */
    dt_scale = (dt > 0.0) ? (dt / 300.0) : 1.0;

    /* 协方差预测: P = P + Q(dt) (手工展开2x2矩阵加法)
     * 状态x保持不变 (常值模型) */
    /* P00 += Q00 * dt_scale: 气压不确定性随时间增加 */
    kf->P[0] = kf->P[0] + kf->Q[0] * dt_scale;
    /* P01/P10: 互协方差不直接加Q (Q对角), 但通过后续更新传递相关性 */
    /* P11 += Q11 * dt_scale: 温度不确定性随时间增加 */
    kf->P[3] = kf->P[3] + kf->Q[3] * dt_scale;
}

/* 更新步: 融合新的气压和温度观测值 (完整的2D Kalman更新)
 * 参数:
 *   kf    - 滤波器指针
 *   p_obs - 气压观测值 (hPa), 如BMP280读数
 *   t_obs - 温度观测值 (\u2103), 如BMP280读数
 *
 * 算法步骤 (2x2矩阵全手工展开):
 *   1) 新息(innovation): y = z - H*x   (H=I, 直接观测)
 *      y[0] = p_obs - x[0]  (气压偏差)
 *      y[1] = t_obs - x[1]  (温度偏差)
 *   2) 新息协方差: S = H*P*H^T + R = P + R
 *   3) Kalman增益: K = P * S^{-1}
 *   4) 状态更新: x = x + K * y
 *   5) 协方差更新: P = (I - K) * P
 *   6) 记录新息用于自适应Q (若window_size>0) */
static inline void kf2d_update(kf2d_t *kf, double p_obs, double t_obs)
{
    double innov[2];     /* 新息向量 y = z - x */
    double S[4];         /* 新息协方差矩阵 S = P + R */
    double det;          /* det(S) */
    double inv_det;      /* 1 / det(S) */
    double Sinv[4];      /* S^{-1} (逆矩阵) */
    double k00, k01, k10, k11; /* Kalman增益矩阵元素 */
    double p00, p01, p10, p11; /* 当前协方差快照 */

    /* ======== 步骤1: 计算新息 ======== */
    /* y = z - H*x, 其中 H = I_{2x2} (直接观测气压和温度) */
    innov[0] = p_obs - kf->x[0];  /* 气压新息: 正=观测比估计高 */
    innov[1] = t_obs - kf->x[1];  /* 温度新息 */

    /* ======== 步骤2: 新息协方差 S = P + R ======== */
    /* 取出当前协方差 (避免结构体反复访问) */
    p00 = kf->P[0]; p01 = kf->P[1];
    p10 = kf->P[2]; p11 = kf->P[3];

    /* S = P + R (手工展开2x2加法) */
    S[0] = p00 + kf->R[0];  /* S00 = P00 + R00: 气压总不确定性 */
    S[1] = p01 + kf->R[1];  /* S01 = P01 + R01: 气压-温度互协方差 */
    S[2] = p10 + kf->R[2];  /* S10 = P10 + R10: 对称 */
    S[3] = p11 + kf->R[3];  /* S11 = P11 + R11: 温度总不确定性 */

    /* ======== 步骤3: Kalman增益 K = P * S^{-1} ======== */
    /* 首先计算S的逆矩阵 (S是对称的: S[1]==S[2]在数值精度内) */
    /* det(S) = S00*S11 - S01*S10 */
    det = S[0] * S[3] - S[1] * S[2];

    /* 防除零: 行列式极小说明观测极可靠, 限幅 */
    if (det < 1e-12) {
        det = 1e-12;
    }
    inv_det = 1.0 / det;

    /* S^{-1} = 1/det * [ S11, -S01; -S10, S00 ] */
    Sinv[0] =  S[3] * inv_det;  /* S^{-1}[0][0] = S11/det */
    Sinv[1] = -S[1] * inv_det;  /* S^{-1}[0][1] = -S01/det */
    Sinv[2] = -S[2] * inv_det;  /* S^{-1}[1][0] = -S10/det */
    Sinv[3] =  S[0] * inv_det;  /* S^{-1}[1][1] = S00/det */

    /* K = P * S^{-1} (2x2 x 2x2, 手工展开)
     * K[i][j] = sum_{k=0}^{1} P[i][k] * S^{-1}[k][j] */
    k00 = p00 * Sinv[0] + p01 * Sinv[2];  /* K00 = P00*Sinv00 + P01*Sinv10 */
    k01 = p00 * Sinv[1] + p01 * Sinv[3];  /* K01 = P00*Sinv01 + P01*Sinv11 */
    k10 = p10 * Sinv[0] + p11 * Sinv[2];  /* K10 = P10*Sinv00 + P11*Sinv10 */
    k11 = p10 * Sinv[1] + p11 * Sinv[3];  /* K11 = P10*Sinv01 + P11*Sinv11 */

    /* 保存Kalman增益 (用于调试/分析) */
    kf->K[0] = k00;
    kf->K[1] = k01;
    kf->K[2] = k10;
    kf->K[3] = k11;

    /* ======== 步骤4: 状态更新 x = x + K * y ======== */
    /* K*y: 2x2矩阵乘2x1向量, 手工展开 */
    /* x[0] += K[0][0]*innov[0] + K[0][1]*innov[1] */
    kf->x[0] = kf->x[0] + k00 * innov[0] + k01 * innov[1];
    /* x[1] += K[1][0]*innov[0] + K[1][1]*innov[1] */
    kf->x[1] = kf->x[1] + k10 * innov[0] + k11 * innov[1];

    /* ======== 步骤5: 协方差更新 P = (I - K) * P ======== */
    /* 手工展开2x2矩阵乘法:
     * P_new[i][j] = sum_{k=0}^{1} (delta_ik - K[i][k]) * P[k][j]
     * 其中 delta_ik = (i==k) ? 1.0 : 0.0 */
    kf->P[0] = (1.0 - k00) * p00 + (-k01)    * p10;  /* P00_new */
    kf->P[1] = (1.0 - k00) * p01 + (-k01)    * p11;  /* P01_new */
    kf->P[2] = (-k10)    * p00 + (1.0 - k11) * p10;  /* P10_new */
    kf->P[3] = (-k10)    * p01 + (1.0 - k11) * p11;  /* P11_new */

    /* ======== 步骤6: 记录新息 (用于自适应Q) ======== */
    if (kf->window_size > 0) {
        /* 环形缓冲: 写入当前位置后递增 */
        kf->innov_buf[kf->innov_head][0] = innov[0];
        kf->innov_buf[kf->innov_head][1] = innov[1];
        kf->innov_head = (kf->innov_head + 1) % kf->window_size;

        /* 积累计数不超过窗口大小 */
        if (kf->innov_count < kf->window_size) {
            kf->innov_count++;
        }
    }
}

/* 自适应过程噪声: 基于新息滑动窗口动态调整Q
 * 参数:
 *   kf          - 滤波器指针
 *   window_size - 滑动窗口大小 (典型10~30步)
 *                 设为0关闭自适应, 回到固定Q模式
 *
 * 原理 (AKF, Adaptive Kalman Filter):
 *   新息的理论协方差: S = P + R
 *   若实际新息协方差(窗口样本估计)显著大于理论值,
 *   说明模型跟不上实际物理变化, 应放大过程噪声Q
 *   使滤波器更快跟踪.
 *
 * 物理背景:
 *   夏季午后强对流: 气压10分钟内可降2~3hPa, 温度升5~8\u2103
 *   固定Q的滤波器会滞后, 自适应Q能自动提高增益,
 *   使估计紧跟实测值.
 *
 * 算法:
 *   1) 计算窗口内新息的样本均值 E[innov]
 *   2) 计算无偏样本方差 Var[innov] = sum((innov-E[innov])^2)/(N-1)
 *   3) 比较样本方差 vs 理论方差 S_ii
 *   4) 若样本 > 1.5*理论, 按比例放大Q
 *   5) 指数平滑(0.5因子)防止Q突变导致滤波器震荡 */
static inline void kf2d_adaptive_q(kf2d_t *kf, int window_size)
{
    int i;
    int n;                    /* 实际使用的样本数 */
    double sum_inno[2];       /* 新息累加和 */
    double mean_inno[2];      /* 新息样本均值 */
    double var_inno[2];       /* 新息样本方差 (无偏估计) */
    double S_ii[2];           /* 理论新息协方差对角元 */
    double ratio[2];          /* 实际/理论比值 */
    const double q_min[2] = {  /* Q最小值防收敛到零 */
        1e-8,                  /* 气压最小Q */
        1e-8                   /* 温度最小Q */
    };
    const double ratio_thresh = 1.5;  /* 放大触发阈值 */
    const double smooth = 0.5;        /* 平滑因子防突变 */

    /* ---- 窗口大小处理 ---- */
    if (window_size <= 0) {
        kf->window_size = 0;  /* 关闭自适应 */
        return;
    }

    /* 限制窗口不超最大容量 */
    if (window_size > KF2D_MAX_WINDOW) {
        window_size = KF2D_MAX_WINDOW;
    }

    /* 窗口大小变化时重置缓冲 (新息序列不连续) */
    if (window_size != kf->window_size) {
        kf->window_size  = window_size;
        kf->innov_head   = 0;
        kf->innov_count  = 0;
        /* 缓冲为空, 无法估计, 等待积累 */
        return;
    }

    /* 需要至少2个样本才能估计方差 */
    if (kf->innov_count < 2) {
        return;
    }

    n = kf->innov_count;

    /* ======== 步骤1: 计算新息均值 ======== */
    sum_inno[0] = 0.0;
    sum_inno[1] = 0.0;
    for (i = 0; i < n; i++) {
        /* 从环形缓冲中按存储顺序读取 */
        int idx = (kf->innov_head - n + i);
        /* 处理C语言负号取模: 加KF2D_MAX_WINDOW保证正数 */
        while (idx < 0) idx += KF2D_MAX_WINDOW;
        idx %= KF2D_MAX_WINDOW;

        sum_inno[0] += kf->innov_buf[idx][0];
        sum_inno[1] += kf->innov_buf[idx][1];
    }
    mean_inno[0] = sum_inno[0] / (double)n;
    mean_inno[1] = sum_inno[1] / (double)n;

    /* ======== 步骤2: 计算无偏样本方差 ======== */
    /* Var = sum((x_i - mean)^2) / (N-1) */
    sum_inno[0] = 0.0;
    sum_inno[1] = 0.0;
    for (i = 0; i < n; i++) {
        int idx = (kf->innov_head - n + i);
        while (idx < 0) idx += KF2D_MAX_WINDOW;
        idx %= KF2D_MAX_WINDOW;

        double d0 = kf->innov_buf[idx][0] - mean_inno[0];
        double d1 = kf->innov_buf[idx][1] - mean_inno[1];
        sum_inno[0] += d0 * d0;
        sum_inno[1] += d1 * d1;
    }
    var_inno[0] = sum_inno[0] / (double)(n - 1);
    var_inno[1] = sum_inno[1] / (double)(n - 1);

    /* ======== 步骤3: 理论新息协方差 S_ii = P_ii + R_ii ======== */
    S_ii[0] = kf->P[0] + kf->R[0];  /* 气压维理论新息方差 */
    S_ii[1] = kf->P[3] + kf->R[3];  /* 温度维理论新息方差 */

    /* ======== 步骤4~5: 自适应调整Q ======== */
    /* 分别处理气压和温度两维 */

    /* --- 气压维自适应 --- */
    if (S_ii[0] > 1e-12) {
        ratio[0] = var_inno[0] / S_ii[0];
    } else {
        ratio[0] = 1.0;
    }
    /* 仅当实际新息方差显著大于理论值时放大Q */
    /* ratio>1.5说明模型滞后, 需要增大Q加快跟踪 */
    if (ratio[0] > ratio_thresh) {
        /* 指数平滑: Q_new = smooth*Q_old + (1-smooth)*Q_old*ratio
         * = Q_old * (smooth + (1-smooth)*ratio) */
        kf->Q[0] = kf->Q[0] * (smooth + (1.0 - smooth) * ratio[0]);
        if (kf->Q[0] < q_min[0]) kf->Q[0] = q_min[0];
    }

    /* --- 温度维自适应 --- */
    if (S_ii[1] > 1e-12) {
        ratio[1] = var_inno[1] / S_ii[1];
    } else {
        ratio[1] = 1.0;
    }
    if (ratio[1] > ratio_thresh) {
        kf->Q[3] = kf->Q[3] * (smooth + (1.0 - smooth) * ratio[1]);
        if (kf->Q[3] < q_min[1]) kf->Q[3] = q_min[1];
    }
}

/* 获取当前状态估计 (气压+温度)
 * 参数:
 *   kf - 滤波器指针
 *   p  - [输出] 气压估计 (hPa), 传NULL跳过
 *   t  - [输出] 温度估计 (\u2103), 传NULL跳过 */
static inline void kf2d_get(const kf2d_t *kf, double *p, double *t)
{
    if (p) *p = kf->x[0];
    if (t) *t = kf->x[1];
}

/* 获取不确定性 (标准差)
 * 参数:
 *   kf   - 滤波器指针
 *   p_sd - [输出] 气压标准差 (hPa), 传NULL跳过
 *   t_sd - [输出] 温度标准差 (\u2103), 传NULL跳过 */
static inline void kf2d_uncertainty(const kf2d_t *kf, double *p_sd, double *t_sd)
{
    if (p_sd) *p_sd = sqrt(kf->P[0]);
    if (t_sd) *t_sd = sqrt(kf->P[3]);
}


#endif /* KALMAN_H */