#pragma once
#include <stdint.h>

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

// マイク用バッファ制御構造体
struct RingBufferCtrl{
    int frame_size;
    int frame_total;
    int w_frame;
    int r_frame;

    bool available() {return (r_frame != w_frame);}
    int  r_index() {return (r_frame * frame_size);}
    void r_frame_inc(){
        int next_frame = r_frame + 1;
        if(next_frame >= frame_total){
            next_frame = 0;
        }
        r_frame = next_frame;
    }
};

// 音声コマンド検出器
class VoiceDetector
{
public:
    void begin(int16_t *voiceBuffer, int16_t *micBuffer, RingBufferCtrl *ring);
    bool regist();
    int  detect();

    int state;

private:
    int16_t* rxMic();
    
    RingBufferCtrl *ring;
};
