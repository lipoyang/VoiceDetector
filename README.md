# VoiceDetector
SPRESENSE用音声コマンド検出器

## 設計メモ

### MainCore
#### DirectMic
* /dev/audio/pcm_in0 のサンプリング周波数は 48kHz 固定っぽい
* モノラル設定でもLR同じ値がニコイチで来のでデータ量としては 96kSa/s
* /dev/audio/pcm_in0 のバッファサイズは 4096バイト固定っぽい
* 16ビットPCM だと 4096バイト = 2048サンプル(実質1024サンプル) = 21.33msec
* バッファ8段 = 8×4096バイト = 32kバイト = 8×21.33msec = 170.66msec
* 21.33msec ごとに 4096バイトを VoiceDetector に渡す

#### VoiceDetector
* 16kSa/sにダウンサンプルする
* 1600バイト = 800サンプル = 50msec ごとにバッファする
* バッファ8段 = 8×1600バイト = 12.5kバイト = 8×50msec = 400msec
* 50msec ごとに サブコアに渡す

### SubCore
#### VoiceDetector
* メッセージで受けたデータをFIFOに入れて順次処理
* MPライブラリのメッセージキューは8段で、詰まるとプログラムは assert する (仕様)
* FIFOは8個以上を受け付けない
* VAD処理は 1フレーム = 10msec なので 50msec = 5フレーム
