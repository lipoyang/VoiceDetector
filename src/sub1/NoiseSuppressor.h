#pragma once
#include <stdint.h>
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846f
#endif

// 双二次フィルタ
class BiQuad {
public:
    float b0, b1, b2;
    float a0, a1, a2;
    float z1, z2;

	// LPF(ローパスフィルタ)の係数を計算してセット
	// fs : サンプリング周波数
	// fc : カットオフ周波数
	// Q : 品質係数（Q=0.707 で Butterworth 特性）
    void makeLpf(float fs, float fc, float Q)
    {
        // レシピ: RBJ Audio EQ Cookbook

        float w0 = 2.0f * M_PI * fc / fs;
        float cosw0 = cosf(w0);
        float sinw0 = sinf(w0);
        float alpha = sinw0 / (2.0f * Q);

        float b0 = (1.0f - cosw0) * 0.5f;
        float b1 =  1.0f - cosw0;
        float b2 = (1.0f - cosw0) * 0.5f;
        float a0 =  1.0f + alpha;
        float a1 = -2.0f * cosw0;
        float a2 =  1.0f - alpha;

        this->b0 = b0 / a0;
        this->b1 = b1 / a0;
        this->b2 = b2 / a0;
        this->a0 = 1.0f;
        this->a1 = a1 / a0;
        this->a2 = a2 / a0;
        this->z1 = this->z2 = 0.0f;
    }

    // HPF(ハイパスフィルタ)の係数を計算してセット
    // fs : サンプリング周波数
    // fc : カットオフ周波数
    // Q : 品質係数（Q=0.707 で Butterworth 特性）
    void makeHpf(float fs, float fc, float Q)
    {
        // レシピ: RBJ Audio EQ Cookbook

        float w0 = 2.0f * M_PI * fc / fs;
        float cosw0 = cosf(w0);
        float sinw0 = sinf(w0);
        float alpha = sinw0 / (2.0f * Q);

        float b0 =  (1.0f + cosw0) * 0.5f;
        float b1 = -(1.0f + cosw0);
        float b2 =  (1.0f + cosw0) * 0.5f;
        float a0 =   1.0f + alpha;
        float a1 =  -2.0f * cosw0;
        float a2 =   1.0f - alpha;

        this->b0 = b0 / a0;
        this->b1 = b1 / a0;
        this->b2 = b2 / a0;
        this->a0 = 1.0f;
        this->a1 = a1 / a0;
        this->a2 = a2 / a0;
        this->z1 = this->z2 = 0.0f;
    }

    // フィルタ処理
    // x : 入力サンプル
    // 戻り値 : 出力サンプル
    float process(float x)
    {
        // Direct Form I / II のハイブリッド (？)
        float y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }
};

// 簡易ノイズゲート（短時間 RMS がしきい値以下ならレベルを下げる）
class NoiseGate {
public:
    float rms;
    float alpha;      // 平滑係数
    float threshold;  // しきい値
    float ratio;      // ゲート時の減衰（0.0〜1.0）

	// 初期化
	// attack_ms : RMS の平滑に使う時間定数（ms）
	// fs : サンプリング周波数
	// threshold : RMS のしきい値
	// ratio : ゲート時の減衰（0.0〜1.0）
    void init(float attack_ms, float fs, float threshold, float ratio)
    {
        float attack_sec = attack_ms / 1000.0f;
        this->alpha = expf(-1.0f / (attack_sec * fs)); // 単純な一次 IIR
        this->rms = 0.0f;
        this->threshold = threshold * threshold; // ※ ここで2乗しておく
        this->ratio = ratio;
    }

	// ノイズゲート処理
	// x : 入力サンプル
	// 戻り値 : 出力サンプル
    float process(float x)
    {
        float x2 = x * x;
        this->rms = this->alpha * this->rms + (1.0f - this->alpha) * x2;
        // float level = sqrtf(this->rms); // ※ ここでは平方根を計算しない

        // if (level < this->threshold) {
        if (rms < this->threshold) {
                return x * this->ratio; // 小さいときは減衰
        }
        else {
            return x;
        }
    }
};

// ノイズ抑制器クラス
class NoiseSuppressor {
public:
	// コンストラクタ
    NoiseSuppressor() {}
    NoiseSuppressor(float frame_time_ms, float threshold, float fs) { 
        init(frame_time_ms, threshold, fs);
    }

	// 初期化
	// frame_time_ms : フレーム時間[ms]
	// threshold : ノイズゲートのしきい値[%]
	// fs : サンプリング周波数[Hz]
    void init(float frame_time_ms, float threshold, float fs)
    {
		// サンプリング周波数
		this->fs = fs;
		this->frame_size = (int)(fs * frame_time_ms / 1000.0f);

		// バンドパスフィルタ: 低域ノイズと高域ノイズを削減
        // だいたい 200 Hz〜4000 Hz を通す
        hpf.makeHpf(fs, 200.0f, 0.707f);
        lpf.makeLpf(fs, 4000.0f, 0.707f);

        // ノイズゲート: しきい値は経験的に調整
        gate.init(10.0f, fs, threshold/100.0f, 0.1f);
	}

    // ノイズ抑制処理
    // in : 入力サンプル（16-bit PCM）
    // out : 出力サンプル（16-bit PCM）
    // n_samples : サンプル数
    void process(const int16_t* in, int16_t* out)
    {
        process(in, out, this->frame_size);
	}

    // ノイズ抑制処理
	// in : 入力サンプル（16-bit PCM）
	// out : 出力サンプル（16-bit PCM）
	// n_samples : サンプル数
    void process(const int16_t* in, int16_t* out, int n_samples)
    {
        for (int i = 0; i < n_samples; ++i) {
            float x = (float)in[i] / 32768.0f;

            // HPF → LPF → ノイズゲート
            float y = hpf.process(x);
            y = lpf.process(y);
            y = gate.process(y);

            // クリップ
            if (y > 1.0f)  y = 1.0f;
            if (y < -1.0f) y = -1.0f;

            out[i] = (int16_t)(y * 32767.0f);
        }
    }

private:
	float fs;
	int frame_size;
    BiQuad hpf, lpf;
    NoiseGate gate;
};