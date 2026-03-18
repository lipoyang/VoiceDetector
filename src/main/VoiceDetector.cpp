// 音声コマンド検出器

#include <Arduino.h>
#include <MP.h>
#include <Audio.h>
#include "VoiceDetector.h"

// オーディオ
AudioClass *theAudio;

// マイク用バッファ制御構造体
struct RingBufferCtrl{
    int frame_size;
    int frame_total;
    int w_frame;
    int r_frame;
    void clear(){
        w_frame = r_frame = 0;
    }
    int  w_index() {return (w_frame * frame_size);}
    bool w_frame_inc(){
        int next_frame = w_frame + 1;
        if(next_frame >= frame_total){
            next_frame = 0;
        }
        if(next_frame == r_frame){
            return false;
        }else{
            w_frame = next_frame;
            return true;
        }
    }
};

// コアID
const int SUBCORE_VD  = 1;

// メッセージID定義
const int8_t MSGID_BEGUN = 1;         // S->M 初期化完了通知
const int8_t MSGID_SHARE_MEMORY = 2;  // M->S 共有メモリ通知 
const int8_t MSGID_REQ_REGIST = 3;    // M->S コマンド登録開始要求
const int8_t MSGID_REQ_DETECT = 4;    // M->S コマンド検出開始要求
const int8_t MSGID_REQ_CANCEL = 5;    // M->S コマンド登録/検出キャンセル
const int8_t MSGID_ON_REGIST = 5;     // S->M コマンド登録通知
const int8_t MSGID_ON_DETECT = 6;     // S->M コマンド検出通知
const int8_t MSGID_ON_ERROR = 7;      // S->M エラー通知

// 定数
const int SAMPLE_RATE    = 16000;   // サンプリング周波数 16kHz
const int VOICE_BUFF_SEC = 3;       // 音声コマンド登録用バッファ 3秒ぶん
const int VAD_FRAME_MSEC = 10;      // VADフレーム 10msec
const int MIC_BUFF_FRAMES = 3;      // マイクバッファのVADフレーム数 3フレーム

// メッセージデータ
// 共有メモリ通知用
struct S_SharedMemory{
  int16_t   *voiceBuffer;
  int16_t   *micBuffer;
  RingBufferCtrl *ring;
};
static S_SharedMemory  sm;

// オーディオの警告コールバック
static void audio_attention_cb(const ErrorAttentionParam *atprm)
{
    printf("Attention! code = %d\n", atprm->error_code);
}

// 初期化
void VoiceDetector::begin()
{
    // オーディオ初期化
    theAudio = AudioClass::getInstance();
    theAudio->begin(audio_attention_cb);
    theAudio->setRecorderMode(AS_SETRECDR_STS_INPUTDEVICE_MIC, 210); // gain +21.0dB (max)
    theAudio->initRecorder(AS_CODECTYPE_PCM, "/mnt/sd0/BIN", AS_SAMPLINGRATE_16000, AS_BITLENGTH_16, AS_CHANNEL_MONO);
    // theAudio->startRecorder();

    // サブコア起動
    int ret = MP.begin(SUBCORE_VD);
    if (ret < 0) {
        Serial.printf("VoiceDetector: MP.begin error = %d\n", ret);
    }
    // サブコアの起動完了待ち
    MP.RecvTimeout(MP_RECV_BLOCKING);
    int8_t msgid;
    uint32_t msgdata = 0;
    MP.Recv(&msgid, &msgdata, SUBCORE_VD);
    if (msgid != MSGID_BEGUN || msgdata != BEGIN_STEP1) {
        Serial.printf("VoiceDetector: MP.Recv error: no BEGUN message %d %lu\n", msgid, msgdata);
    }

    // メモリ確保
    size_t voiceBufferSize = SAMPLE_RATE * VOICE_BUFF_SEC * sizeof(int16_t);
    sm.voiceBuffer = (int16_t *)MP.AllocSharedMemory(voiceBufferSize);
    
    size_t micBufferSize = MIC_BUFF_FRAMES * SAMPLE_RATE * VAD_FRAME_MSEC / 1000 * sizeof(int16_t);
    sm.micBuffer = (int16_t *)MP.AllocSharedMemory(micBufferSize);
    
    sm.ring = (RingBufferCtrl*)MP.AllocSharedMemory(sizeof(RingBufferCtrl));
    sm.ring->frame_size = SAMPLE_RATE * VAD_FRAME_MSEC / 1000 * sizeof(int16_t);
    sm.ring->frame_total = MIC_BUFF_FRAMES;
    sm.ring->clear();

    MP.Send(MSGID_SHARE_MEMORY, &sm, SUBCORE_VD);

    // サブコアのVoiceDetectorの初期化完了待ち
    MP.Recv(&msgid, &msgdata, SUBCORE_VD);
    if (msgid != MSGID_BEGUN || msgdata != BEGIN_STEP2) {
        Serial.printf("VoiceDetector: MP.Recv error: no BEGUN message %d %lu\n", msgid, msgdata);
    }
    // 受信をポーリングに変更
    MP.RecvTimeout(MP_RECV_POLLING);

    state = VD_IDLE;
}

// メインループ処理
void VoiceDetector::loop()
{
    // メッセージ受信
    int8_t msgid;
    uint32_t msgdata;
    int ret = MP.Recv(&msgid, &msgdata, SUBCORE_VD);
    if(ret < 0) {
        if(ret != -EAGAIN){
            Serial.printf("VoiceDetector: MP.Recv error %d\n", ret);
            return;
        }
    }else{
        switch (ret) {
        case MSGID_ON_REGIST:
            if (onRegist) {
                onRegist(msgdata);
            }
            theAudio->stopRecorder();
            state = VD_IDLE;
            break;
        case MSGID_ON_DETECT:
            if (onDetect) {
                onDetect(msgdata);
            }
            theAudio->stopRecorder();
            state = VD_IDLE;
            break;
        case MSGID_ON_ERROR:
            if (onError) {
                onError(msgdata);
            }
            theAudio->stopRecorder();
            state = VD_IDLE;
            break;
        default:
            Serial.printf("VoiceDetector: unknown msgid %d\n", msgid);
            theAudio->stopRecorder();
            state = VD_IDLE;
            break;
        }
    }

    // マイクから読み出す
    if (theAudio->getRecordingSize() > sm.ring->frame_size){
        int index = sm.ring->w_index();
        int16_t *buff = &(sm.micBuffer[index]);
        uint32_t read_size = 0;

        int err = theAudio->readFrames((char*)(buff), sm.ring->frame_size, &read_size);

        if (err != AUDIOLIB_ECODE_OK && err != AUDIOLIB_ECODE_INSUFFICIENT_BUFFER_AREA) {
            printf("VoiceDetector: mic err = %d\n", err);
            sleep(1);
            theAudio->stopRecorder();
            return;
        }
        if(read_size != (uint32_t)sm.ring->frame_size){
            printf("VoiceDetector: mic few data (%lu)\n", read_size);
        }
        if(sm.ring->w_frame_inc() == false){
            printf("VoiceDetector: mic buffer overflow\n");
        }
    }
}

// 音声コマンド登録開始
// command_no : コマンド番号 (0,1,2,3,4)
void VoiceDetector::regist(int command_no)
{
    if(command_no < 0 || command_no > MAX_COMMAND){
        printf("VoiceDetector::regist() : Wrong command_no (%d)\n", command_no);
        return;
    }
    MP.Send(MSGID_REQ_REGIST, (uint32_t)command_no, SUBCORE_VD);

    theAudio->startRecorder();
    state = VD_REGIST0 + command_no;
}

// 音声コマンド検出開始
void VoiceDetector::detect()
{
    uint32_t dummy = 0;
    MP.Send(MSGID_REQ_DETECT, dummy, SUBCORE_VD);

    theAudio->startRecorder();
    state = VD_DETECT;
}

// 音声コマンド登録/検出のキャンセル
void VoiceDetector::cancel()
{
    uint32_t dummy = 0;
    MP.Send(MSGID_REQ_CANCEL, dummy, SUBCORE_VD);

    theAudio->stopRecorder();
    state = VD_IDLE;
}