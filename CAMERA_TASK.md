# CAMERA_TASK — カメラで「見て」: 撮影して画像を届ける命令を追加する

リポジトリ: ~/dev/muse-gadget-sdk（ブランチ cores3-custom、先頭 10ead65）
対象ボード: muse-m5stack-cores3

## ユーザーの希望

- StackChan のカメラで目の前を撮影し、その画像を椿（Muse）側で
  見られるようにしたい。声かけや命令で「見て」と言われたら撮影する
  使い方を想定。

## ハードウェア（M5Stack 公式 ESPHome 文書で確認済み）

- カメラは GC0308、解像度 320×240。
- ピン: VSYNC=GPIO46、HREF=GPIO38、XCLK=GPIO2（20MHz）、
  PCLK=GPIO45、データ=[GPIO39,40,41,42,15,16,48,47]。
  SCCB は共有 I2C バス（SCL=GPIO11 / SDA=GPIO12）。
- 電源と IO 拡張の初期化が先に済んでいること（setup_priority の
  注意あり）。フレームバッファは PSRAM へ。

## やること

1. **既存の仕組みの調査**: この SDK にカメラ撮影や画像転送の既存
   経路（VM やアプリへ画像を送る仕組み、muse_video_wifi.c 等）が
   あるか、CoreS3 のボード定義にカメラが含まれているかを先に
   確認する。
2. **命令 `camera.look`** を追加する（voice.speak と同じ流儀）:
   - 受けたら撮影し、JPEG を下記の届け先へ送り、届け終わったら
     完了を返す。会話中・発話中・他の撮影中は busy。
   - 撮影中は thinking 相当の表示・LED 連動にする。
3. **届け先の優先順位**:
   - (a) SDK 既存の画像経路が実在し、VM/アプリ側で受け取れるなら
     それを使う。
   - (b) なければ TTS と同じ形: 設定で持つ HTTP エンドポイントへ
     JPEG を POST する。Kconfig の作りは CONFIG_MUSE_TTS_ENDPOINT
     の前例に合わせ、新しい設定（例: CONFIG_MUSE_CAMERA_ENDPOINT、
     既定は http://192.168.24.52:50023/capture）にする。応答で
     ファイル名を受け取り、命令の返信 JSON にそのファイル名を
     入れる（椿側が mini から取得する）。
   - (c) トンネル経由の base64 は、送信ゲート（DMA ヘッドルーム）の
     前例があるため最終手段とし、実測サイズで判断する。
4. **メモリ**: フレームバッファは PSRAM。内部 RAM が逼迫している
   経緯（ウェイクワード実装時）を踏まえ、起動後の空きと最大連続
   ブロックをログで示し、既存機能（ウェイク検知・TTS・サーボ）を
   壊さないこと。

## 制約

- ビルドは /tmp/build_cores3.sh、BUILD_EXIT=0 まで。パーティション表は
  partitions_muse_cores3.csv のまま（sdkconfig の CUSTOM_FILENAME と
  flash_args の 0x830000 srmodels.bin を確認すること）。
- 既存の命令・連動の仕様は変えない。
- フラッシュしない。push しない。cores3-custom にコミットする。
- 確認範囲は camera.look の追加と撮影・転送経路＋コンパイルまで。

## 報告フォーマット

1. 調査結果（既存の画像経路の有無、ボード定義のカメラ対応）
2. 採用した届け先とその理由、命令の仕様（busy 条件、返信 JSON）
3. メモリへの影響（起動後の空きの前後比較）
4. コミットハッシュ
5. ビルド結果（BUILD_EXIT）とパーティション表・flash_args の確認結果
6. 書き込み後に確認すべきログ行
