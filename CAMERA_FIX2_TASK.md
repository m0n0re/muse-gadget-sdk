# CAMERA_FIX2_TASK — 色の直し（U/V 入れ替わり）と、返信が届かない件

リポジトリ: ~/dev/muse-gadget-sdk（ブランチ cores3-custom、先頭 44b02e1）
対象ボード: muse-m5stack-cores3

## 実機で確認できたこと

- 撮影と届けは成功した: バッファは PSRAM、フレーム 153600 バイト、
  JPEG 25662 バイトが mini の受け口に届き、保存された。
- **色が全面的におかしい**: 輝度は正しく絵は読めるが、全体が
  マゼンタと緑の強い偽色になっている。YUV の U/V が入れ替わって
  いる典型的な出方。GC0308 の出力順（YUYV/UYVY/YVYU のどれか）と
  JPEG エンコーダへ渡している形式の想定が食い違っているはず。
- **完了の返信が届かなかった**: ログに
  `camera.look reply queued: ok=1 (free DMA 5623, internal 11055)`
  とあり、`sent invoke result` が出なかった。前回報告の推測どおり、
  結果キューの取り出し条件（noise_control.cpp の
  `noise_tx_has_small_frame_dma_headroom`、DMA 空き 6KiB）が
  撮影直後は満たせない。

## やること

1. 色: センサーの出力順とエンコーダ入力形式を突き合わせ、正しい
   色で JPEG 化する。センサー側のレジスタで順序を変えるか、
   エンコーダ側の入力形式指定を合わせるか、その版で正しい方法で。
   変換のためにフレーム全体をコピーし直す場合は PSRAM 上で行い、
   内部 RAM を圧迫しないこと。
2. 返信: 小さな invoke result（camera.look の返信のようなサイズ）
   が、DMA 空きが一時的に 6KiB を割っても最終的に届くようにする。
   先行の緩和（ウェイク系の 6KB 下限）と同じ考え方で、結果キュー
   の取り出し条件を小さな結果向けに緩めるか、空きが戻るまで
   遅らせて送る。無制限に待つ作りにはせず、他の送信（ping 等）を
   妨げないこと。変えた条件はログで分かるようにする。

## 制約

- ビルドは /tmp/build_cores3.sh、BUILD_EXIT=0 まで。パーティション表は
  partitions_muse_cores3.csv のまま（sdkconfig の CUSTOM_FILENAME と
  flash_args の 0x830000 srmodels.bin を確認すること）。
- 既存機能（ウェイク検知・TTS・サーボ・LED・sleep）の挙動は変えない。
- フラッシュしない。push しない。cores3-custom にコミットする。
- 確認範囲は camera.look の色と返信経路＋コンパイルまで。

## 報告フォーマット

1. 色の原因と直し方（どの形式同士の食い違いだったか）
2. 返信の直し方（変えた条件と、遅延の上限）
3. コミットハッシュ
4. ビルド結果（BUILD_EXIT）とパーティション表・flash_args の確認結果
5. 書き込み後に確認すべきログ行
