#pragma once

// 状態
#define VD_IDLE     -1  // 待機
#define VD_REGIST0   0  // コマンド登録 0
#define VD_REGIST1   1  // コマンド登録 1
#define VD_REGIST2   2  // コマンド登録 2
#define VD_REGIST3   3  // コマンド登録 3
#define VD_REGIST4   4  // コマンド登録 4
#define VD_DETECT    5  // コマンド検出

// 最大コマンド登録数
#define MAX_COMMAND  5 

// メッセージコード
#define RESULT_SUCCESS  0   // 成功
#define RESULT_ERROR    0x10000;            // エラー
#define RESULT_TIMEOUT  (RESULT_ERROR + 1)  // タイムアウト
#define RESULT_CANCEL   (RESULT_ERROR + 2)  // キャンセル
#define MFCC_0          0   // コマンド0のMFCC
#define MFCC_1          1   // コマンド1のMFCC  
#define MFCC_2          2   // コマンド2のMFCC
#define MFCC_3          3   // コマンド3のMFCC
#define MFCC_4          4   // コマンド4のMFCC
#define MFCC_END        0x10000 // MFCCのロード終了

// 音声コマンド検出器
class VoiceDetector
{
public:
    void begin();
    void loop();
    void regist(int command_no);
    void detect();
    void cancel();

    bool isIdle() {return (state == VD_IDLE);}

    void (*onRegist)(int result) = nullptr;
    void (*onDetect)(int commnad_no) = nullptr;
    void (*onError)(int error_no) = nullptr;

private:
    int state;

    int loadFile(int command_no);
    int saveFile(int command_no);
};