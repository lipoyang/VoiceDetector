// 音声コマンド検出器

#include <Arduino.h>
#include <MP.h>
#include "VoiceDetector.h"
#include "DirectMic.h"

// 音声コマンドMFCCデータファイルのパス (書式付き)
#define MFCC_FILE_PATH "/mnt/sd0/voice%ld.bin"

// オーディオ
static DirectMic mic;

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
const int8_t MSGID_REQ_LOAD     = 9;  // M->S MFCCデータのロード要求
const int8_t MSGID_RES_LOAD     = 10; // S->M MFCCデータのロード応答 

// 定数
const int MIC_DATA_FRAMES  = 5;     // マイク入力データのVADフレーム数換算 (50msec / 10msec)
const int MIC_BUFF_FRAMES  = 40;    // マイクバッファのVADフレーム数
const int VAD_BUFF_SIZE = 160;      // VADのフレームサイズ (16kHzサンプル × 10msec)  
const size_t MFCC_FILE_SIZE_MAX = 4096;

int16_t    micBuffer[MIC_BUFF_FRAMES][VAD_BUFF_SIZE];  // マイク入力用バッファ2 (サブコアへ)]
uint8_t   *fileBuffer;  // MFCCファイルバッファ 
uint32_t   frame_filled = 0;
uint32_t   frame_index = 0;

// マイクのエラーハンドラ
static void onMicError(int err)
{
    printf("Mic Error code = %d\n", err);
}

// マイクのデータハンドラ
static void onMicData(int16_t* data)
{
    // 48kHzサンプル 40msec ぶんのデータを 10msecごとに 16kHzサンプルにダウンサンプル
    // ( 1920サンプル → 4 * 160サンプル)
    for(int i = 0; i < MIC_DATA_FRAMES; i++){
        for(int j = 0; j < VAD_BUFF_SIZE; j++){
            micBuffer[frame_index][j] = *data;
            data += 3;
        }
        MP.Send(MSGID_MIC_DATA, micBuffer[frame_index], SUBCORE_VD);
        frame_index++;
        if(frame_index >= MIC_BUFF_FRAMES) frame_index = 0;
    }
}

// 初期化
void VoiceDetector::begin()
{
    // マイク初期化
    mic.onError = onMicError;
    mic.onData = onMicData;
    mic.begin();

    // サブコア起動
    int ret = MP.begin(SUBCORE_VD);
    if (ret < 0) {
        printf("VoiceDetector: MP.begin error = %d\n", ret);
    }
    // サブコアの起動完了待ち
    MP.RecvTimeout(MP_RECV_BLOCKING);
    int8_t msgid;
    uint32_t dummy = 0;
    MP.Recv(&msgid, &dummy, SUBCORE_VD);
    if (msgid != MSGID_BEGUN) {
        printf("VoiceDetector: MP.Recv error: no BEGUN message (%d)\n", msgid);
    }

    // メモリ確保
    fileBuffer = (uint8_t*)malloc(MFCC_FILE_SIZE_MAX);
    MP.Send(MSGID_SHARE_MEMORY, fileBuffer, SUBCORE_VD);

    // 音声コマンドのMFCCデータのロード
    for(uint32_t mfcc_no = MFCC_0; mfcc_no <= MFCC_4; mfcc_no++){
        ret = loadFile(mfcc_no); 
        if(ret != 0){
            if(ret != -1){
                printf("MFCC [%ld] load failed (%d)\n", mfcc_no, ret);
            }
        }else{
            // ロード要求
            printf("MFCC [%ld] loading...\n", mfcc_no);
            MP.Send(MSGID_REQ_LOAD, mfcc_no, SUBCORE_VD);
            // ロード応答待ち
            uint32_t msgdata = RESULT_ERROR;
            MP.Recv(&msgid, &msgdata, SUBCORE_VD);
            if (msgid != MSGID_RES_LOAD || msgdata != mfcc_no) {
                printf("VoiceDetector: MP.Recv error: MFCC load (%d %lu)\n", msgid, msgdata);
            }
        }
    }
    //printf("No more MFCC files\n");
    
    // MFCCロード終了要求
    uint32_t mfcc_no = MFCC_END;
    MP.Send(MSGID_REQ_LOAD, mfcc_no, SUBCORE_VD);
    // 応答待ち
    uint32_t msgdata = RESULT_ERROR;
    MP.Recv(&msgid, &msgdata, SUBCORE_VD);
    if (msgid != MSGID_RES_LOAD || msgdata != MFCC_END) {
        printf("VoiceDetector: MP.Recv error: MFCC final (%d %lu)\n", msgid, msgdata);
    }
    //printf("MFCC initialized\n");

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
            printf("VoiceDetector: MP.Recv error %d\n", ret);
            return;
        }
    }else{
        switch (ret) {
        case MSGID_ON_REGIST:
            mic.stop();
            if(msgdata <= MFCC_4){
                int ret = saveFile(msgdata);
                if(ret != 0) msgdata = RESULT_ERROR;
            }
            if (onRegist) {
                onRegist(msgdata);
            }
            state = VD_IDLE;
            break;
        case MSGID_ON_DETECT:
            mic.stop();
            if (onDetect) {
                onDetect(msgdata);
            }
            state = VD_IDLE;
            break;
        default:
            printf("VoiceDetector: unknown msgid %d\n", msgid);
            mic.stop();
            state = VD_IDLE;
            break;
        }
    }

    if(state != VD_IDLE)
    {
        mic.loop();
    }
}

// 音声コマンド登録開始
// command_no : コマンド番号 (0,1,2,3,4)
void VoiceDetector::regist(uint32_t command_no)
{
    if(command_no >= MAX_COMMAND){
        printf("VoiceDetector::regist() : Wrong command_no (%ld)\n", command_no);
        return;
    }
    MP.Send(MSGID_REQ_REGIST, command_no, SUBCORE_VD);

    frame_filled = 0;
    frame_index = 0;
    mic.start();
    state = VD_REGIST0 + command_no;
}

// 音声コマンド検出開始
void VoiceDetector::detect()
{
    uint32_t dummy = 0;
    MP.Send(MSGID_REQ_DETECT, dummy, SUBCORE_VD);

    frame_filled = 0;
    frame_index = 0;
    mic.start();
    state = VD_DETECT;
}

// 音声コマンド登録/検出のキャンセル
void VoiceDetector::cancel()
{
    uint32_t dummy = 0;
    MP.Send(MSGID_REQ_CANCEL, dummy, SUBCORE_VD);

    mic.start();
    state = VD_IDLE;
}

// 音声コマンドのMFCCデータファイルをロードする。
// command_no : コマンド番号 (0,1,2,3,4)
// 戻り値 : 0:成功 / 0以外:失敗
int VoiceDetector::loadFile(uint32_t command_no)
{
    char path[32];
    sprintf(path, MFCC_FILE_PATH, command_no);

    auto* file = fopen(path, "rb");
    if (file == NULL) {
        // printf("%s not found\n", path);
        return -1;
    }

    uint8_t tag;
    if (fread(&tag, sizeof(tag), 1, file) != 1)
    {
        fclose(file); return -2;
    }
    fileBuffer[0] = tag;

    int32_t size, coef_num;
    if (fread(&size, sizeof(size), 1, file) != 1)
    {
        fclose(file); return -3;
    }
    *((int32_t*)&fileBuffer[1]) = size;

    if (fread(&coef_num, sizeof(coef_num), 1, file) != 1)
    {
        fclose(file); return -4;
    }
    *((int32_t*)&fileBuffer[5]) = coef_num;

    const auto data_byte = sizeof(int16_t);
    const unsigned int data_num = size * coef_num;
    if ((data_byte * data_num) > (MFCC_FILE_SIZE_MAX - (1 + 4 + 4))){
        fclose(file); return -5;
    }
    if (fread(&fileBuffer[9], data_byte, data_num, file) != data_num)
    {
        fclose(file); return -6;
    }

    fclose(file);
    return 0;
}

// 音声コマンドのMFCCデータファイルをセーブする。
// command_no : コマンド番号 (0,1,2,3,4)
// 戻り値 : 0:成功 / 0以外:失敗
int VoiceDetector::saveFile(uint32_t command_no)
{
    char path[32];
    sprintf(path, MFCC_FILE_PATH, command_no);

    auto* file = fopen(path, "wb");
    if (file == NULL) {
        printf("failed to open %s\n", path);
        return -1;
    }

    const uint8_t tag = fileBuffer[0];
    if (fwrite(&tag, sizeof(tag), 1, file) != 1)
    {
        fclose(file); return -2;
    }

    const int32_t size = *((int32_t*)&fileBuffer[1]);
    if (fwrite(&size, sizeof(size), 1, file) != 1)
    {
        fclose(file); return -3;
    }
    const int32_t coef_num = *((int32_t*)&fileBuffer[5]);
    if (fwrite(&coef_num, sizeof(coef_num), 1, file) != 1)
    {
        fclose(file); return -4;
    }

    const auto data_byte = sizeof(int16_t);
    const unsigned int data_num = size * coef_num;
    if ((data_byte * data_num) > (MFCC_FILE_SIZE_MAX - (1 + 4 + 4))){
        fclose(file); return -5;
    }
    if (fwrite(&fileBuffer[9], data_byte, data_num, file) != data_num)
    {
        fclose(file); return -6;
    }

    fclose(file);
    return 0;
}
