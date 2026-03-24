#ifdef SUBCORE
#error "Core selection is wrong!!"
#endif

#include <Arduino.h>
#include <SDHCI.h>
#include "VoiceDetector.h"

// SDカード
SDClass SD;

// 音声コマンド検出器
VoiceDetector vd;

// コマンド登録通知
void onRegist(uint32_t commnad_no)
{
  if(commnad_no < MAX_COMMAND){
    printf("Voice Command Registed! (%ld)\n", commnad_no);
  }else{
    printf("Voice Command Regist ERROR! (%ld)\n", commnad_no);
  }
}

// コマンド検出通知
void onDetect(uint32_t commnad_no)
{
  if(commnad_no < MAX_COMMAND){
    printf("Voice Command Detected! (%ld)\n", commnad_no);
  }else if(commnad_no == MFCC_MISMATCH){
    printf("Voice Command mismatch\n");
  }else{
    printf("Voice Command Detect ERROR! (%ld)\n", commnad_no);
  }
}

// 初期化
void setup()
{
  pinMode(LED0, OUTPUT);
  pinMode(LED1, OUTPUT);
  pinMode(LED2, OUTPUT);
  pinMode(LED3, OUTPUT);

  Serial.begin(115200);
  while(!Serial){ delay(100); }
  Serial.println("Hello!");
  
  while (!SD.begin()) {
    Serial.print(".");
    delay(500);
  }

  // 音声コマンド検出器の初期化
  vd.onRegist = onRegist;
  vd.onDetect = onDetect;
  vd.begin();
}

// メインループ
void loop()
{
  if(vd.isIdle()){
    printf("Please Input 0-4 or 5\n");
    printf("0-4:Regist Voice Command / 5:Detect Voice Command\n");
    char c = 0;
    while (1) {
      if(Serial.available() > 0){
      c = Serial.read();
      while(Serial.available() > 0){}
      break;
      }
    }
    int num = c - '0';

    // 0-4 : コマンド登録
    if (num >= VD_REGIST0 && num <= VD_REGIST4) {
      printf("Regist Voice Command (%d)\n", num);
      vd.regist(num);
    }
    // 5 : コマンド検出
    else if (num == VD_DETECT) {
      printf("Compare Wake Word\n");
      vd.detect();
    }
    else {
        printf("Wrong Command! (%c)\n", c);
    }
    printf("\n");
  }else{
    // Q : 中断
    if(Serial.available() > 0){
      char c = Serial.read();
      if(c == 'q' || c == 'Q')
      {
        printf("Cancel!\n");
        vd.cancel();
      }
    }
  }

  // 音声コマンド検出器のメインループ処理
  vd.loop();

  delay(1);
}
