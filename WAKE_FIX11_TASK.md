# WAKE_FIX11_TASK — link.register が送られない分岐の特定と修正

リポジトリ: ~/dev/muse-gadget-sdk（ブランチ cores3-custom、先頭 ff8b156 から作業）
対象ボード: muse-m5stack-cores3

## 実機ログで確定した新しい事実

ff8b156 の起動ログ（/tmp/bootlog24.txt 相当の観測）:
```
link.noise_ctrl: TLS connected ... session established
link.noise_ctrl: opened POST /link-control stream
link.noise_ctrl: got response status=200 on stream 1
（ここで途絶える）
link.noise_ctrl: raw down: register_acked=0 tunnel_opened=0 （10秒ごと、ずっと）
```
- **`sent link.register` の行が一度も出ない。** 登録そのものが送られていない。
  したがって register_acked=0 のまま /link-tunnel は開かれず、raw は上がらない。
- 対照: b7d5fa8 の起動では `sent link.register (3065 bytes)` →
  `link.register acked; tunnel may open` まで進んでいた（その後サーバーが
  ストリームをリセットし、raw は維持できなかった）。
- ff8b156 の同時点のメモリは free internal 18851〜23403 / largest 7680 で、
  FIX10 で疑った ack 応答のパース以前の問題である。ack の話以前に、
  「stream 1 の応答を受けてから link.register を送るまで」のどこかで
  無言で止まっている。
- afaba78 では free 約40KB でこの経路は常に通る。メモリ圧迫下でのみ
  止まる分岐（確保失敗の無言 return、コールバック／タスク生成の失敗、
  応答パースの失敗など）が濃厚だが、特定はコード読みとログで行うこと。

## やること

1. noise_control.cpp（と関係ファイル）で、「POST /link-control の応答
   （status=200）を受けてから link.register を送信するまで」の経路を
   すべて追い、止まりうる分岐を列挙する。各分岐に、理由と当時の
   free / largest が残るログを足す（無言の return / NULL / pdFAIL を残さない）。
2. 特定した失敗点に合わせて直す。方針候補:
   - 応答のパースやバッファ確保が内部RAMで失敗するなら、PSRAM へ逃がす、
     確保しない判定に変える、確保サイズを小さくする。
   - タスク生成が失敗するなら、既存の前例（tun_tx、muse_vm）と同じく
     PSRAM スタックのフォールバックを付ける。ただし NVS・フラッシュ
     書き込みと PSRAM スタックの衝突には注意する。
   - 一度失敗すると二度と試みない構造なら、セッションが生きている間は
     register の送信を間隔を置いて再試行する（無限の密な再試行は避ける）。
3. b7d5fa8 で見えた「acked の後にサーバーが /link-tunnel をリセットし、
   raw が維持できない」件も、今回のログで追える範囲でよいので、
   ストリーム再オープンの再試行が実際に走っているか確認し、
   走っていなければその旨を報告に残す（今回の主眼は register 送信まで）。
4. 成功の判定はこちらで実機確認する: `sent link.register` →
   `link.register acked` → `tunnel stream open` → ハートビート raw=up が
   維持されること、続けて ready model と voice.speak。

## 制約

- ビルドは /tmp/build_cores3.sh、BUILD_EXIT=0 まで。パーティション表は
  partitions_muse_cores3.csv。ビルド後の sdkconfig の CUSTOM_FILENAME と
  flash_args の srmodels.bin（0x830000）を必ず確認すること。
  sdkconfig の残留（古い表のまま）には特に注意する。
- 既存の診断ログ（warmup / deferred / vm lookup / chat session 段階ログ /
  raw down 定期ログ）は残す。
- フラッシュしない。push しない。cores3-custom にコミットする。
- 既存の挙動は変えない。確認範囲は link の登録経路＋コンパイルまで。
- 実機でしか確定できない点は断定せず、「書き込み後に確認すべきログ行」に残す。

## 報告フォーマット

1. register が送られない分岐の特定結果（コードの裏付け）
2. 採った方針と修正内容（ファイルと要点）
3. /link-tunnel 再オープン再試行について分かったこと
4. コミットハッシュ
5. ビルド結果（BUILD_EXIT）とパーティション表・flash_args の確認結果
6. 書き込み後にシリアルで確認すべきログ行（成功時／失敗時）
