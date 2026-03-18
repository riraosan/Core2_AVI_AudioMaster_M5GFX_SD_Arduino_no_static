
#include <Arduino.h>
#include <M5Core2.h>       // AXP192 (SPK_EN) のみ使用
#include <M5GFX.h>         // 描画は M5GFX を使用
#include <driver/i2s.h>    // I2S 直接制御
#include <SD.h>            // ★ Arduino SD ライブラリ

#include <avilib.h>      // lanyou1900/avilib（fopen を使用）

#include "avi_player_config.h"

// =====================================================================================
// 重要: 本ソースでは **static** キーワードを使用していません（変数/関数とも）。
// =====================================================================================

// ******************** I2S 設定（Core2 内蔵 SPK: BCK=12, LRCK=0, DATA=2） ********************
#define I2S_PORT   I2S_NUM_0
#define I2S_BCK    12
#define I2S_LRCK   0
#define I2S_DOUT   2

#ifndef AUDIO_SR
  #define AUDIO_SR 44100
#endif
#ifndef AUDIO_BITS
  #define AUDIO_BITS 16
#endif

// Core2 microSD 接続（VSPI）
#define SD_CS      4
#define SD_SCLK    18
#define SD_MISO    19
#define SD_MOSI    23

// 再生に"投入した"サンプル総数（音主時間の基準）
volatile uint64_t g_samples_out_total = 0;

// 描画
M5GFX display;

// ---- ユーティリティ ----
uint64_t audioTimeUS() {
  return (g_samples_out_total * 1000000ULL) / AUDIO_SR;
}

// ---- I2S 初期化 ----
void i2s_init_core2()
{
  i2s_driver_uninstall(I2S_PORT);

  i2s_config_t cfg = {};
  cfg.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX);
  cfg.sample_rate = AUDIO_SR;
  cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT; // 16bit
  cfg.channel_format = I2S_CHANNEL_FMT_ONLY_RIGHT; // 内蔵SPKは片ch
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(4,1,0)
  cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
#else
  cfg.communication_format = I2S_COMM_FORMAT_I2S;
#endif
  cfg.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
  cfg.dma_buf_count = 8;     // 調整可（6〜12）
  cfg.dma_buf_len   = 512;   // 調整可（256〜1024）
  cfg.use_apll = true;       // APLL でクロック精度を向上
  cfg.tx_desc_auto_clear = true; // 欠損時のガチャ音を低減

  ESP_ERROR_CHECK(i2s_driver_install(I2S_PORT, &cfg, 0, NULL));

  i2s_pin_config_t pin = {};
  pin.bck_io_num   = I2S_BCK;
  pin.ws_io_num    = I2S_LRCK;
  pin.data_out_num = I2S_DOUT;
  pin.data_in_num  = I2S_PIN_NO_CHANGE;
  ESP_ERROR_CHECK(i2s_set_pin(I2S_PORT, &pin));

  // 初回クリック対策: DMA を 0 でクリア
  i2s_zero_dma_buffer(I2S_PORT);
}

// ---- SD を Arduino SD.h で初期化 ----
bool sd_begin_arduino()
{
  // Core2 の VSPI ピンで SPI を開始
  SPI.begin(SD_SCLK, SD_MISO, SD_MOSI, SD_CS);

  // 25MHz で開始（ボード個体差に応じて下げる場合は 20MHz など）
  if (!SD.begin(SD_CS, SPI, 25000000)) {
    return false;
  }

  // ここで Arduino の SD ライブラリは FAT を /sdcard にマウントします。
  // 以降、fopen("/sdcard/xxx", "rb") で C ファイルAPIからもアクセス可能な環境になります（ESP32 Arduino 実装）。

  return true;
}

// ---- オーディオ書き込み ----
void i2s_write_pcm(const void* data, size_t bytes)
{
  size_t written = 0;
  if (bytes == 0) return;
  ESP_ERROR_CHECK(i2s_write(I2S_PORT, data, bytes, &written, portMAX_DELAY));
  g_samples_out_total += (written / (AUDIO_BITS/8)); // モノ 16bit → 2byte/サンプル
}

// ---- AVI 再生（音主同期） ----
void play_avi_audio_master(const char* path)
{
  FILE* fp = fopen(path, "rb");
  if (!fp) {
    Serial.printf("open failed: %s", path);
    return;
  }

  avi_t* avif = AVI_open_input_file(path, 1);
  if (!avif) {
    Serial.println("AVI_open_input_file failed");
    fclose(fp);
    return;
  }

  long vframes = AVI_video_frames(avif);
  long vw = AVI_video_width(avif);
  long vh = AVI_video_height(avif);
  double vfps = AVI_frame_rate(avif); // MJPEG 前提

  long arate = AVI_audio_rate(avif);
  int  abits = AVI_audio_bits(avif);
  int  ach   = AVI_audio_channels(avif);

  Serial.printf("AVI: %ldx%ld @ %.3f fps, audio %ld Hz, %d bit, ch=%d", vw, vh, vfps, arate, abits, ach);

  display.fillScreen(TFT_BLACK);

  // フレーム毎のワークバッファ
  size_t MAX_FRAME = 300*1024;  // 例: 300KB まで
  uint8_t* frame = (uint8_t*)heap_caps_malloc(MAX_FRAME, MALLOC_CAP_DEFAULT);
  if (!frame) {
    Serial.println("frame malloc failed");
    AVI_close(avif);
    fclose(fp);
    return;
  }

  // 音声読み出し用の小バッファ
  size_t AUD_CHUNK = 4*1024;
  uint8_t* abuf = (uint8_t*)heap_caps_malloc(AUD_CHUNK, MALLOC_CAP_DEFAULT);
  if (!abuf) {
    Serial.println("audio malloc failed");
    free(frame);
    AVI_close(avif);
    fclose(fp);
    return;
  }

  if (arate != AUDIO_SR || abits != AUDIO_BITS) {
    Serial.println("[WARN] AVI audio params differ from I2S setting. Expect artifacts.");
  }

  g_samples_out_total = 0;

  for (long i = 0; i < vframes; ++i) {
    long fsize = 0; int key = 0;
    int ret = AVI_read_frame(avif, (char*)frame, &key);
    if (ret == 0 || fsize <= 0) {
      Serial.printf("end or read_frame err at %ld", i);
      break;
    }

    // ---- 音主同期: このフレームの予定時刻 ----
    uint64_t pts_us = (uint64_t)((double)i * (1000000.0 / vfps));

    // 予定時刻まで、背景で音声を食べ進める
    while (audioTimeUS() + 2000ULL < pts_us) { // 2ms 先行まで許容
      long rd = AVI_read_audio(avif, (char*)abuf, AUD_CHUNK);
      if (rd <= 0) break; // 音声が尽きた
      i2s_write_pcm(abuf, rd);
    }

    // ---- 映像描画（MJPEG フレーム） ----
    display.drawJpg(frame, fsize, (display.width()-vw)/2, (display.height()-vh)/2);
  }

  // 残りの音声を掃き出し（任意）
  for (;;) {
    long rd = AVI_read_audio(avif, (char*)abuf, AUD_CHUNK);
    if (rd <= 0) break;
    i2s_write_pcm(abuf, rd);
  }

  free(abuf);
  free(frame);
  AVI_close(avif);
  fclose(fp);
}

void setup()
{
  Serial.begin(115200);
  delay(200);

  // 画面
  display.begin();
  display.setRotation(1);
  display.fillScreen(TFT_BLACK);
  display.setTextColor(TFT_WHITE, TFT_BLACK);
  display.setTextSize(2);
  display.drawString("Core2 AVI (Audio-master) SD.h", 10, 10);

  // AXP: SPK_EN 有効化（M5Unifiedは使わない）
  M5.begin(true, true, true, true);
  M5.Axp.SetSpkEnable(true);

  // I2S 初期化
  i2s_init_core2();

  // SD 初期化（Arduino SD）
  if (!sd_begin_arduino()) {
    display.drawString("SD.begin failed", 10, 40);
    return;
  }

  // 確認: Arduino SD 側での存在チェック（/movie.avi は SD のルート）
  if (!SD.exists("/movie.avi")) {
    display.drawString("/movie.avi not found", 10, 60);
    return;
  }

  display.drawString("Playing /sdcard/movie.avi", 10, 80);
  delay(300);

  // avilib は C の fopen を使うため、/sdcard の VFS マウントを前提にパスを与える
  play_avi_audio_master("/sdcard/movie.avi");

  display.drawString("Done", 10, 110);
}

void loop() {
  delay(1000);
}
