# CAMERA_FIX_TASK — DVP の DMA バッファを PSRAM に置いて撮影を通す

リポジトリ: ~/dev/muse-gadget-sdk（ブランチ cores3-custom、先頭 8040158）
対象ボード: muse-m5stack-cores3

## 実機ログで分かったこと

- センサーは検出できている: `gc0308: Detected Camera sensor PID=0x9b`
  （SCCB・XCLK=GPIO2・電源は合っている）。
- 失敗はここ:
  - `cores3_cam: format 320x240, 0 bytes/frame`
  - `dvp_ext: esp_cam_new_dvp_ctlr_ext(864): no mem for CAM DVP DMA receive buffer`
  - `esp_video: video->ops->start=101` → `STREAMON failed` →
    `look: capture failed: ESP_FAIL`
- 撮影時点の内部 RAM は free 11327 / largest 4864 しかない（ウェイク
  ワード常駐のため）。フレーム規模の DMA バッファを内部 RAM からは
  確保できない。**受けバッファを PSRAM に置くのが本筋**。
- ついでに `0 bytes/frame` と出ている件: 交渉されたピクセル形式が
  想定どおりか（サイズ計算が 0 にならないか）も確認すること。
- なお、失敗の返信は 220ms ほどで確定しているのに、呼び出し側は
  20 秒でタイムアウトした。エラー返信が届いていない可能性がある
  ので、返信経路もあわせて見ること（成功時は届くのか、エラー時の
  形だけの問題か）。

## やること

1. esp_video / esp_cam の DVP 経路で、受けバッファを PSRAM に
   置く方法を調べて適用する（バッファの caps 指定、Kconfig、
   外部確保した PSRAM バッファを渡す方式など、その版で正しい
   やり方で）。ESP32-S3 の CAM DMA が PSRAM へ書ける前提の設計が
   公式例にもあるはず。
2. ピクセル形式と 1 フレームのバイト数を確定させ、ログに出す。
3. 撮影→POST→返信までを通し、撮影後の空きが元に戻ることも
   ログで確認できるようにする（解放漏れの検出）。

## 制約

- ビルドは /tmp/build_cores3.sh、BUILD_EXIT=0 まで。パーティション表は
  partitions_muse_cores3.csv のまま（sdkconfig の CUSTOM_FILENAME と
  flash_args の 0x830000 srmodels.bin を確認すること）。
- ウェイク検知・TTS・サーボ・LED など既存機能の挙動は変えない。
  撮影のためにウェイクエンジンを止めるような手段は採らない。
- フラッシュしない。push しない。cores3-custom にコミットする。
- 確認範囲は camera.look の撮影経路＋コンパイルまで。

## 報告フォーマット

1. バッファを PSRAM に置くためにした変更（根拠も）
2. 確定したピクセル形式とフレームサイズ
3. エラー返信の経路について分かったこと（あれば）
4. コミットハッシュ
5. ビルド結果（BUILD_EXIT）とパーティション表・flash_args の確認結果
6. 書き込み後に確認すべきログ行
