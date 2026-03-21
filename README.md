
# Core2_AVI

**SDドライバを Arduino の `SD.h` に置き換え**た版です。その他の構成は、

- **M5Unified不使用**（AXP/SPK_ENに M5Core2 ライブラリのみ）
- **M5GFX** による MJPEG フレーム描画
- **I2S（driver/i2s）直叩き**, **DMAゼロクリア**, **tx_desc_auto_clear**
- **音主（Audio‑master）同期**

となっています。

## 使い方

1. microSD を FAT32 で用意し、ルートに `output.avi`（**MJPEG + PCM(16‑bit/48.0k/モノ)**）を配置
2. PlatformIO → Upload
3. 起動後、`SD.begin()` でマウントされ、`/output.avi` を読み込みます。

## AVIファイル作成方法

```
ffmpeg -y -i input.mp4 -ac 1 -ar 48000 -c:a pcm_s16le -c:v mjpeg -q:v 7 -vf "fps=15,scale=-1:240:flags=lanczos,crop=320:240:(in_w-320)/2:0" output.avi
```

## ピン/初期化

- SD: VSPI (`SCLK=18, MISO=19, MOSI=23, CS=4`) → `SPI.begin(...)` → `SD.begin(4, SPI, 25MHz)`
- I2S: Core2 内蔵スピーカ（NS4168）向け `BCK=12, LRCK=0, DOUT=2`

## 注意

- もしボード/SDカードの相性で 80MHz が不安定な場合は、`SD.begin(SD_CS, SPI, 40000000)` に下げてください。
