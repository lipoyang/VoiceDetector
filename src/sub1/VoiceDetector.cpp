/*
 * 音声コマンド検出器
 *
 * AIｽﾀｯｸﾁｬﾝ の WakeWord.cpp から移植しました。
 * by Bizan Nishimura
 */

#include <Arduino.h>
#include <MP.h>
#include <algorithm> // copy_n
#include <memory>
#include <stdio.h>
#include <sys/stat.h>
#include "simplevox.h"
#include "NoiseSuppressor.h"
#include "VoiceDetector.h"

#define base_path "/mnt/sd0"
#define file_name "/wakeword.bin"

int frameNo; // デバッグ用

const int MIC_BUFF_FRAMES = 3;      // マイクバッファのVADフレーム数 3フレーム
constexpr int kSampleRate = 16000;
constexpr int audioLength = kSampleRate * 3;  // 3 seconds
// constexpr int kRxBufferNum = 3;

int16_t* rawAudio;
//int16_t* rxBuffer;
int16_t* rawBuffer;
NoiseSuppressor nsInst;
simplevox::VadEngine vadEngine;
simplevox::MfccEngine mfccEngine;
simplevox::MfccFeature* mfcc = nullptr;

/**
 * VAD -> Buffer -> MFCCの流れで逐次的にMFCCの算出を行う。
 * この例では参考のためにREGISTではraw dataを残しているが、
 * COMPAREの処理だけにすればraw dataは必要なくなるためメモリサイズの削減が可能となる。
 * 処理としてHangbeforeおよびPreDetection時間のデータ処理が複雑な点と
 * frame_lengthがVADとMFCCで異なり、１つの処理単位がhop_lengthである点など
 * 構造を理解していないと分からない処理が多いのが難点…。
 */

/**
 * @brief 除算を行い演算結果を切り上げます（正の整数）
 * @param[in] dividend  被除数
 * @param[in]  divisor   除数
 * @return 切り上げた整数(3/2 -> 2)
 */
constexpr int divCeil(int dividend, int divisor)
{
  return (dividend + divisor - 1) / divisor;
}

/**
 * @brief １フレームの録音を行い読み取り可能なバッファを返します
 * @return  読み取り可能な１フレーム分のバッファ (無いときは nullptr を返す)
 */
int16_t* VoiceDetector::rxMic()
{
  // const int frameLength = vadEngine.config().frame_length();

  // メインコアで録音したデータを取得 (by Bizan Nishimura)
  if(micQueue.empty()){
    return nullptr;
  }else{
    int16_t *data = micQueue.front();
    micQueue.pop();
    return data;
  }
}

int rawBufferMaxSize;
int rawBufferSize;
void raw_init(int length)
{
  rawBufferMaxSize = length;
  rawBufferSize = 0;
  rawBuffer = (int16_t*)calloc(rawBufferMaxSize, sizeof(*rawBuffer));
}

int raw_size() { return rawBufferSize; }
void raw_reset() { rawBufferSize = 0; }
int16_t* raw_front() { return rawBuffer; }

void raw_pushBack(const int16_t* src, int length)
{
  if (rawBufferSize + length <= rawBufferMaxSize)
  {
    std::copy_n(src, length, &rawBuffer[rawBufferSize]);
    rawBufferSize += length;
  }
}

void raw_popFront(int length)
{
  std::copy_n(&rawBuffer[length], rawBufferSize - length, rawBuffer);
  rawBufferSize -= length;
}

float* features;
int mfccFrameNum;
int mfccCoefNum;
int mfccBeforeFrameNum;
void feature_init(const simplevox::MfccConfig& mfccConfig, const simplevox::VadConfig& vadConfig, int maxTimeMs)
{
  const int maxLength = maxTimeMs * mfccConfig.sample_rate / 1000;
  mfccFrameNum = (maxLength - (mfccConfig.frame_length() - mfccConfig.hop_length())) / mfccConfig.hop_length();
  mfccCoefNum = mfccConfig.coef_num;
  features = (float*)malloc(sizeof(*features) * mfccFrameNum * mfccCoefNum);

  // VADのHangbeforeおよびPreDetectionに相当するデータサイズを算出
  const int vadBeforeLength = 
            vadConfig.frame_length() *
            ( divCeil(vadConfig.before_length(), vadConfig.frame_length())
            + divCeil(vadConfig.decision_length(), vadConfig.frame_length()));
  // VADのBefore区間に相当するフレーム数を算出
  mfccBeforeFrameNum = (vadBeforeLength - (mfccConfig.frame_length() - mfccConfig.hop_length())) / mfccConfig.hop_length();
}

float* feature_get(int number) { return &features[number * mfccCoefNum]; }

// 初期化
// voiceBuffer : 音声バッファ用メモリ
void VoiceDetector::begin(int16_t *voiceBuffer)
{
  auto vadConfig = vadEngine.config();
  vadConfig.sample_rate = kSampleRate;
  vadConfig.decision_time_ms = 50; // 50msec by B.Nishimura
  auto mfccConfig = mfccEngine.config();
  mfccConfig.sample_rate = kSampleRate;

#if 0
  rawAudio = (int16_t*)malloc(audioLength * sizeof(*rawAudio));
  rxBuffer = (int16_t*)malloc(kRxBufferNum * vadConfig.frame_length() * sizeof(*rxBuffer));
#else
  // メインコアで確保した共有メモリを使用 (by Bizan Nishimura)
  rawAudio = voiceBuffer;
#endif  
  raw_init(mfccConfig.frame_length() + vadConfig.frame_length());
  feature_init(mfccConfig, vadConfig, 3000);

  nsInst.init(vadConfig.frame_time_ms, 1, vadConfig.sample_rate);
  if (!vadEngine.init(vadConfig))
  {
    puts("Failed to initialize vad.");
    while(true) delay(10);
  }
  if (!mfccEngine.init(mfccConfig))
  {
    puts("Failed to initialize mfcc.");
    while(true) delay(10);
  }
  
  struct stat st;

  if (stat(base_path file_name, &st) == 0) {
    if ((st.st_mode & S_IFMT) == S_IFREG) {
      puts("wakeword.bin exists.");
      mfcc = mfccEngine.loadFile(base_path file_name);
    }else{
      puts("wakeword.bin is not a normal file!");
    }
  }else{
    puts("wakeword.bin not found.");
  }
}

// 音声コマンド登録処理
// 戻り値 : true:完了, false:未完了
bool VoiceDetector::regist()
{
  bool ret =false;
  auto* data = rxMic();
  if (data == nullptr) { return ret; }

  nsInst.process(data, data);

  int length = vadEngine.detect(rawAudio, audioLength, data);
  if (length <= 0) { return ret; }
#if defined(WAV_FILE_DEBUG) || defined(MIC_DEBUG)
  float t_end = (float)frameNo * 0.01f;
  float t_len = (float)length * 0.01f / 160.0f;
  float t_begin = t_end - t_len;
  printf("Detected! %.2f - %.2f sec (len = %.2f sec)\n", t_begin, t_end, t_len);
#endif

  if (mfcc != nullptr){ delete mfcc; }
  mfcc = mfccEngine.create(rawAudio, length);
  if (mfcc)
  {
    mfccEngine.saveFile(base_path file_name, *mfcc);
    ret = true;
  }
  vadEngine.reset();
  return ret;
}

// 音声コマンド検出処理
// 戻り値 : -1:未検出, 0-4:コマンド検出
int VoiceDetector::detect()
{
  bool ret = false;
  auto* data = rxMic();
  if (data == nullptr) {
    return -1; 
  }
  nsInst.process(data, data);

  if (mfcc == nullptr) {
     return -1; 
  }
  static int mfccFrameCount = 0;
  const auto state = vadEngine.process(data);
  const int vadFrameLength = vadEngine.config().frame_length();
  const int mfccFrameLength = mfccEngine.config().frame_length();
  const int mfccHopLength = mfccEngine.config().hop_length();
  // Silence以上ならrawBufferにデータを追加
  if (state >= simplevox::VadState::Silence)
  {
    raw_pushBack(data, vadFrameLength);
  }
  // rawBufferにMFCCのframe_length()以上のデータがある場合はMFCCを算出
  while (raw_size() >= mfccFrameLength && mfccFrameCount < mfccFrameNum)
  {
    // feature_pushBack
    mfccEngine.calculate(raw_front(), feature_get(mfccFrameCount));
    mfccFrameCount++;
    raw_popFront(mfccHopLength);  // hop_length分シフト
  }
  // Speech以前(Silence, PreDetection)の場合はmfccBeforeFrameNumを超えた分は取り除く(シフトする)
  if (state < simplevox::VadState::Speech && mfccFrameCount > mfccBeforeFrameNum)
  {
    const int shiftCount = mfccFrameCount - mfccBeforeFrameNum;
    const int shiftLength = shiftCount * mfccCoefNum;
    std::copy_n(&features[shiftLength], mfccFrameCount * mfccCoefNum - shiftLength, features);
    mfccFrameCount -= shiftCount;
  }
  // 検出完了もしくは最大フレームに到達した場合は判定終了
  if (state == simplevox::VadState::Detected
      || (state >= simplevox::VadState::Speech && mfccFrameNum <= mfccFrameCount))
  {
#if defined(WAV_FILE_DEBUG) || defined(MIC_DEBUG)
      float t_end = (float)frameNo * 0.01f;
      printf("Comparing... %.2f sec\n", t_end);
#endif
    std::unique_ptr<simplevox::MfccFeature> feature(mfccEngine.create(features, mfccFrameCount, mfccCoefNum));
    const auto dist = simplevox::calcDTW(*mfcc, *feature);

    char pass = (dist < 180) ? '!': '?';  // 180未満で一致と判定, しきい値は要調整
    printf("Dist: %6lu, %c\n", dist, pass);
    raw_reset();
    mfccFrameCount = 0;
    vadEngine.reset();
    if(dist < 180) ret = true;
  }    
  return (ret ? 0 : -1); // TODO
}

// マイク音声データを追加
// data : マイク音声データ
void VoiceDetector::putMicData(int16_t *data)
{
  if( micQueue.size() >= MIC_BUFF_FRAMES){
    MPLog("Buffer over flow!\n");
    return;
  }
  micQueue.push(data);
}
