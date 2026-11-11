# WAKE_FIX6_TASK — ウェイク版で link.tun（raw トンネル）が上がらない問題

リポジトリ: ~/dev/muse-gadget-sdk（ブランチ cores3-custom、先頭 4033cbf から作業）
対象ボード: muse-m5stack-cores3

## 実機で確認した症状

- ウェイクは動いている（`ready model=wn10_himuse`、`alive` で fed 増加、
  実発声で `wake word detected` → ノート録音 4.5秒まで成功）。
- しかしサービスとのデータ経路が死んでいる:
  - ハートビートが `ws=up raw=down`、`tun rx+0/0B tx+0/0B` のまま（再起動2回とも）。
  - ノート送信時にデバイス側が `muse: CAN'T REACH MUSE` を出し、ノートは
    保存されて再送待ち（画面に SAVED NOTE : WILL TRY AGAIN）。
  - こちらからの voice.speak 呼び出しもタイムアウト／オフライン扱い。
- ウェイク無しの版（afaba78）では同じハートビートが `raw=up` になる。
- 過去のウェイク版ブートログ（32656a8）には、起動 約6.3秒で
  `link.tun: xTaskCreate failed` と `link.app: tunnel netif failed to start`
  が出ていた。muse_wake_start は約3.2秒で、その後である。
- 内部RAMの空き: ウェイク版は約17〜18KB、afaba78 は約39KB。今日の障害は
  すべて内部RAMの不足・断片化が起点（リング確保、static 初期化、検知タスク、
  そして今回の tun タスク生成）。

## やること

1. link.tun のタスク生成がウェイク版で失敗する理由を確認する
   （生成タイミング、必要スタック、内部RAMの状態）。
2. ウェイク版でも `raw=up` で安定して上がるように直す。方針候補:
   - tun タスクのスタックを PSRAM に置く（muse_chat や検知タスクの前例あり）
   - tun の生成をウェイク初期化より前に済ませる（順序の変更）
   - ウェイク側の内部RAM使用の削減
   いずれも、他の経路（link の再接続処理を含む）の挙動を壊さないこと。
3. 成功の判定はこちらで実機確認する: ハートビートで raw=up、voice.speak が
   完了する、ウェイクのノートが CAN'T REACH MUSE にならず送れる。

## 制約

- ビルドは /tmp/build_cores3.sh、BUILD_EXIT=0 まで。パーティション表は
  partitions_muse_cores3.csv。ビルド後の sdkconfig の CUSTOM_FILENAME と
  flash_args の srmodels.bin（0x830000）を必ず確認すること。
- ウェイクの診断ログ（warmup / start / probe / create / ready / alive）は残す。
- フラッシュしない。push しない。cores3-custom にコミットする。
- 既存の挙動（ボタンのボイスノート、TTS、voice.speak、アバター、通話経路、
  ウェイク検知そのもの）は変えない。確認範囲は link とウェイクの初期化順序・
  メモリまわり＋コンパイルまで。

## 報告フォーマット

1. tun タスク生成失敗の理由（タイミングとメモリの裏付け）
2. 採った方針と修正内容（ファイルと要点）
3. コミットハッシュ
4. ビルド結果（BUILD_EXIT）とパーティション表・flash_args の確認結果
5. 書き込み後にシリアルで確認すべきログ行（成功時／失敗時）
