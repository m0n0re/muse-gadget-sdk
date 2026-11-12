# CAMERA_DIAG_TASK — 生フレームを送る診断命令を追加する

リポジトリ: ~/dev/muse-gadget-sdk（ブランチ cores3-custom、先頭 152cf42）
対象ボード: muse-m5stack-cores3

## 背景

- camera.look の撮影・送信・返信は通った。ただし色がまだ全面的に
  おかしい。U/V を入れ替える直し（152cf42）を入れたら、偽色の
  出方が変わっただけで直らなかった。つまり色成分の並びの想定が
  まだ合っていない。
- 推測を重ねず、センサーからの生データをこちらで解析して並びを
  特定したい。そのための診断命令を追加する。

## やること

- 命令 `camera.raw` を追加する（camera.look と同じ busy 条件・
  表示連動でよい）:
  - 撮影し、**一切変換していない生フレーム**（153600 バイトのはず）
    を HTTP POST で送る。U/V 入れ替えもしない。
  - 送り先は、設定済みエンドポイントと同じサーバーの `/raw`
    （例: http://192.168.24.52:50023/raw）。Content-Type は
    application/octet-stream。
  - 返信は camera.look と同形式（ok と file と bytes）。
    width / height / そのとき申告されている pixelformat の fourcc
    も payload に入れる。
  - ログに、送ったバイト数と先頭 16 バイトのダンプを出す。
- camera.look 本体の挙動は変えない（U/V 入れ替えも当面そのまま）。

## 制約

- ビルドは /tmp/build_cores3.sh、BUILD_EXIT=0 まで。パーティション表は
  partitions_muse_cores3.csv のまま（sdkconfig の CUSTOM_FILENAME と
  flash_args の 0x830000 srmodels.bin を確認すること）。
- フラッシュしない。push しない。cores3-custom にコミットする。
- 確認範囲は camera.raw の追加＋コンパイルまで。

## 報告フォーマット

1. 追加した命令の仕様（送り先の組み立て方、返信の中身）
2. コミットハッシュ
3. ビルド結果（BUILD_EXIT）とパーティション表・flash_args の確認結果
4. 書き込み後に確認すべきログ行
