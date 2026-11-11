# WAKE_FIX13_TASK — セッション維持系の送信に残った DMA ゲートをすべて緩和する

リポジトリ: ~/dev/muse-gadget-sdk（ブランチ cores3-custom、先頭 2114149 から作業）
対象ボード: muse-m5stack-cores3

## 実機ログで確定した現在の状態

2114149 の起動（180秒観測）:
```
t=7.1s  sent link.register → acked
t=7.3s  sent link.heartbeat (27 bytes)   ← この1回だけ
t=7.3s  tunnel stream open、raw=up
t=8.1s  muse_chat_session connected
（以後、link.heartbeat は一度も送られない）
t=72.3s tunnel keepalive timeout (65s idle); marking down
以後5秒ごとに: retrying tunnel stream open → tunnel stream open →
  server reset tunnel stream（約180ms後）→ raw=down のまま
```
- link.heartbeat の送信は register 直後の1回のみ。周期送信がどこかで
  止まっている（FIX12 の緩和は送信関数側。スケジュール側や前段の
  ヘッドルーム事前判定に 16KB のままの箇所がある疑い）。
- トンネルのキープアライブ送信側（noise_tunnel.cpp:306 の
  `tunnel TX paused: DMA headroom`）も FIX12 報告で未対応として
  挙がっている。65秒の無通信はこの経路の停止で説明が付く。
- invoke 結果（voice.speak の完了通知、ノート応答）も、取り出し段と
  送信段の両方で 16KB 判定を受けると FIX12 報告にある。これが残ると
  raw が維持できても命令の完了が返らない。

## やること

1. **トンネルのキープアライブ送信**（noise_tunnel.cpp の DMA ポーズ判定）:
   閾値を確認し、小さいフレームについては register / heartbeat と同じ
   緩和（DMA 空き合計 6KB、最大ブロック 2KB 維持）を適用する。
   大きなデータ送信の判定は変えない。
2. **link.heartbeat の周期送信が止まる箇所**: スケジュール側・前段判定を
   追い、16KB のまま残っている判定があれば同じ緩和を適用する。
   試行したのに送れなかった場合は待機ログが出るようにする
   （無言でスキップしない）。
3. **invoke 結果の送信**（s_result_q の取り出し段の
   `noise_tx_has_dma_headroom(nullptr)` と送信段）: 同じ緩和を適用する。
   結果メッセージは小さいものが前提だが、サイズが大きい場合は
   従来判定に留めるなど、線引きを報告に明記する。
4. 成功の判定はこちらで実機確認する: link.heartbeat が周期的に送られ、
   raw=up が 65 秒を超えて（できれば数分）維持され、keepalive timeout と
   server reset が出ないこと。続けて voice.speak の完了と滞留ノート送信。

## 制約

- ビルドは /tmp/build_cores3.sh、BUILD_EXIT=0 まで。パーティション表は
  partitions_muse_cores3.csv。ビルド後の sdkconfig の CUSTOM_FILENAME と
  flash_args の srmodels.bin（0x830000）を必ず確認すること。
- 既存の診断ログ（待機ログ、raw down 定期ログ等）は残す。
- フラッシュしない。push しない。cores3-custom にコミットする。
- 大きな送信（音声データや大きな本体）の DMA 判定は変えない。
  確認範囲は上記3経路の送信判定＋コンパイルまで。

## 報告フォーマット

1. 修正内容（経路ごとに、ファイルと要点、緩和の線引き）
2. heartbeat 周期送信が止まっていた箇所の特定結果
3. コミットハッシュ
4. ビルド結果（BUILD_EXIT）とパーティション表・flash_args の確認結果
5. 書き込み後にシリアルで確認すべきログ行（成功時／失敗時）
