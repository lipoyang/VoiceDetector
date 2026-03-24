#if (SUBCORE != 1)
#error "Core selection is wrong!!"
#endif

#include <Arduino.h>
#include <MP.h>
#include "VoiceDetector.h"

// コアID
const int MAINCORE_ID = 0;

// メッセージID定義
const int8_t MSGID_BEGUN        = 1;  // S->M 初期化完了通知
const int8_t MSGID_SHARE_MEMORY = 2;  // M->S 共有メモリ通知 
const int8_t MSGID_REQ_REGIST   = 3;  // M->S コマンド登録開始要求
const int8_t MSGID_REQ_DETECT   = 4;  // M->S コマンド検出開始要求
const int8_t MSGID_REQ_CANCEL   = 5;  // M->S コマンド登録/検出キャンセル
const int8_t MSGID_MIC_DATA     = 6;  // M->S マイク音声データ通知
const int8_t MSGID_ON_REGIST    = 7;  // S->M コマンド登録通知
const int8_t MSGID_ON_DETECT    = 8;  // S->M コマンド検出通知
const int8_t MSGID_REQ_LOAD     = 9;  // M->S MFCCデータのロード要求
const int8_t MSGID_RES_LOAD     = 10; // S->M MFCCデータのロード応答 

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

  // 初期化完了をメインコアに知らせる
  uint32_t dummy = 0;
  MP.Send(MSGID_BEGUN, dummy, MAINCORE_ID);

  // 共有メモリを取得する
  MP.RecvTimeout(MP_RECV_BLOCKING);
  int8_t msgid;
  int16_t *msgdata;
  do {
    MP.Recv(&msgid, &msgdata);
    if(msgid == MSGID_SHARE_MEMORY){
      break;
    }else{
      MPLog("Unexpected message %d\n", msgid);
    }
  } while(1);

  // 音声コマンド検出器を初期化
  int16_t *voiceBuffer = msgdata;
  const size_t VOICE_BUFF_SIZE = 96000;
  uint8_t *fileBuffer = &((uint8_t*)voiceBuffer)[VOICE_BUFF_SIZE]; // ※ voiceBufferの後に配置 (バッドノウハウ)
  vd.begin(voiceBuffer, fileBuffer);
  vd.state = VD_IDLE;

  // MFCCデータのロード
  uint32_t mfcc_no;
  while(1){
    do {
      MP.Recv(&msgid, &mfcc_no);
      if(msgid == MSGID_REQ_LOAD){
        break;
      }else{
        MPLog("Unexpected message ID %d\n", msgid);
      }
    } while(1);

    if(mfcc_no == MFCC_END){
      // MPLog("MFCC load completed\n");
      MP.Send(MSGID_RES_LOAD, mfcc_no, MAINCORE_ID);
      break;
    }
    else if(mfcc_no >= MFCC_0 && mfcc_no <= MFCC_4){
      bool result = vd.loadFile(mfcc_no);
      if(result == false){
        MPLog("Failed to load MFCC[%ld]\n", mfcc_no);
        MP.Send(MSGID_RES_LOAD, RESULT_ERROR, MAINCORE_ID);
      }else{
        MP.Send(MSGID_RES_LOAD, mfcc_no, MAINCORE_ID);
      }
    }
    else{
      MPLog("Unexpected message DATA (%ld)\n", mfcc_no);
    }
  }
  
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
        if(msgdata <= VD_REGIST4){
          vd.state = msgdata;
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
      // マイク音声データ通知
      case MSGID_MIC_DATA:
        vd.putMicData((int16_t*)msgdata);
        break;
      default:
        MPLog("Unknown msgid %d\n", msgid);
        vd.state = VD_IDLE;
        break;
      }
  }
  // コマンド登録処理
  if(vd.state >= VD_REGIST0 && vd.state <= VD_REGIST4){
    bool ret = vd.regist(vd.state);
    if(ret == true){
      ret = vd.saveFile(vd.state);
      if(ret == true){
        MP.Send(MSGID_ON_REGIST, (uint32_t)vd.state, MAINCORE_ID);
      }else{
        MPLog("Failed to save MFCC\n");
        MP.Send(MSGID_ON_REGIST, RESULT_ERROR, MAINCORE_ID);
      }
      vd.state = VD_IDLE;
    }
  }
  // コマンド検出処理
  else if(vd.state == VD_DETECT){
    int command = vd.detect();
    if(command >= 0){
      MP.Send(MSGID_ON_DETECT, command, MAINCORE_ID);
      vd.state = VD_IDLE;
    }
  }
}
