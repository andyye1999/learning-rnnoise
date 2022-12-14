//
// Created by aone on 2021/5/10.
//

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "kiss_fft.h"
#include "pitch.h"
#include "rnn.h"
#include "rnnoise.h"
#include "rnn_data.h"

#define FRAME_SIZE_SHIFT 2
#define FRAME_SIZE (120<<FRAME_SIZE_SHIFT) /* FRAME_SIZE = 480 */
#define WINDOW_SIZE (2*FRAME_SIZE) // WINDOW_SIZE = 960 正好一帧
#define FREQ_SIZE (FRAME_SIZE + 1) // FREQ_SIZE = 481

#define PITCH_MIN_PERIOD 60
#define PITCH_MAX_PERIOD 768
#define PITCH_FRAME_SIZE 960
#define PITCH_BUF_SIZE (PITCH_MAX_PERIOD + PITCH_FRAME_SIZE) // PITCH_BUF_SIZE = 1728

#define SQUARE(x) ((x)*(x))

#define NB_BANDS 22

#define CEPS_MEM 8
#define NB_DELTA_CEPS 6

/*!
 * 22个Bark尺度的频带gains
 * 跨帧的前6个系数的一阶导数
 * 跨帧的前6个系数的二阶导数
 * 6个频段的pitch增益(发声强度)
 * 1个pitch周期
 * 1个特殊的非平稳值, 用于检测语音
 * 共 22 + 6 + 6 + 6 + 1 + 1 = 42个特征
 */
#define NB_FEATURES (NB_BANDS + 3 * NB_DELTA_CEPS + 2)

#ifndef TRAINING
#define TRAINING 0
#endif

/* 内置模型，在没有文件作为输入时使用 */
extern const struct RNNModel rnnoise_model_orig;

static const opus_int16 eband5ms[] = {
        /*0 200 400 600 800 1k 1.2 1.4 1.6 2k  2.4  2.8  3.2  4k  4.8 5.6  6.8  8k  9.6  12k  15.6  20k*/
          0, 1,  2,  3,  4, 5,  6,  7,  8, 10, 12,  14,  16,  20, 24, 28,  34,  40, 48,  60,  78,   100
};

typedef struct {
    int init;
    kiss_fft_state *kfft;
    float half_window[FRAME_SIZE];
    float dct_table[NB_BANDS * NB_BANDS];
} CommonState;

struct DenoiseState {
    float analysis_mem[FRAME_SIZE]; // frame_analysis的缓存
    float cepstral_mem[CEPS_MEM][NB_BANDS];
    int memid;
    float synthesis_mem[FRAME_SIZE];
    float pitch_buf[PITCH_BUF_SIZE];
    float pitch_enh_buf[PITCH_BUF_SIZE];
    float last_gain;
    int last_period;
    float mem_hp_x[2]; // 计算biquad的中间过程
    float lastg[NB_BANDS];
    RNNState rnn;
};

/*!
 * 计算各频带能量(共22个频带)
 * @param bandE 表示bandEnergy 对应数组长度 NB_BANDS=22
 * @param X 信号x计算得到的FFT复数
 */
void compute_band_energy(float *bandE, const kiss_fft_cpx *X) {
    int i;
    float sum[NB_BANDS] = {0};
    for (i = 0; i < NB_BANDS - 1; i++) {
        int j;
        int band_size;
        band_size = (eband5ms[i + 1] - eband5ms[i]) << FRAME_SIZE_SHIFT; // 带宽size
        for (j = 0; j < band_size; j++) {
            float tmp;
            float frac = (float) j / band_size; // 平滑滤波
            tmp = SQUARE(X[(eband5ms[i] << FRAME_SIZE_SHIFT) + j].r);  // 计算实部平方
            tmp += SQUARE(X[(eband5ms[i] << FRAME_SIZE_SHIFT) + j].i); // 计算虚部平方
            sum[i] += (1 - frac) * tmp; // 实际上这里每个sum[i]都计算了两遍(除了最初的和末尾的) // 每两个三角窗都有一半区域是重叠的，因而对于某一段频点，其既被算入该频带，也被算入下一频带
            sum[i + 1] += frac * tmp;
        }
    }
    sum[0] *= 2; // sum[0]没有计算两遍，这里需要补上
    sum[NB_BANDS - 1] *= 2; // sum[NB_BANDS-1]没有计算两遍，这里需要补上 // 第一个band和最后一个band的窗只有一半因而能量乘以2
    for (i = 0; i < NB_BANDS; i++) {
        bandE[i] = sum[i];
    }
}

/*!
 * 计算相关系数 band之间相关度
 * @param bandE
 * @param X 傅里叶变换系数
 * @param P 基音周期pitch傅里叶变换系数
 */
void compute_band_corr(float *bandE, const kiss_fft_cpx *X, const kiss_fft_cpx *P) {
    int i;
    float sum[NB_BANDS] = {0};
    for (i = 0; i < NB_BANDS - 1; i++) {
        int j;
        int band_size;
        band_size = (eband5ms[i + 1] - eband5ms[i]) << FRAME_SIZE_SHIFT;
        for (j = 0; j < band_size; j++) {
            float tmp;
            float frac = (float) j / band_size;
            tmp = X[(eband5ms[i] << FRAME_SIZE_SHIFT) + j].r * P[(eband5ms[i] << FRAME_SIZE_SHIFT) + j].r;
            tmp += X[(eband5ms[i] << FRAME_SIZE_SHIFT) + j].i * P[(eband5ms[i] << FRAME_SIZE_SHIFT) + j].i;
            sum[i] += (1 - frac) * tmp;
            sum[i + 1] += frac * tmp;
        }
    }
    sum[0] *= 2;
    sum[NB_BANDS - 1] *= 2;
    for (i = 0; i < NB_BANDS; i++) {
        bandE[i] = sum[i];
    }
}

/*!
 * 插值增益
 * @param g 插值后增益
 * @param bandE 原始能量系数
 */
void interp_band_gain(float *g, const float *bandE) {
    int i;
    memset(g, 0, FREQ_SIZE);
    for (i = 0; i < NB_BANDS - 1; i++) {
        int j;
        int band_size;
        band_size = (eband5ms[i + 1] - eband5ms[i]) << FRAME_SIZE_SHIFT;
        for (j = 0; j < band_size; j++) {
            float frac = (float) j / band_size;
            g[(eband5ms[i] << FRAME_SIZE_SHIFT) + j] = (1 - frac) * bandE[i] + frac * bandE[i + 1];
        }
    }
}

CommonState common;

static void check_init() { // 检查初始化状态，即要对fft运算分配内存空间，然后生成需要使用的dct table
    int i;
    if (common.init) return;
    common.kfft = opus_fft_alloc_twiddles(2 * FRAME_SIZE, NULL, NULL, NULL, 0);
    for (i = 0; i < FRAME_SIZE; i++)
        common.half_window[i] = sin(
                .5 * M_PI * sin(.5 * M_PI * (i + .5) / FRAME_SIZE) * sin(.5 * M_PI * (i + .5) / FRAME_SIZE));
    for (i = 0; i < NB_BANDS; i++) {
        int j;
        for (j = 0; j < NB_BANDS; j++) {
            common.dct_table[i * NB_BANDS + j] = cos((i + .5) * j * M_PI / NB_BANDS);
            if (j == 0) common.dct_table[i * NB_BANDS + j] *= sqrt(.5);
        }
    }
    common.init = 1;
}

/*!
 * 离散余弦变换
 * @param out 输出变换后系数
 * @param in 输入信号
 */
static void dct(float *out, const float *in) {
    int i;
    check_init();
    for (i = 0; i < NB_BANDS; i++) {
        int j;
        float sum = 0;
        for (j = 0; j < NB_BANDS; j++) {
            sum += in[j] * common.dct_table[j * NB_BANDS + i];
        }
        out[i] = sum * sqrt(2. / 22);
    }
}

#if 0
static void idct(float *out, const float *in) {
  int i;
  check_init();
  for (i=0;i<NB_BANDS;i++) {
    int j;
    float sum = 0;
    for (j=0;j<NB_BANDS;j++) {
      sum += in[j] * common.dct_table[i*NB_BANDS + j];
    }
    out[i] = sum*sqrt(2./22);
  }
}
#endif

/*!
 * 信号的傅里叶变换计算
 * @param out FFT变换后系数
 * @param in 加窗后的信号 2帧
 */
static void forward_transform(kiss_fft_cpx *out, const float *in) {
    int i;
    kiss_fft_cpx x[WINDOW_SIZE];
    kiss_fft_cpx y[WINDOW_SIZE];
    check_init();
    for (i = 0; i < WINDOW_SIZE; i++) {
        x[i].r = in[i];
        x[i].i = 0;
    }
    opus_fft(common.kfft, x, y, 0);
    for (i = 0; i < FREQ_SIZE; i++) {
        out[i] = y[i];
    }
}

/*!
 * 信号的逆傅里叶变换计算
 * @param out 逆序的输出IFFT结果
 * @param in 傅里叶变换后的系数
 */
static void inverse_transform(float *out, const kiss_fft_cpx *in) {
    int i;
    kiss_fft_cpx x[WINDOW_SIZE];
    kiss_fft_cpx y[WINDOW_SIZE];
    check_init();
    for (i = 0; i < FREQ_SIZE; i++) {
        x[i] = in[i];
    }
    for (; i < WINDOW_SIZE; i++) {
        x[i].r = x[WINDOW_SIZE - i].r;
        x[i].i = -x[WINDOW_SIZE - i].i;
    }
    opus_fft(common.kfft, x, y, 0);
    /* output in reverse order for IFFT. */
    out[0] = WINDOW_SIZE * y[0].r;
    for (i = 1; i < WINDOW_SIZE; i++) {
        out[i] = WINDOW_SIZE * y[WINDOW_SIZE - i].r;
    }
}
/*!
 * 每次对2帧信号加窗
 * @param x 加窗后的信号依然写入到x中
 */
static void apply_window(float *x) {
    int i;
    check_init();
    for (i = 0; i < FRAME_SIZE; i++) {
        x[i] *= common.half_window[i];
        x[WINDOW_SIZE - 1 - i] *= common.half_window[i];
    }
}

int rnnoise_get_size() {
    return sizeof(DenoiseState);
}

int rnnoise_get_frame_size() {
    return FRAME_SIZE;
}

int rnnoise_init(DenoiseState *st, RNNModel *model) {
    memset(st, 0, sizeof(*st));
    if (model)
        st->rnn.model = model;
    else
        st->rnn.model = &rnnoise_model_orig;
    st->rnn.vad_gru_state = calloc(sizeof(float), st->rnn.model->vad_gru_size);
    st->rnn.noise_gru_state = calloc(sizeof(float), st->rnn.model->noise_gru_size);
    st->rnn.denoise_gru_state = calloc(sizeof(float), st->rnn.model->denoise_gru_size);
    return 0;
}

DenoiseState *rnnoise_create(RNNModel *model) {
    DenoiseState *st;
    st = malloc(rnnoise_get_size());
    rnnoise_init(st, model);
    return st;
}

void rnnoise_destroy(DenoiseState *st) {
    free(st->rnn.vad_gru_state);
    free(st->rnn.noise_gru_state);
    free(st->rnn.denoise_gru_state);
    free(st);
}

#if TRAINING
int lowpass = FREQ_SIZE;
int band_lp = NB_BANDS;
#endif

/*!
 * 得到信号傅里叶系数及频带能量
 * @param st DenoiseState结构体
 * @param X 输入信号傅里叶变换得到的复数 数组长度 FREQ_SIZE = 481
 * @param Ex 此帧各频带能量 数组长度 NB_BANDS = 22
 * @param in 抑制电源干扰后的信号帧 数组长度 FRAME_SIZE = 480
 */
static void frame_analysis(DenoiseState *st, kiss_fft_cpx *X, float *Ex, const float *in) {
    int i;
    float x[WINDOW_SIZE]; // 两帧 size=960

    // 两个RNN_COPY实现了滑动窗口
    RNN_COPY(x, st->analysis_mem, FRAME_SIZE);// 从 analysis_mem拷贝FRAME_SIZE个数据赋给x的前半段，analysis_mem是上一次的输入
    for (i = 0; i < FRAME_SIZE; i++) x[FRAME_SIZE + i] = in[i]; // 将输入数据赋给x的后半段
    RNN_COPY(st->analysis_mem, in, FRAME_SIZE);  // 然后再将in拷贝给 analysis_mem

    apply_window(x); // 加窗后的x
    forward_transform(X, x); // X是x傅里叶变换后的系数
#if TRAINING
    for (i=lowpass;i<FREQ_SIZE;i++)
      X[i].r = X[i].i = 0;
#endif
    compute_band_energy(Ex, X); // 计算该帧各频带的能量
}

/*!
 * 计算单帧的特征
 * @param st DenoiseState结构体
 * @param X 输入信号x傅里叶变换后的系数
 * @param P 基音周期pitch傅里叶变换系数
 * @param Ex 此帧各频带能量 数组长度 NB_BANDS = 22
 * @param Ep 基音周期pitch的频带能量计算
 * @param Exp 计算pitch时的相关系数
 * @param features 各特征系数
 * @param in 输入的单帧信号 数组长度 FRAME_SIZE=480
 * @return 是否静音帧
 */
static int compute_frame_features(DenoiseState *st, kiss_fft_cpx *X, kiss_fft_cpx *P,
                                  float *Ex, float *Ep, float *Exp, float *features, const float *in) {
    int i;
    float E = 0;
    float *ceps_0, *ceps_1, *ceps_2;
    float spec_variability = 0;
    float Ly[NB_BANDS];
    float p[WINDOW_SIZE];
    float pitch_buf[PITCH_BUF_SIZE >> 1];
    int pitch_index;
    float gain;
    float *(pre[1]);
    float tmp[NB_BANDS];
    float follow, logMax;
    frame_analysis(st, X, Ex, in); // 得到该帧in的傅里叶系数X和各频带能量Ex
    RNN_MOVE(st->pitch_buf, &st->pitch_buf[FRAME_SIZE], PITCH_BUF_SIZE - FRAME_SIZE); // 也是从源src拷贝给dst n个字节数，不同的是，若src和dst内存有重叠，也能顺利拷贝
    // pitch_buffer长度是1728，这里的意思是将其后面(1728 - 480)个数据放到最前面
    RNN_COPY(&st->pitch_buf[PITCH_BUF_SIZE - FRAME_SIZE], in, FRAME_SIZE);
    pre[0] = &st->pitch_buf[0];
    // pitch估计方法来自opus 中的 pitch.c
    /*
     * 降采样，对pitch_buf平滑降采样，求自相关，利用自相关求lpc系数，然后进行lpc滤波，即得到lpc残差
     */
    pitch_downsample(pre, pitch_buf, PITCH_BUF_SIZE, 1);
    // 寻找基音周期   存入pitch_index 
    pitch_search(pitch_buf + (PITCH_MAX_PERIOD >> 1), pitch_buf, PITCH_FRAME_SIZE,
                 PITCH_MAX_PERIOD - 3 * PITCH_MIN_PERIOD, &pitch_index);
    pitch_index = PITCH_MAX_PERIOD - pitch_index;

    gain = remove_doubling(pitch_buf, PITCH_MAX_PERIOD, PITCH_MIN_PERIOD,
                           PITCH_FRAME_SIZE, &pitch_index, st->last_period, st->last_gain);// 去除高阶谐波影响
    st->last_period = pitch_index;
    st->last_gain = gain; // 根据index得到p[i]
    for (i = 0; i < WINDOW_SIZE; i++)
        p[i] = st->pitch_buf[PITCH_BUF_SIZE - WINDOW_SIZE - pitch_index + i];
    apply_window(p); // pitch数据应用window
    forward_transform(P, p); // 对pitch数据进行傅里叶变换
    compute_band_energy(Ep, P); // 计算pitch部分band能量
    compute_band_corr(Exp, X, P); // 计算X和P的相关系数
    for (i = 0; i < NB_BANDS; i++) Exp[i] = Exp[i] / sqrt(.001 + Ex[i] * Ep[i]); // Exp进行标准化
    dct(tmp, Exp); // 然后再做一次dct，实际上就是信号与pitch相关BFCC了
    for (i = 0; i < NB_DELTA_CEPS; i++) features[NB_BANDS + 2 * NB_DELTA_CEPS + i] = tmp[i]; // features[34,40)
    features[NB_BANDS + 2 * NB_DELTA_CEPS] -= 1.3; // features[34]
    features[NB_BANDS + 2 * NB_DELTA_CEPS + 1] -= 0.9; // features[35]
    features[NB_BANDS + 3 * NB_DELTA_CEPS] = .01 * (pitch_index - 300); // features[40]
    logMax = -2; //而feature的1-NB_BANDS（22）是由log10(Ex)再做一次DCT后填充的，代码如下
    follow = -2;
    for (i = 0; i < NB_BANDS; i++) {
        Ly[i] = log10(1e-2 + Ex[i]);
        Ly[i] = MAX16(logMax - 7, MAX16(follow - 1.5, Ly[i]));
        logMax = MAX16(logMax, Ly[i]);
        follow = MAX16(follow - 1.5, Ly[i]);
        E += Ex[i];
    }
    if (!TRAINING && E < 0.04) {
        /* If there's no audio, avoid messing up the state. */
        RNN_CLEAR(features, NB_FEATURES);
        return 1;
    }
    dct(features, Ly);      // 计算features
     /*
     * cepstral_mem是一个8*22的数组，每一次feature里的值填充到ceps_0,然后这个数组会往下再做一次。
     * ceps_0是float指针，它指向的是ceptral_mem第一个NB_BANDS数组，然后每次与相邻的band数组相见，做出一个delta差值。
     */
    features[0] -= 12;
    features[1] -= 4;
    ceps_0 = st->cepstral_mem[st->memid];
    ceps_1 = (st->memid < 1) ? st->cepstral_mem[CEPS_MEM + st->memid - 1] : st->cepstral_mem[st->memid - 1];
    ceps_2 = (st->memid < 2) ? st->cepstral_mem[CEPS_MEM + st->memid - 2] : st->cepstral_mem[st->memid - 2];
    for (i = 0; i < NB_BANDS; i++) ceps_0[i] = features[i];
    st->memid++;
    for (i = 0; i < NB_DELTA_CEPS; i++) {
        features[i] = ceps_0[i] + ceps_1[i] + ceps_2[i]; // features[0,6)
        features[NB_BANDS + i] = ceps_0[i] - ceps_2[i];  // features[22,28) 倒谱系数
        features[NB_BANDS + NB_DELTA_CEPS + i] = ceps_0[i] - 2 * ceps_1[i] + ceps_2[i]; // features[28, 34] 二阶系数
        // ceps_0[i] - 2 * ceps_1[i] + ceps_2[i] = (ceps_0[i] - ceps_1[i]) - (ceps_1[i] - ceps_2[i]) = 二阶系数
    }
    /* Spectral variability features. */
    if (st->memid == CEPS_MEM) st->memid = 0;
    // 最后一个特性值
    for (i = 0; i < CEPS_MEM; i++) {
        int j;
        float mindist = 1e15f;
        for (j = 0; j < CEPS_MEM; j++) {
            int k;
            float dist = 0;
            for (k = 0; k < NB_BANDS; k++) {
                float tmp;
                tmp = st->cepstral_mem[i][k] - st->cepstral_mem[j][k];
                dist += tmp * tmp;
            }
            if (j != i)
                mindist = MIN32(mindist, dist);
        }
        spec_variability += mindist;
    }
    //应该是最后一个特征谱稳度
    features[NB_BANDS + 3 * NB_DELTA_CEPS + 1] = spec_variability / CEPS_MEM - 2.1; // features[41] 特殊的非平稳值,用于检测语音

    return TRAINING && E < 0.1;
}

/*!
 * 语音帧合成
 * @param st DenoiseState结构体
 * @param out 合成的语音帧
 * @param y 该帧的傅里叶变换系数
 */
static void frame_synthesis(DenoiseState *st, float *out, const kiss_fft_cpx *y) {
    float x[WINDOW_SIZE];
    int i;
    inverse_transform(x, y);
    apply_window(x);
    for (i = 0; i < FRAME_SIZE; i++) out[i] = x[i] + st->synthesis_mem[i];
    RNN_COPY(st->synthesis_mem, &x[FRAME_SIZE], FRAME_SIZE);
}

/*!
 * 二阶滤波器 无限脉冲响应滤波器 IIR ref:https://arachnoid.com/BiQuadDesigner/index.html
 * Biquadractic Filter求解如下：
 * y(n) = x(n) + b0*x(n-1) + b1*x(n-2) - a0*y(n-1) - a1*y(n-2) // 实际上x(n)的系数为1
 * @param y 滤波后时域信号
 * @param mem 计算biquad的中间过程 mem[1] = b1*x(n-2) - a1*y(n-2)
 * mem[0] = b1*x(n-2) - a1*y(n-2) + b0*x(n-1) - a0*y(n-1)
 * @param x 待滤波的时域信号
 * @param b 系数b
 * @param a 系数a
 * @param N N点信号滤波
 */
static void biquad(float *y, float mem[2], const float *x, const float *b, const float *a, int N) {
    int i;
    for (i = 0; i < N; i++) {
        float xi, yi;
        xi = x[i];
        yi = x[i] + mem[0];
        mem[0] = mem[1] + (b[0] * (double) xi - a[0] * (double) yi);
        mem[1] = (b[1] * (double) xi - a[1] * (double) yi);
        y[i] = yi;
    }
}

/*!
 * 用于过滤pitch谐波之间的噪声
 * @param X 信号帧的傅里叶变换系数
 * @param P 基音周期pitch傅里叶变换系数
 * @param Ex 此帧各频带能量 数组长度 NB_BANDS = 22
 * @param Ep 基音周期pitch的频带能量计算
 * @param Exp 计算pitch时的相关系数
 * @param g 每个频带的增益 gain = sqrt(Energy(clean speech) / Energy(noisy speech)); 即 idea ratio mask(IRM)
 */
void pitch_filter(kiss_fft_cpx *X, const kiss_fft_cpx *P, const float *Ex, const float *Ep,
                  const float *Exp, const float *g) {
    int i;
    float r[NB_BANDS];
    float rf[FREQ_SIZE] = {0};
    for (i = 0; i < NB_BANDS; i++) {
#if 0
        if (Exp[i]>g[i]) r[i] = 1;
        else r[i] = Exp[i]*(1-g[i])/(.001 + g[i]*(1-Exp[i]));
        r[i] = MIN16(1, MAX16(0, r[i]));
#else
        if (Exp[i] > g[i]) r[i] = 1;
        else r[i] = SQUARE(Exp[i]) * (1 - SQUARE(g[i])) / (.001 + SQUARE(g[i]) * (1 - SQUARE(Exp[i])));
        r[i] = sqrt(MIN16(1, MAX16(0, r[i]))); // r[i]就是论文中的alphab Exp就是论文中的pb
#endif
        r[i] *= sqrt(Ex[i] / (1e-8 + Ep[i]));
    }
    interp_band_gain(rf, r);
    for (i = 0; i < FREQ_SIZE; i++) {
        X[i].r += rf[i] * P[i].r;
        X[i].i += rf[i] * P[i].i;
    }
    float newE[NB_BANDS];
    compute_band_energy(newE, X);
    float norm[NB_BANDS];
    float normf[FREQ_SIZE] = {0};
    for (i = 0; i < NB_BANDS; i++) {
        norm[i] = sqrt(Ex[i] / (1e-8 + newE[i]));  // 重整信号, 使每个频带的信号具有和原始信号X(k)相同的能量
    }
    interp_band_gain(normf, norm);
    for (i = 0; i < FREQ_SIZE; i++) {
        X[i].r *= normf[i];
        X[i].i *= normf[i];
    }
}

/*!
 *
 * @param st DenoiseState结构体
 * @param out 输出帧数据
 * @param in 输入帧数据
 * @return vad_prob 语音活动检测范围(0,1), 0表示无话音
 */
float rnnoise_process_frame(DenoiseState *st, float *out, const float *in) {
    int i;
    kiss_fft_cpx X[FREQ_SIZE];
    kiss_fft_cpx P[WINDOW_SIZE];
    float x[FRAME_SIZE];
    float Ex[NB_BANDS], Ep[NB_BANDS];
    float Exp[NB_BANDS];
    float features[NB_FEATURES];
    float g[NB_BANDS];
    float gf[FREQ_SIZE] = {1};
    float vad_prob = 0;
    int silence;
    static const float a_hp[2] = {-1.99599, 0.99600};
    static const float b_hp[2] = {-2, 1};
    biquad(x, st->mem_hp_x, in, b_hp, a_hp, FRAME_SIZE); // high pass 高通滤波 抑制50Hz或60Hz的电源干扰
    silence = compute_frame_features(st, X, P, Ex, Ep, Exp, features, x);

    if (!silence) { // 非静音帧
        compute_rnn(&st->rnn, g, &vad_prob, features);
        pitch_filter(X, P, Ex, Ep, Exp, g);
        for (i = 0; i < NB_BANDS; i++) {
            float alpha = .6f;
            g[i] = MAX16(g[i], alpha * st->lastg[i]);
            st->lastg[i] = g[i];
        }
        interp_band_gain(gf, g);
#if 1
        for (i = 0; i < FREQ_SIZE; i++) {
            X[i].r *= gf[i];
            X[i].i *= gf[i];
        }
#endif
    }
    frame_synthesis(st, out, X);
    return vad_prob;
}

#if TRAINING

static float uni_rand() {
  return rand()/(double)RAND_MAX-.5;
}

static void rand_resp(float *a, float *b) {
  a[0] = .75*uni_rand();
  a[1] = .75*uni_rand();
  b[0] = .75*uni_rand();
  b[1] = .75*uni_rand();
}

int main(int argc, char **argv) {
  int i;
  int count=0;
  static const float a_hp[2] = {-1.99599, 0.99600};
  static const float b_hp[2] = {-2, 1};
  float a_noise[2] = {0};
  float b_noise[2] = {0};
  float a_sig[2] = {0};
  float b_sig[2] = {0};
  float mem_hp_x[2]={0};
  float mem_hp_n[2]={0};
  float mem_resp_x[2]={0};
  float mem_resp_n[2]={0};
  float x[FRAME_SIZE];
  float n[FRAME_SIZE];
  float xn[FRAME_SIZE];
  int vad_cnt=0;
  int gain_change_count=0;
  float speech_gain = 1, noise_gain = 1;
  FILE *f1, *f2;
  int maxCount;
  DenoiseState *st;
  DenoiseState *noise_state;
  DenoiseState *noisy;
  st = rnnoise_create(NULL);
  noise_state = rnnoise_create(NULL);
  noisy = rnnoise_create(NULL);
  if (argc!=4) {
    fprintf(stderr, "usage: %s <speech> <noise> <count>\n", argv[0]);
    return 1;
  }
  f1 = fopen(argv[1], "r");
  f2 = fopen(argv[2], "r");
  maxCount = atoi(argv[3]);
  for(i=0;i<150;i++) {
    short tmp[FRAME_SIZE];
    fread(tmp, sizeof(short), FRAME_SIZE, f2);
  }
  while (1) {
    kiss_fft_cpx X[FREQ_SIZE], Y[FREQ_SIZE], N[FREQ_SIZE], P[WINDOW_SIZE];
    float Ex[NB_BANDS], Ey[NB_BANDS], En[NB_BANDS], Ep[NB_BANDS];
    float Exp[NB_BANDS];
    float Ln[NB_BANDS];
    float features[NB_FEATURES];
    float g[NB_BANDS];
    short tmp[FRAME_SIZE];
    float vad=0;
    float E=0;
    if (count==maxCount) break;
    if ((count%1000)==0) fprintf(stderr, "%d\r", count);
    if (++gain_change_count > 2821) {
      speech_gain = pow(10., (-40+(rand()%60))/20.);
      noise_gain = pow(10., (-30+(rand()%50))/20.);
      if (rand()%10==0) noise_gain = 0;
      noise_gain *= speech_gain;
      if (rand()%10==0) speech_gain = 0;
      gain_change_count = 0;
      rand_resp(a_noise, b_noise);
      rand_resp(a_sig, b_sig);
      lowpass = FREQ_SIZE * 3000./24000. * pow(50., rand()/(double)RAND_MAX);
      for (i=0;i<NB_BANDS;i++) {
        if (eband5ms[i]<<FRAME_SIZE_SHIFT > lowpass) {
          band_lp = i;
          break;
        }
      }
    }
    if (speech_gain != 0) {
      fread(tmp, sizeof(short), FRAME_SIZE, f1);
      if (feof(f1)) {
        rewind(f1);
        fread(tmp, sizeof(short), FRAME_SIZE, f1);
      }
      for (i=0;i<FRAME_SIZE;i++) x[i] = speech_gain*tmp[i];
      for (i=0;i<FRAME_SIZE;i++) E += tmp[i]*(float)tmp[i];
    } else {
      for (i=0;i<FRAME_SIZE;i++) x[i] = 0;
      E = 0;
    }
    if (noise_gain!=0) {
      fread(tmp, sizeof(short), FRAME_SIZE, f2);
      if (feof(f2)) {
        rewind(f2);
        fread(tmp, sizeof(short), FRAME_SIZE, f2);
      }
      for (i=0;i<FRAME_SIZE;i++) n[i] = noise_gain*tmp[i];
    } else {
      for (i=0;i<FRAME_SIZE;i++) n[i] = 0;
    }
    biquad(x, mem_hp_x, x, b_hp, a_hp, FRAME_SIZE);
    biquad(x, mem_resp_x, x, b_sig, a_sig, FRAME_SIZE);
    biquad(n, mem_hp_n, n, b_hp, a_hp, FRAME_SIZE);
    biquad(n, mem_resp_n, n, b_noise, a_noise, FRAME_SIZE);
    for (i=0;i<FRAME_SIZE;i++) xn[i] = x[i] + n[i];
    if (E > 1e9f) {
      vad_cnt=0;
    } else if (E > 1e8f) {
      vad_cnt -= 5;
    } else if (E > 1e7f) {
      vad_cnt++;
    } else {
      vad_cnt+=2;
    }
    if (vad_cnt < 0) vad_cnt = 0;
    if (vad_cnt > 15) vad_cnt = 15;

    if (vad_cnt >= 10) vad = 0;
    else if (vad_cnt > 0) vad = 0.5f;
    else vad = 1.f;

    frame_analysis(st, Y, Ey, x);
    frame_analysis(noise_state, N, En, n);
    for (i=0;i<NB_BANDS;i++) Ln[i] = log10(1e-2+En[i]);
    int silence = compute_frame_features(noisy, X, P, Ex, Ep, Exp, features, xn);
    pitch_filter(X, P, Ex, Ep, Exp, g);
    //printf("%f %d\n", noisy->last_gain, noisy->last_period);
    for (i=0;i<NB_BANDS;i++) {
      g[i] = sqrt((Ey[i]+1e-3)/(Ex[i]+1e-3));
      if (g[i] > 1) g[i] = 1;
      if (silence || i > band_lp) g[i] = -1;
      if (Ey[i] < 5e-2 && Ex[i] < 5e-2) g[i] = -1;
      if (vad==0 && noise_gain==0) g[i] = -1;
    }
    count++;
#if 1
    fwrite(features, sizeof(float), NB_FEATURES, stdout);
    fwrite(g, sizeof(float), NB_BANDS, stdout);
    fwrite(Ln, sizeof(float), NB_BANDS, stdout);
    fwrite(&vad, sizeof(float), 1, stdout);
#endif
  }
  fprintf(stderr, "matrix size: %d x %d\n", count, NB_FEATURES + 2*NB_BANDS + 1);
  fclose(f1);
  fclose(f2);
  return 0;
}

#endif
