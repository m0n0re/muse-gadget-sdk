# WAKE_FIX12_TASK — link.heartbeat の送信にも DMA 下限の緩和を広げる

リポジトリ: ~/dev/muse-gadget-sdk（ブランチ cores3-custom、先頭 19b7b29 から作業）
対象ボード: muse-m5stack-cores3

## 実機ログで確定した現在の状態

19b7b29 は register が通るようになった（`sent link.register` → acked →
`tunnel stream open` → ハートビート raw=up、ウェイクも ready）。
しかし約65秒後に次が起き、以後ループする:
```
link.noise_tun: tunnel keepalive timeout (65s idle); marking down
link.noise_tun: retrying tunnel stream open
link.noise_tun: tunnel stream open (stream 2)
link.noise_ctrl: server reset tunnel stream   ← 約180ms後に必ずリセット
（5秒ごとに再試行 → 同じリセット、raw=down のまま）
```
- 原因は FIX11 報告で予告されていた件: `link.heartbeat` の送信も
  `send_json_body_chunk()` の DMA ヘッドルーム判定（空き合計16KB）を通る。
  ウェイク常駐時の DMA 空きは約9〜14KB で、この判定を常に満たせず、
  heartbeat がサーバーへ届かない。サーバー側は端末が消えた扱いになり、
  トンネルのキープアライブも届かず、開き直したストリームも即リセットする。
- 起動直後には `link.heartbeat send waiting for DMA headroom:
  dma free 14439 (need 16384), dma largest 5888` が実際に出ている。

## やること

1. link.heartbeat の送信についても、register と同じ緩和
   （DMA 空き合計の下限 6KB、最大ブロック 2KB は維持）を適用する。
   実装は register の場合と揃える（対象メッセージの識別方法も同じ流儀で）。
2. あわせて、同じ経路を通る小さな制御メッセージで、サーバー側の
   生存判定やセッション維持に必要なものがあれば、報告に列挙する。
   今回直すのは heartbeat までとし、それ以外は列挙に留める。
3. 成功の判定はこちらで実機確認する: 起動後 raw=up が 65 秒を超えて
   維持されること（キープアライブ・タイムアウトや server reset が
   出ないこと）、ready model と alive の継続、voice.speak の完了、
   滞留ノートの送信。

## 制約

- ビルドは /tmp/build_cores3.sh、BUILD_EXIT=0 まで。パーティション表は
  partitions_muse_cores3.csv。ビルド後の sdkconfig の CUSTOM_FILENAME と
  flash_args の srmodels.bin（0x830000）を必ず確認すること。
- 既存の診断ログ（待機ログ、raw down 定期ログ等）は残す。
- フラッシュしない。push しない。cores3-custom にコミットする。
- 通常の制御メッセージ（heartbeat と register 以外）の 16KB 判定は変えない。
  確認範囲は link の送信判定＋コンパイルまで。

## 報告フォーマット

1. 修正内容（ファイルと要点）
2. セッション維持に関わる他の小さい制御メッセージの列挙（あれば）
3. コミットハッシュ
4. ビルド結果（BUILD_EXIT）とパーティション表・flash_args の確認結果
5. 書き込み後にシリアルで確認すべきログ行（成功時／失敗時）
