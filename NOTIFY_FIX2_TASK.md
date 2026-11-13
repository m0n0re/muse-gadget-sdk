# NOTIFY_FIX2_TASK — notify.get の返信を payload の下に置く

リポジトリ: ~/dev/muse-gadget-sdk（ブランチ cores3-custom、先頭 0735114）
対象ボード: muse-m5stack-cores3

## 問題

書き込み後の実機で `notify.get` は実行できるようになったが、
呼び出し側に返るのは `{"ok": true}` だけで、`enabled` /
`from_min` / `to_min` が届かない。現状のハンドラ（app.c）は
head.state と同じく result 直下に項目を置いている。
一方 camera.look の返信（bytes / file など）は呼び出し側に届く。
そちらは成功項目を `payload` の下に置く形になっている
（muse_look.c とその呼び出し側の包み方を参照）。

## やること

- app.c の `notify.get` ハンドラの返信を、camera.look の成功返信と
  同じ形に変える: `ok: true` は従来どおり持ち、設定の3項目は
  `payload` オブジェクトの下に置く。
- head.state など他の命令の返信は変えない。
- ログ行（`notify.get: enabled ... from ... to ...`）は残す。

## 制約

- ビルドは /tmp/build_cores3.sh、BUILD_EXIT=0 まで。パーティション表は
  partitions_muse_cores3.csv のまま（flash_args の 0x830000
  srmodels.bin も確認）。
- フラッシュしない。push しない。cores3-custom にコミットする。

## 報告フォーマット

1. 変えた返信の形（変更前後の JSON 例）
2. コミットハッシュ
3. ビルド結果（BUILD_EXIT）とパーティション表・flash_args の確認結果
