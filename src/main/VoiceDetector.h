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
#define BEGIN_STEP1     1   // 初期化ステップ1
#define BEGIN_STEP2     2   // 初期化ステップ2
#define RESULT_SUCCESS  0   // 成功
#define RESULT_ERROR    0x10000;            // エラー
#define RESULT_TIMEOUT  (RESULT_ERROR + 1)  // タイムアウト
#define RESULT_CANCEL   (RESULT_ERROR + 2)  // キャンセル

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
};