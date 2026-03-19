
#include <Arduino.h>
#include <M5Core2.h>    // AXP(SPK_EN)のみ使用
#include <M5GFX.h>      // 画面描画
#include <driver/i2s.h> // I2S 直接制御
#include <SD.h>         // SD(SPI)
#include <SPI.h>
#include <memory>

// ====== Core2 内蔵スピーカ（I2Sアンプ: NS4168）配線 ======
#define I2S_PORT I2S_NUM_0
#define I2S_BCK 12
#define I2S_LRCK 0
#define I2S_DOUT 2

#ifndef AUDIO_SR
#define AUDIO_SR 44100
#endif
#ifndef AUDIO_BITS
#define AUDIO_BITS 16
#endif

// Core2 microSD（VSPI）
#define SD_CS 4
#define SD_SCLK 18
#define SD_MISO 19
#define SD_MOSI 23

// ====== 画面 ======
M5GFX display;

// ====== 再生状態（音主時間）======
volatile uint64_t g_samples_out_total = 0;
inline uint64_t audioTimeUS() { return (g_samples_out_total * 1000000ULL) / AUDIO_SR; }

// ====== 小ユーティリティ（LE） ======
uint32_t rdU32(File &f)
{
  uint8_t b[4];
  f.read(b, 4);
  return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}
uint16_t rdU16(File &f)
{
  uint8_t b[2];
  f.read(b, 2);
  return (uint16_t)b[0] | ((uint16_t)b[1] << 8);
}
void skipPad(File &f, uint32_t sz)
{
  if (sz & 1)
    f.read();
}
bool rd4(File &f, char out[5])
{
  if (f.read((uint8_t *)out, 4) != 4)
    return false;
  out[4] = 0;
  return true;
}

// ====== I2S 初期化 ======
void i2s_init_core2()
{
  i2s_driver_uninstall(I2S_PORT);

  i2s_config_t cfg = {};
  cfg.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX);
  cfg.sample_rate = AUDIO_SR;
  cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
  cfg.channel_format = I2S_CHANNEL_FMT_ONLY_RIGHT; // Core2内蔵SPKは片ch
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(4, 1, 0)
  cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
#else
  cfg.communication_format = I2S_COMM_FORMAT_I2S;
#endif
  cfg.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
  cfg.dma_buf_count = 8;
  cfg.dma_buf_len = 512;
  cfg.use_apll = true;           // APLLで精度向上
  cfg.tx_desc_auto_clear = true; // 欠損時のガチャ低減
  ESP_ERROR_CHECK(i2s_driver_install(I2S_PORT, &cfg, 0, NULL));

  i2s_pin_config_t pin = {};
  pin.bck_io_num = I2S_BCK;
  pin.ws_io_num = I2S_LRCK;
  pin.data_out_num = I2S_DOUT;
  pin.data_in_num = I2S_PIN_NO_CHANGE;
  ESP_ERROR_CHECK(i2s_set_pin(I2S_PORT, &pin));

  i2s_zero_dma_buffer(I2S_PORT); // 初回クリック抑制
}

// ====== SD 初期化（Arduino SD.h）======
bool sd_begin_arduino()
{
  SPI.begin(SD_SCLK, SD_MISO, SD_MOSI, SD_CS);
  return SD.begin(SD_CS, SPI, 25000000); // 必要なら 20MHz へ
}

// ====== 音声書き込み ======
void i2s_write_pcm(const void *data, size_t bytes)
{
  if (!bytes)
    return;
  size_t written = 0;
  ESP_ERROR_CHECK(i2s_write(I2S_PORT, data, bytes, &written, portMAX_DELAY));
  g_samples_out_total += written / (AUDIO_BITS / 8); // モノ16bit -> 2byte/サンプル
}

// ====== AVI パース用の最小構造体（必要項目のみ）======
struct AviMain
{
  uint32_t usec_per_frame = 0; // avih
  uint32_t width = 0, height = 0;
};

struct StreamInfo
{
  bool isVideo = false;
  bool isAudio = false;
  uint32_t fccHandler = 0; // 'MJPG' など（vids）
  // WAVEFORMATEX 相当（必要最小限）
  uint16_t wFormatTag = 1; // 1=PCM
  uint16_t nChannels = 1;
  uint32_t nSamplesPerSec = AUDIO_SR;
  uint16_t wBitsPerSample = AUDIO_BITS;
};

String fourccToStr(uint32_t f)
{
  char s[5];
  s[0] = f & 0xFF;
  s[1] = (f >> 8) & 0xFF;
  s[2] = (f >> 16) & 0xFF;
  s[3] = (f >> 24) & 0xFF;
  s[4] = 0;
  return String(s);
}

// ====== AVI ヘッダを読む（'RIFF' 'AVI ' / 'LIST' 'hdrl'）======
bool parse_hdrl(File &f, AviMain &avi, StreamInfo &v, StreamInfo &a, int &videoStreamIdx, int &audioStreamIdx)
{
  videoStreamIdx = audioStreamIdx = -1;
  // 'RIFF' + size + 'AVI '
  char id[5];
  if (!rd4(f, id) || String(id) != "RIFF")
    return false;
  uint32_t riffSize = rdU32(f);
  (void)riffSize;
  if (!rd4(f, id) || String(id) != "AVI ")
    return false;

  while (f.available())
  {
    if (!rd4(f, id))
      return false;
    if (String(id) == "LIST")
    {
      uint32_t listSize = rdU32(f);
      char listType[5];
      rd4(f, listType);
      if (String(listType) == "hdrl")
      {
        uint32_t listEnd = f.position() + (listSize - 4);
        int streamSeq = 0;
        while (f.position() < listEnd)
        {
          char cid[5];
          rd4(f, cid);
          uint32_t csz = rdU32(f);
          uint32_t chunkEnd = f.position() + csz;
          String scid = String(cid);

          if (scid == "avih")
          {
            avi.usec_per_frame = rdU32(f);
            (void)rdU32(f);
            (void)rdU32(f);
            (void)rdU32(f); // skip
            (void)rdU32(f); // total frames (unused)
            for (int i = 0; i < 4; i++)
              (void)rdU32(f);
            avi.width = rdU32(f);
            avi.height = rdU32(f);
            f.seek(chunkEnd);
          }
          else if (scid == "LIST")
          {
            char ltype[5];
            rd4(f, ltype);
            uint32_t subEnd = f.position() + (csz - 4);
            if (String(ltype) == "strl")
            {
              bool isVideo = false, isAudio = false;
              uint32_t fccHandler = 0;
              uint16_t wTag = 1, ch = 1, bps = 16;
              uint32_t sr = AUDIO_SR;

              while (f.position() < subEnd)
              {
                char sid4[5];
                rd4(f, sid4);
                uint32_t ssz = rdU32(f);
                uint32_t send = f.position() + ssz;
                String sid = String(sid4);
                if (sid == "strh")
                {
                  uint32_t fccType = rdU32(f);
                  fccHandler = rdU32(f);
                  if (String(fourccToStr(fccType)) == "vids")
                    isVideo = true;
                  if (String(fourccToStr(fccType)) == "auds")
                    isAudio = true;
                  f.seek(send);
                }
                else if (sid == "strf")
                {
                  if (isVideo)
                  {
                    uint32_t biSize = rdU32(f);
                    (void)biSize;
                    avi.width = rdU32(f);
                    avi.height = rdU32(f);
                    (void)rdU16(f);
                    uint16_t bitcount = rdU16(f);
                    (void)bitcount;
                    uint32_t compression = rdU32(f);
                    fccHandler = compression; // e.g. 'MJPG'
                    f.seek(send);
                  }
                  else if (isAudio)
                  {
                    wTag = rdU16(f);
                    ch = rdU16(f);
                    sr = rdU32(f);
                    (void)rdU32(f);
                    (void)rdU16(f);
                    bps = rdU16(f);
                    f.seek(send);
                  }
                  else
                  {
                    f.seek(send);
                  }
                }
                else
                {
                  f.seek(send);
                }
              }
              if (isVideo && videoStreamIdx < 0)
              {
                videoStreamIdx = streamSeq;
                v.isVideo = true;
                v.fccHandler = fccHandler;
              }
              else if (isAudio && audioStreamIdx < 0)
              {
                audioStreamIdx = streamSeq;
                a.isAudio = true;
                a.wFormatTag = wTag;
                a.nChannels = ch;
                a.nSamplesPerSec = sr;
                a.wBitsPerSample = bps;
              }
              streamSeq++;
            }
            else
            {
              f.seek(f.position() + (csz - 4));
            }
          }
          else
          {
            f.seek(chunkEnd);
          }
          skipPad(f, csz);
        }
        return true;
      }
      else
      {
        f.seek(f.position() + (listSize - 4));
      }
    }
    else
    {
      uint32_t csz = rdU32(f);
      f.seek(f.position() + csz);
      skipPad(f, csz);
    }
  }
  return false;
}

// ====== LIST 'movi' を探して再生（音主同期）======
void play_avi_movi(File &f, const AviMain &avi, const StreamInfo &v, const StreamInfo &a, int videoStreamIdx, int audioStreamIdx)
{
  char id[5];
  f.seek(0); // 先頭へ
  rd4(f, id);
  (void)rdU32(f);
  rd4(f, id); // 'RIFF' size 'AVI '

  uint32_t movi_pos = 0, movi_size = 0;
  while (f.available())
  {
    rd4(f, id);
    if (String(id) == "LIST")
    {
      uint32_t listSize = rdU32(f);
      char ltype[5];
      rd4(f, ltype);
      if (String(ltype) == "movi")
      {
        movi_pos = f.position();
        movi_size = listSize - 4;
        break;
      }
      else
      {
        f.seek(f.position() + (listSize - 4));
      }
    }
    else
    {
      uint32_t csz = rdU32(f);
      f.seek(f.position() + csz);
      skipPad(f, csz);
    }
  }
  if (!movi_pos)
  {
    Serial.println("movi not found");
    return;
  }

  display.fillScreen(TFT_BLACK);
  display.setTextColor(TFT_WHITE, TFT_BLACK);
  display.drawString("Playing (AVI MJPEG+PCM / Audio-master)", 4, 4);

  const double fps = (avi.usec_per_frame > 0) ? (1000000.0 / (double)avi.usec_per_frame) : 15.0;
  uint64_t frameIndex = 0;

  f.seek(movi_pos);
  const uint32_t movi_end = movi_pos + movi_size;
  std::unique_ptr<uint8_t[]> vbuf(new uint8_t[300 * 1024]);
  std::unique_ptr<uint8_t[]> abuf(new uint8_t[8 * 1024]);

  while (f.position() < movi_end)
  {
    char ckid[5];
    if (!rd4(f, ckid))
      break;
    uint32_t csz = rdU32(f);
    uint32_t next = f.position() + csz;

    int s0 = ckid[0] - '0', s1 = ckid[1] - '0';
    int sid = ((s0 >= 0 && s0 <= 9) ? s0 : 0) * 10 + ((s1 >= 0 && s1 <= 9) ? s1 : 0);
    bool isDC = (ckid[2] == 'd' && ckid[3] == 'c');
    bool isWB = (ckid[2] == 'w' && ckid[3] == 'b');

    if (sid == videoStreamIdx && isDC)
    {
      if (csz > 0 && csz <= 300 * 1024)
      {
        f.read(vbuf.get(), csz);
        uint64_t pts_us = (uint64_t)((double)frameIndex * (1000000.0 / fps));
        while (audioTimeUS() + 2000ULL < pts_us)
        {
          delayMicroseconds(500);
        }
        int x = (display.width() - (int)avi.width) / 2;
        int y = (display.height() - (int)avi.height) / 2;
        display.drawJpg(vbuf.get(), csz, x < 0 ? 0 : x, y < 0 ? 0 : y);
        frameIndex++;
      }
      else
      {
        f.seek(next);
      }
    }
    else if (sid == audioStreamIdx && isWB)
    {
      const size_t CHUNK = 2048;
      size_t remain = csz;
      while (remain)
      {
        size_t blk = (remain > CHUNK) ? CHUNK : remain;
        f.read(abuf.get(), blk);
        i2s_write_pcm(abuf.get(), blk);
        remain -= blk;
      }
    }
    else
    {
      f.seek(next);
    }
    skipPad(f, csz);
  }
}

void setup()
{
  Serial.begin(115200);
  delay(200);

  display.begin();
  display.setRotation(1);
  display.fillScreen(TFT_BLACK);
  display.setTextColor(TFT_WHITE, TFT_BLACK);
  display.setTextSize(2);
  display.drawString("AVI (MJPEG+PCM) - SD.h / no avilib", 6, 6);

  // AXP: SPK_EN
  M5.begin(true, true, true, true);
  M5.Axp.SetSpkEnable(true);

  // I2S
  i2s_init_core2();

  // SD
  if (!sd_begin_arduino())
  {
    display.drawString("SD.begin failed", 6, 30);
    return;
  }
  if (!SD.exists("/movie.avi"))
  {
    display.drawString("/movie.avi not found", 6, 30);
    return;
  }

  File f = SD.open("/movie.avi", FILE_READ);
  AviMain avi;
  StreamInfo v, a;
  int vid = -1, aud = -1;
  if (!parse_hdrl(f, avi, v, a, vid, aud))
  {
    display.drawString("hdrl parse failed", 6, 30);
    return;
  }

  play_avi_movi(f, avi, v, a, vid, aud);
  f.close();

  display.drawString("Done", 6, 52);
}

void loop() { delay(1000); }
