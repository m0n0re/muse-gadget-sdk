# WAKE_FIX9_TASK — ウェイク版で muse_chat_session の接続が失敗する問題

リポジトリ: ~/dev/muse-gadget-sdk（ブランチ cores3-custom、先頭 b7d5fa8 から作業すること。
ツリーは切り分けのため一時的に afaba78 へ退避してあるので、まず cores3-custom に戻す）
対象ボード: muse-m5stack-cores3

## 対照実験で確定したこと

- afaba78（ウェイク無し）に書き戻すと、起動47秒でハートビート `raw=up`、
  voice.speak も正常。**サービス側は健全**で、問題はウェイク版のローカル状態に
  限られる。
- ウェイク版 b7d5fa8 の起動ログ:
  - リンク層は完全動作: リース HTTP 200、noise ハンドシェイク、
    `session established`、`link.register acked`、トンネル netif up。
  - ウェイクも正常: deferred start → `ready model=wn10_himuse`、alive で fed 増加。
  - しかし `muse_chat_session: connect failed: Can't reach Muse's server` が
    起動3.4秒・8.4秒・18.6秒・38.7秒と再試行のたびに出続け、`connected to
    Muse's VM` は一度も出ない。ハートビートは `raw=down` のまま。
  - 起動71秒には `link.noise_ctrl: server reset tunnel stream` も出た。
- メモリの差: ウェイク版の定常状態は内部RAM free 約13KB（min 0KB、largest 約3KB）。
  afaba78 は free 約40KB。今日の障害はすべてこの差に起因している。

## やること

1. muse_chat_session の接続処理（resolve_vm、connect_once、その先）を追い、
   失敗している段階と理由を特定する。現状の `Can't reach Muse's server` は
   まとめすぎなので、段階ごとの失敗（DNS、ソケット生成、connect、TLS、
   ハンドシェイク、メモリ確保など）と esp_err / errno が分かるログを足す。
2. ウェイク常駐時の内部RAM状態（free 13KB / largest 3KB 級）で、その段階が
   なぜ失敗するのかを裏付ける。
3. 直す。方針候補（原因に合わせて選ぶ。併用可）:
   - chat session 側のバッファ・スタックを PSRAM に逃がす
   - ウェイク側の内部RAM占有を減らす（検知まわりの内部バッファの PSRAM 化など）
   - 接続処理の実行タイミングや再試行条件の見直し（メモリが戻る契機と合わせる）
4. 成功の判定はこちらで実機確認する: 起動後に `connected to Muse's VM
   (session established)` が出て、ハートビート raw=up、voice.speak 完了、
   ウェイクのノートが送れること。

## 制約

- ビルドは /tmp/build_cores3.sh、BUILD_EXIT=0 まで。パーティション表は
  partitions_muse_cores3.csv。ビルド後の sdkconfig の CUSTOM_FILENAME と
  flash_args の srmodels.bin（0x830000）を必ず確認すること。
- ウェイクとリンクの診断ログは残す。今回足す chat session の段階ログも残す。
- フラッシュしない。push しない。cores3-custom にコミットする。
- 既存の挙動（ボタンのボイスノート、TTS、voice.speak、アバター、通話経路、
  ウェイク検知、リンクの再接続処理の意味）は変えない。
  確認範囲は chat session の接続経路とメモリまわり＋コンパイルまで。
- 実機でしか確定できない点は、断定せず「書き込み後に確認すべきログ行」として残す。

## 報告フォーマット

1. 接続が失敗する段階と、その理由（ログ・コードの裏付け）
2. 採った方針と修正内容（ファイルと要点）
3. コミットハッシュ
4. ビルド結果（BUILD_EXIT）とパーティション表・flash_args の確認結果
5. 書き込み後にシリアルで確認すべきログ行（成功時／失敗時）
