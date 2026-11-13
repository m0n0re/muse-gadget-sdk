# NOTIFY_FIX_TASK — notify.get をコマンド宣言にも足す

リポジトリ: ~/dev/muse-gadget-sdk（ブランチ cores3-custom、先頭 85f4ed6）
対象ボード: muse-m5stack-cores3

## 問題

85f4ed6 で `notify.get` のハンドラ（app.c）と設定は入ったが、
`esp32/main/noise_control.cpp` のコマンド宣言（add_command）に
`notify.get` が無いため、プラットフォーム側に通知されず、呼び出しが
`node_command_unsupported` で弾かれる（書き込み後の実機で確認済み）。

## やること

- noise_control.cpp の CoreS3 ブロック（camera 系や head 系の
  add_command が並ぶ場所）に、`notify.get` の宣言を既存の流儀で
  追加する。引数なし。説明文は「お知らせ読み上げの設定
  （enabled と from_min / to_min）を返す照会命令」であることが
  分かる英文で。timeout_ms は照会系の既存命令に合わせる。
- それ以外の変更はしない。

## 制約

- ビルドは /tmp/build_cores3.sh、BUILD_EXIT=0 まで。パーティション表は
  partitions_muse_cores3.csv のまま（flash_args の 0x830000
  srmodels.bin も確認）。
- フラッシュしない。push しない。cores3-custom にコミットする。

## 報告フォーマット

1. 追加した宣言の内容（説明文と timeout_ms）
2. コミットハッシュ
3. ビルド結果（BUILD_EXIT）とパーティション表・flash_args の確認結果
