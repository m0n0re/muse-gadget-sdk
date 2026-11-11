# WAKE_FIX7_TASK — ウェイク初期化とリース取得（TLS）の競合を解く

リポジトリ: ~/dev/muse-gadget-sdk（ブランチ cores3-custom、先頭 0314333 から作業）
対象ボード: muse-m5stack-cores3

## 実機で確認した症状（0314333 の起動ログ）

- tun は直った: `link.tun: tun_tx task started (stack in PSRAM)`、
  `tunnel netif up: ip=10.42.0.1` が出る。ウェイクも ready / alive 正常。
- しかしハートビートは `ws=up raw=down`、`tun rx/tx 0` のまま（起動93秒時点）。
- 起動直後の並行処理で次が起きている:
  ```
  I (3170) muse_wake: start: ...           ← ウェイク初期化（約3.2秒）
  I (3224) link.app: found device credentials in NVS, fetching leased VM
  E (3312) esp-tls-mbedtls: mbedtls_ssl_setup returned -0x008D
  E (3313) esp-tls: create_ssl_handle failed / Failed to open new connection
  E (3316) HTTP_CLIENT: Connection failed, sock < 0
  ```
  リース取得の HTTPS が TLS セットアップのメモリ確保で失敗し、リースが取れない
  ため raw チャネルが上がらない、という読みである（-0x008D の内訳確認も含めて
  検証してほしい）。過去のウェイク版ブートでも同じ TLS エラーが出ていた。
- ウェイク無し版（afaba78）では raw=up になる。今日の一連の障害はすべて
  起動ピーク時の内部RAM不足・断片化が起点。

## やること

1. リース取得の失敗と raw=down の因果を確認する（再試行の有無と間隔も）。
   再試行があるのに回復しないのか、再試行自体がないのかで打ち手が変わる。
2. 直し方の方針（効果のあるものを選ぶ。併用可）:
   - **ウェイク初期化の後倒し**: muse_wake_start を、リンクが完全に上がった後
     （raw=up 後、またはリース取得完了後）に実行する。起動直後の数秒は
     聞き取りが始まらなくてよい。warmup（app_main 先頭）はそのままでよい。
   - リース取得のリトライ強化（失敗しても定期的に再試行して回復する）。
   - TLS 側のメモリ対策（mbedTLS の確保先を PSRAM に寄せる等）は、上の2つで
     足りない場合のみ検討する。
3. 成功の判定はこちらで実機確認する: 起動後のハートビートで raw=up、
   voice.speak 完了、ウェイクのノートが送れる、ready / alive も正常。

## 制約

- ビルドは /tmp/build_cores3.sh、BUILD_EXIT=0 まで。パーティション表は
  partitions_muse_cores3.csv。ビルド後の sdkconfig の CUSTOM_FILENAME と
  flash_args の srmodels.bin（0x830000）を必ず確認すること。
- ウェイクとリンクの診断ログは残す。ウェイク開始のタイミングが分かるログ
  （何を契機に開始したか）を入れること。
- フラッシュしない。push しない。cores3-custom にコミットする。
- 既存の挙動（ボタンのボイスノート、TTS、voice.speak、アバター、通話経路、
  ウェイク検知）は変えない。リンクの再接続処理の意味も変えない。
  確認範囲は初期化順序と再試行まわり＋コンパイルまで。

## 報告フォーマット

1. リース取得失敗と raw=down の因果（再試行の実態を含む）
2. 採った方針と修正内容（ファイルと要点）
3. コミットハッシュ
4. ビルド結果（BUILD_EXIT）とパーティション表・flash_args の確認結果
5. 書き込み後にシリアルで確認すべきログ行（成功時／失敗時）
