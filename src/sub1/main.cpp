#if (SUBCORE != 1)
#error "Core selection is wrong!!"
#endif

#include <Arduino.h>
#include <MP.h>
#include "VoiceDetector.h"

// コアID
const int MAINCORE_ID = 0;

// メッセージID定義
const int8_t MSGID_BEGUN = 1;         // S->M 初期化完了通知
const int8_t MSGID_SHARE_MEMORY = 2;  // M->S 共有メモリ通知 
const int8_t MSGID_REQ_REGIST = 3;    // M->S コマンド登録開始要求
const int8_t MSGID_REQ_DETECT = 4;    // M->S コマンド検出開始要求
const int8_t MSGID_REQ_CANCEL = 5;    // M->S コマンド登録/検出キャンセル
const int8_t MSGID_ON_REGIST = 5;     // S->M コマンド登録通知
const int8_t MSGID_ON_DETECT = 6;     // S->M コマンド検出通知
const int8_t MSGID_ON_ERROR = 7;      // S->M エラー通知

// メッセージデータ
// 共有メモリ通知用
struct S_SharedMemory{
  int16_t *voiceBuffer;
  int16_t *micBuffer;
  RingBufferCtrl *ring;
};

// 音声コマンド検出器
VoiceDetector vd;

// エラーループ
void errorLoop(int num)
{
  int i;
  while (1) {
    for (i = 0; i < num; i++) {
      ledOn(LED1);
      delay(300);
      ledOff(LED1);
      delay(300);
    }
    delay(1000);
  }
}

// 初期化
void setup()
{
  // サブコア開始
  int ret = MP.begin();
  if (ret < 0) {
    errorLoop(2);
  }

  // 初期化完了をメインコアに知らせる(1)
  MP.Send(MSGID_BEGUN, BEGIN_STEP1, MAINCORE_ID);

  // 共有メモリを取得する
  MP.RecvTimeout(MP_RECV_BLOCKING);
  int8_t msgid;
  S_SharedMemory *msgdata;
  do {
    MP.Recv(&msgid, &msgdata);
    if(msgid == MSGID_SHARE_MEMORY){
      break;
    }else{
      MPLog("Unexpected message %d\n", msgid);
    }
  } while(1);

  // 音声コマンド検出器を初期化
  int16_t *voiceBuffer = msgdata->voiceBuffer;
  int16_t *micBuffer   = msgdata->micBuffer;
  RingBufferCtrl *ring = msgdata->ring;
  vd.begin(voiceBuffer, micBuffer, ring);
  vd.state = VD_IDLE;

  // 初期化完了をメインコアに知らせる(2)
  MP.Send(MSGID_BEGUN, BEGIN_STEP2, MAINCORE_ID);
  MP.RecvTimeout(MP_RECV_POLLING);
}

// メインループ
void loop()
{
  // メッセージ受信
  int8_t msgid;
  uint32_t msgdata;
  int ret = MP.Recv(&msgid, &msgdata);
  if(ret < 0){
    if(ret != -EAGAIN) {
      MPLog("MP.Recv error %d\n", ret);
      return;
    }
  }else{
    switch(ret){
      // コマンド登録開始要求
      case MSGID_REQ_REGIST:
        if(msgdata < MAX_COMMAND){
          vd.state = VD_REGIST0 + msgdata;
        }else{
          MPLog("Wrong command no (%d)\n", msgdata);
        }
        break;
      // コマンド検出開始要求
      case MSGID_REQ_DETECT:
        vd.state = VD_DETECT;
        break;
      // コマンド登録/検出キャンセル
      case MSGID_REQ_CANCEL:
        vd.state = VD_IDLE;
        break;
      default:
        MPLog("Unknown msgid %d\n", msgid);
        vd.state = VD_IDLE;
        break;
      }
  }
  // コマンド登録処理
  if(vd.state >= VD_REGIST0 && vd.state <= VD_REGIST4){
    bool ret = vd.detect();
    if(ret == true){
      MP.Send(MSGID_ON_REGIST, (uint32_t)RESULT_SUCCESS);
      vd.state = VD_IDLE;
    }
  }
  // コマンド検出処理
  else if(vd.state == VD_DETECT){
    int command = vd.detect();
    if(command >= 0){
      MP.Send(MSGID_ON_DETECT, command);
      vd.state = VD_IDLE;
    }
  }
}
