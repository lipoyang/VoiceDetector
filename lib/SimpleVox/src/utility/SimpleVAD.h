#pragma once

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <math.h>

// VAD（音声アクティビティ検出)
class SimpleVAD {
public:
    int ampTreshold; // 振幅判定閾値
	int fh; // ゼロクロス判定の高い方の閾値[Hz]
	int fl; // ゼロクロス判定の低い方の閾値[Hz]

	int amp; // 振幅の平均値
	int zcr; // ゼロクロスのカウント
	bool isSpeech; // 音声判定結果

	// コンストラクタ
	// level : 0〜4 のレベル (高いほど判定が厳しい)
	SimpleVAD(int level)
    {
        this->ampTreshold = 100 * (level + 1);
        this->fh = 5000;
		this->fl = 200;

        this->amp = 0;
        this->zcr = 0;
        this->isSpeech = false;
    }

	// 音声判定
	// buffer : 入力サンプル（16-bit PCM）
	// size : サンプル数
	// 戻り値 : true = 音声あり, false = 無音
    bool process(const int16_t* buffer, int sample_rate, int frame_time_ms)
    {
		int size = (int)(sample_rate * frame_time_ms / 1000);

        // ゼロクロス判定閾値
        int zcrL = 2 * fl * size / sample_rate;
        int zcrH = 2 * fh * size / sample_rate;

        int ave = 0;
		int zcr = 0;
        for (int i = 0; i < size; i++) {
			// 振幅の平均値
            ave += abs((int)buffer[i]);

			// ゼロクロスのカウント
            if (i > 0) {
                if ((buffer[i - 1] > 0 && buffer[i] < 0) ||
                    (buffer[i - 1] < 0 && buffer[i] > 0)) {
                    zcr++;
                }
            }
        }
		ave /= size;
		this->amp = ave;
		this->zcr = zcr;

        // 判定
        if (ave > ampTreshold && zcr > zcrL && zcr < zcrH) {
            this->isSpeech = true; // 音声あり
        } else {
            this->isSpeech = false; // 無音
        }
		return this->isSpeech;
    }
};
