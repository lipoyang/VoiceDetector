// AudioClassを使わないマイククラス
#include "DirectMic.h"

#include <Arduino.h>

#include <nuttx/config.h>
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <mqueue.h>
#include <sys/ioctl.h>
#include <poll.h>
#include <nuttx/audio/audio.h>

#define CHANNEL_NUM (1)       // チャンネル数
#define SAMPLE_RATE (48000)   // サンプリング周波数 (48kHz以外では動かない？？？)
#define BITWIDTH    (16)      // ビット深度

#define NUM_APB (8)           // バッファ段数
#define SZ_APB  (4800)        // バッファサイズ (2400サンプル×16bit / 48kHz = 50msec)

static struct ap_buffer_s apbs[NUM_APB];  // オーディオバッファ構造体
static uint8_t buff[NUM_APB][SZ_APB];     // オーディオバッファ用メモリ

static int micfd   = -1; // マイクのファイル記述子
static mqd_t micmq = -1; // マイク用メッセージキュー
static bool started = false;

/*****************************************************************************
 * 内部関数
 *****************************************************************************/

// オーディオバッファの初期化
static void init_apbs(void)
{
  for (int i = 0; i < NUM_APB; i++)
    {
      apbs[i].nmaxbytes = SZ_APB;   // バッファサイズ
      apbs[i].nbytes = 0;           // 書き込み位置のリセット？
      apbs[i].curbyte = 0;          // 読み出し位置のリセット？
      apbs[i].samp = &buff[i][0];   // メモリのアドレス
      nxmutex_init(&apbs[i].lock);  // ミューテックスの初期化
    }
}

// オーディオデバイスファイルを開く
// devpath : デバイスファイルのパス
static int open_devfile(FAR const char *devpath)
{
  return open(devpath, O_RDWR | O_CLOEXEC);
}

// ドライバからの通知を受けるメッセージキューを生成
// mqname : メッセージキューの名前
static mqd_t create_messageq(FAR const char *mqname)
{
  mqd_t mq;
  struct mq_attr attr;

  attr.mq_maxmsg = 12;
  attr.mq_msgsize = sizeof(struct audio_msg_s);
  attr.mq_curmsgs = 0;
  attr.mq_flags = 0;
  mq = mq_open(mqname, O_RDWR | O_CREAT, 0644, &attr);

  return mq;
}

// オーディオデバイスにメッセージキューを設定
// fd : オーディオデバイス
// mq : キュー
static int register_messageq(int fd, mqd_t mq)
{
  return ioctl(fd, AUDIOIOC_REGISTERMQ, (unsigned long)mq);
}

// オーディオの設定
// fd : オーディオデバイス
// type : 種別
// chnum : チャンネル数
// bps : ビットレート
static int configure(int fd, int type, int chnum, int fs, int bps)
{
  struct audio_caps_desc_s cap;

  cap.caps.ac_len = sizeof(struct audio_caps_s);
  cap.caps.ac_type = type;
  cap.caps.ac_channels = chnum;
  cap.caps.ac_chmap = 0;
  cap.caps.ac_controls.hw[0] = fs & 0xffff;
  cap.caps.ac_controls.b[2] = bps;
  cap.caps.ac_controls.b[3] = (fs >> 16) & 0xff;

  return ioctl(fd, AUDIOIOC_CONFIGURE, (unsigned long)(uintptr_t)&cap);
}

// ボリューム / マイクゲイン の設定
// fd : オーディオデバイス
// is_vol : true:ボリューム / false:マイクゲイン
// volgain : ボリューム または マイクゲイン
static int set_volgain(int fd, bool is_vol, int volgain)
{
  struct audio_caps_desc_s cap_desc;

  cap_desc.caps.ac_len            = sizeof(struct audio_caps_s);
  cap_desc.caps.ac_type           = AUDIO_TYPE_FEATURE;
  cap_desc.caps.ac_format.hw      = is_vol ? AUDIO_FU_VOLUME :
                                             AUDIO_FU_INP_GAIN;
  cap_desc.caps.ac_controls.hw[0] = volgain;

  return ioctl(fd, AUDIOIOC_CONFIGURE, (unsigned long)(uintptr_t)&cap_desc);
}

// オーディオバッファをデバイスに与える
// (マイクならデータ入力要求、スピーカーならデータ出力要求)
// fd : オーディオデバイス
// apb : オーディオバッファ構造体
static int enqueue_buffer(int fd, FAR struct ap_buffer_s *apb)
{
  struct audio_buf_desc_s desc;

  desc.numbytes = apb->nbytes;
  desc.u.buffer = apb;

  return ioctl(fd, AUDIOIOC_ENQUEUEBUFFER,
                  (unsigned long)(uintptr_t)&desc);
}

// DMA転送を開始
// fd : オーディオデバイス
static int start_dma(int fd)
{
  return ioctl(fd, AUDIOIOC_START, 0);
}

// DMA転送を停止
// fd : オーディオデバイス
static int stop_dma(int fd)
{
  return ioctl(fd, AUDIOIOC_STOP, 0);
}

// キューをクリア
// mq : キュー
static void cleanup_messageq(mqd_t mq)
{
  int qnum = 0;
  struct audio_msg_s msg;
  struct mq_attr attr;

  if (!mq_getattr(mq, &attr))
    {
      qnum = (int)attr.mq_curmsgs;
    }

  while (qnum--)
    {
      mq_receive(mq, (FAR char *)&msg, sizeof(msg), NULL);
    }
}

// メッセージキューを介したデバイスからのメッセージを処理
// mq : キュー
// fd : デバイス
// data : 取得したデータを返す (ダブルポインタ注意！)
// 戻り値 : 0以外ならエラー
static int handle_msg(mqd_t mq, int fd, int16_t **data)
{
  int ret = 0;
  ssize_t size;
  struct audio_msg_s msg;
  FAR struct ap_buffer_s *apb;

  // メッセージを受ける
  size = mq_receive(mq, (FAR char *)&msg, sizeof(msg), NULL);

  if (size == sizeof(msg)) // サイズチェック
  {
    switch (msg.msg_id)
    {
      case AUDIO_MSG_DEQUEUE:
        apb = (FAR struct ap_buffer_s *)msg.u.ptr;

        *data = (FAR int16_t *)apb->samp;
        // printf("0: %7d, %7d, %7d, %7d\n", data[0], data[1], data[2], data[3]);

        enqueue_buffer(fd, apb);
        break;

      case AUDIO_MSG_COMPLETE: // 完了
        ret = -1;
        break;
      case AUDIO_MSG_UNDERRUN: // アンダーラン
        ret = -2;
        break;
      case AUDIO_MSG_IOERROR: // 入出力エラー
        ret = -3;
        break;
    }
  }
  return ret;
}

/*****************************************************************************
 * 公開関数
 *****************************************************************************/

// 初期化
// 戻り値: 成否
bool DirectMic::begin()
{
    // オーディオバッファの初期化
    init_apbs();

    // デバイスを開く
    micfd = open_devfile("/dev/audio/pcm_in0");
    if (micfd < 0)
    {
        printf("Error to open /dev/audio/pcm_in0\n");
        return false;
    }

    // メッセージキューの生成
    micmq = create_messageq("/tmp/micmq");
    if (micmq < 0 )
    {
        printf("Error to create message queue\n");
        return false;
    }

    // メッセージキューをクリア
    cleanup_messageq(micmq);

    // メッセージキューをデバイスに設定
    register_messageq(micfd, micmq);

    // デバイスの設定
    configure(micfd, AUDIO_TYPE_INPUT,  CHANNEL_NUM, SAMPLE_RATE, BITWIDTH);

    // 音量を設定 (1000が最大)
    set_volgain(micfd, false, 1000);

    // 全てのバッファをデバイスに
    for (int i = 0; i < NUM_APB; i++)
    {
        enqueue_buffer(micfd, &apbs[i]);
    }
    return true;
}

// 開始
void DirectMic::start()
{
    if(!started){
        // メッセージキューをクリア
        cleanup_messageq(micmq);

        // 全てのバッファをデバイスに
        for (int i = 0; i < NUM_APB; i++)
        {
            enqueue_buffer(micfd, &apbs[i]);
        }

        // DMA転送開始
        start_dma(micfd);
        started = true;
    }
}

// 停止
void DirectMic::stop()
{
    if(started){
        // DMA転送を停止
        stop_dma(micfd);
        started = false;
    }
}

// 終了
void DirectMic::end()
{
    // デバイスとメッセージキューを解放
    if (micfd >= 0) close(micfd);
    if (micmq >= 0) mq_close(micmq);

    // メッセージキューを削除
    mq_unlink("/tmp/micmq");
}

// メインループ処理
void DirectMic::loop()
{
    if(started){
        // デバイスのポーリング (高優先度データ以外はノンブロッキング)
        struct pollfd fds[1];
        fds[0].fd     = micmq;
        fds[0].events = POLLIN;
        int ret = poll(fds, 1, -1);

        if (fds[0].revents & POLLIN)
        {
            // メッセージを処理
            int16_t *data = nullptr;
            ret = handle_msg(micmq, micfd, &data);
            if (ret == 0){
                if(onData != nullptr){
                    onData(data);
                }
            } else {
                if(onError != nullptr){
                    onError(ret);
                }
            }
        }
    }
}