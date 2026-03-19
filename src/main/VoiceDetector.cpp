// 音声コマンド検出器

#include <Arduino.h>
#include <MP.h>
#include <Audio.h>
#include "VoiceDetector.h"

// オーディオ
AudioClass *theAudio;

// コアID
const int SUBCORE_VD  = 1;

// メッセージID定義
const int8_t MSGID_BEGUN        = 1;  // S->M 初期化完了通知
const int8_t MSGID_SHARE_MEMORY = 2;  // M->S 共有メモリ通知 
const int8_t MSGID_REQ_REGIST   = 3;  // M->S コマンド登録開始要求
const int8_t MSGID_REQ_DETECT   = 4;  // M->S コマンド検出開始要求
const int8_t MSGID_REQ_CANCEL   = 5;  // M->S コマンド登録/検出キャンセル
const int8_t MSGID_MIC_DATA     = 6;  // M->S マイク音声データ通知
const int8_t MSGID_ON_REGIST    = 7;  // S->M コマンド登録通知
const int8_t MSGID_ON_DETECT    = 8;  // S->M コマンド検出通知
const int8_t MSGID_ON_ERROR     = 9;  // S->M エラー通知

// 定数
const int SAMPLE_RATE    = 16000;   // サンプリング周波数 16kHz
const int VOICE_BUFF_SEC = 3;       // 音声コマンド登録用バッファ 3秒ぶん
const int VAD_FRAME_MSEC = 10;      // VADフレーム 10msec
const int MIC_BUFF_FRAMES = 3;      // マイクバッファのVADフレーム数 3フレーム

const size_t VOICE_BUFF_SIZE = SAMPLE_RATE * VOICE_BUFF_SEC * sizeof(int16_t);
const size_t VAD_BUFF_SIZE   = SAMPLE_RATE * VAD_FRAME_MSEC / 1000 * sizeof(int16_t);

// TODO メンバへ　インデックスは初期化、クリアも
int16_t   *voiceBuffer; // 音声コマンド登録用バッファ
int16_t   *micBuffer1;   // マイク入力用バッファ1 (マイクから)
int16_t   *micBuffer2;   // マイク入力用バッファ2 (サブコアへ)
uint32_t   frame_filled = 0;
uint32_t   frame_index = 0;

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
    voiceBuffer = (int16_t *)MP.AllocSharedMemory(VOICE_BUFF_SIZE);
    micBuffer1 = (int16_t *)malloc(VAD_BUFF_SIZE);
    micBuffer2 = (int16_t *)malloc(MIC_BUFF_FRAMES * VAD_BUFF_SIZE);
    MP.Send(MSGID_SHARE_MEMORY, voiceBuffer, SUBCORE_VD);

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

    if(state != VD_IDLE)
    {
        // マイクから読み出す
        uint32_t to_read = VAD_BUFF_SIZE - frame_filled;
        uint32_t read_size = 0;

        int err = theAudio->readFrames((char*)micBuffer1 + frame_filled, to_read, &read_size);

        if (err != AUDIOLIB_ECODE_OK && err != AUDIOLIB_ECODE_INSUFFICIENT_BUFFER_AREA) {
            printf("VoiceDetector: mic err = %d\n", err);
            sleep(1);
            theAudio->stopRecorder();
            frame_filled = frame_index = 0;
            return;
        }
        
        frame_filled += read_size;
        if(frame_filled >= VAD_BUFF_SIZE){
            frame_filled = 0;
            memcpy(&micBuffer2[frame_index * VAD_BUFF_SIZE], micBuffer1, VAD_BUFF_SIZE);
            MP.Send(MSGID_MIC_DATA, &micBuffer2[frame_index * VAD_BUFF_SIZE], SUBCORE_VD);
            frame_index++;
            if(frame_index >= MIC_BUFF_FRAMES) frame_index = 0;
        }
    }
}

// 音声コマンド登録開始
// command_no : コマンド番号 (0,1,2,3,4)
void VoiceDetector::regist(int command_no)
{
    if(command_no < 0 || command_no >= MAX_COMMAND){
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