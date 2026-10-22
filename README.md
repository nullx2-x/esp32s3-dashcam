# XIAO ESP32S3 Sense ドライブレコーダー

Seeed XIAO ESP32S3 Sense (OV3660) + microSD をループ録画のドラレコにするファームウェア (ESP-IDF 5.5)。

## 機能

- 1280x720 MJPEG / 最大15fps を 60 秒ごとの AVI に分割して `/REC` へ録画
- 空き容量が下限 (既定 10%, 最低 256MB) を切ると古いファイルから削除。保護したファイルは `/LOCK` に移動し削除対象外
- 3 秒ごとにヘッダを更新・fsync するので、電源断でもそこまでの映像は再生可能
- Web UI (スマホ向け): ライブ映像、録画 ON/OFF、保護、スナップショット、ファイル一覧/再生/ダウンロード/削除、設定、OTA、SD フォーマット、ログ
- USB UVC カメラ (1280x720 MJPEG)。PC からは普通の Web カメラとして見える
- Wi-Fi: 自宅 Wi-Fi (STA) と本体 AP を同時に提供

## アクセス

| 経路 | URL |
| --- | --- |
| 同じ Wi-Fi | `http://dashcam.local/` (mDNS。IP が変わっても同じ名前) |
| 本体 AP `dashcam-XXXX` | `http://192.168.4.1/` (AP 内では任意のホスト名が本体に解決される) |

API: `/stream` (MJPEG), `/snapshot.jpg`, `/api/status`, `/api/settings`, `/api/files`, `/files/<dir>/<name>`, `/play/<dir>/<name>?speed=N` ほか。

## ビルド

ESP-IDF v5.4 以降 (v5.5.4 で確認) が必要。

```sh
. $IDF_PATH/export.sh
cp sdkconfig.defaults.local.example sdkconfig.defaults.local   # 編集する
idf.py build
```

`sdkconfig.defaults.local` (git 管理外) には Wi-Fi と AP パスワードの初期値を書く。起動後は Web UI から変更でき、NVS に保存される。
AP パスワードを空にすると AP は暗号化なしになるので、8 文字以上を設定すること。

`sdkconfig.defaults*` を変えたら `rm sdkconfig` してからビルドする。

## 書き込み

通常運用中は USB が UVC カメラになっているためシリアルが見えない。

- OTA (推奨): Web UI のメンテナンス欄から `build/esp32s3_dashcam.bin` をアップロード、または
  `curl -X POST --data-binary @build/esp32s3_dashcam.bin http://dashcam.local/api/ota`
- USB: Web UI の「USB書き込みモードで再起動」か、BOOT ボタン 3 秒長押しで USB-Serial/JTAG に切り替えてから
  `idf.py -p /dev/ttyACM0 flash`
  (`app-flash` は ota_0 にしか書かないので、OTA で ota_1 から起動している場合は反映されない)
- どちらも効かない場合: BOOT を押しながら RESET (またはUSB挿し直し) でダウンロードモード

## 制限・注意

- SD は FAT32 のみ (exFAT 非対応)。Web UI から本体でフォーマット可
- SD は SDMMC 1bit 40MHz で接続し、失敗時は SPI にフォールバック (約 12fps 上限)。
  SPI モードに入ったカードは電源を切るまで SD モードに戻らない
- UVC の解像度はビルド時固定 (1280x720)
- 音声・GPS・G センサーなし
- Web UI に認証なし。AP は WPA2 だが、自宅 LAN 内からは誰でも操作できる

## ライセンス

MIT License ([LICENSE](LICENSE))。

ビルド時に取得する依存コンポーネントはそれぞれのライセンスに従う:
[ESP-IDF](https://github.com/espressif/esp-idf), [esp32-camera](https://github.com/espressif/esp32-camera),
[mdns](https://components.espressif.com/components/espressif/mdns),
[usb_device_uvc](https://components.espressif.com/components/espressif/usb_device_uvc) (いずれも Apache-2.0)、TinyUSB (MIT)。
