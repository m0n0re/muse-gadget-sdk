# WAKE_FIX8_TASK — 後倒しタスクの PSRAM スタックが起こす cache-freeze assert

リポジトリ: ~/dev/muse-gadget-sdk（ブランチ cores3-custom、先頭 f133956 から作業）
対象ボード: muse-m5stack-cores3

## 実機で確認した症状（f133956 の起動ログ）

良い点:
- TLS エラーは消えた。リース取得は成功している:
  `link.vm_api: HTTP 200` → `connecting to VM ...` →
  `link.noise_ctrl: TLS connected ... handshake msg3 sent, session established`、
  `tunnel netif up` も出る。
- 後倒しの契機も動いた:
  `deferred start: trigger=link up ... after 4 s (free internal 30947, largest 12800)`

新しいクラッシュ（毎回の起動で再現）:
```
muse_wake: deferred start: trigger=link up ...
muse_wake: start: ... task "muse_wake_init", core 1 ...
MODEL_LOADER: ... (srmodels 読み込み中)
assert failed: esp_cache_freeze_caches_disable_interrupts
  esp_cache_utils.c:96 (s_task_stack_is_sane_when_cache_frozen())
→ Rebooting...
```
- 原因は明確: muse_wake_start を **PSRAM スタックの muse_wake_init タスク**から
  呼んでいる。モデル読み込みのフラッシュ操作でキャッシュが凍結される際、
  外部RAMスタックは sane でないため assert する。以前は muse_boot
  （内部スタック）から呼んでいたので起きなかった。
- このクラッシュで起動が7秒あたりで落ち続けるため、その先の接続
  （muse_chat_session の再試行や raw=up）まで到達できていない。
  なお起動 約3.4秒で `muse_chat_session: connect failed: Can't reach Muse's
  server` が出るが、これは VM セッション確立（約5.8秒）より前の先行失敗で、
  クラッシュが直れば再試行で回復する見込み。回復しない場合は報告に残すこと。

## やること

1. muse_wake_start の実行を、内部RAMスタックのタスク文脈で行う。
   方針候補:
   - muse_wake_init タスク自体を内部スタックにする（短命で小さい。当時の
     空きは free 30947 / largest 12800 なので 4〜6KB なら収まるはず）。
   - ポーリング役（PSRAM可）と開始実行役（内部スタック）を分ける。
   いずれでもよいが、モデル読み込みを含む muse_wake_start の呼び出しが
   内部スタックで走ることを保証すること。
2. 成功の判定はこちらで実機確認する: `ready model=wn10_himuse` と alive、
   ハートビート raw=up、voice.speak 完了、ノート送信。
   あわせて muse_chat_session が VM 確立後に接続できるかもログで追えるよう、
   既存ログで不足する場合は接続成功のログを足す。

## 制約

- ビルドは /tmp/build_cores3.sh、BUILD_EXIT=0 まで。パーティション表は
  partitions_muse_cores3.csv。ビルド後の sdkconfig の CUSTOM_FILENAME と
  flash_args の srmodels.bin（0x830000）を必ず確認すること。
- 診断ログ（warmup / deferred / start / create / ready / alive）は残す。
- フラッシュしない。push しない。cores3-custom にコミットする。
- 既存の挙動は変えない。確認範囲はウェイク開始の実行文脈＋コンパイルまで。

## 報告フォーマット

1. 採った方針（スタックの置き場所とサイズ、根拠）
2. 修正内容（ファイルと要点）
3. コミットハッシュ
4. ビルド結果（BUILD_EXIT）とパーティション表・flash_args の確認結果
5. 書き込み後にシリアルで確認すべきログ行（成功時／失敗時）
6. muse_chat_session の再試行について分かったこと（あれば）
