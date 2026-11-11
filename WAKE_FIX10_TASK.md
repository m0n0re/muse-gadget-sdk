# WAKE_FIX10_TASK — raw チャンネルが上がらない根本原因の特定と、内部RAMの余裕づくり

リポジトリ: ~/dev/muse-gadget-sdk（ブランチ cores3-custom、先頭 0d507a9 から作業すること。
ツリーは切り分けのため一時的に afaba78 へ退避してあるので、まず cores3-custom に戻す）
対象ボード: muse-m5stack-cores3

## これまでの対照実験で確定した事実

1. afaba78（ウェイク無し）: 起動47秒でハートビート `raw=up`、voice.speak 正常。
2. ウェイク版では、リンク層（リース取得・noise ハンドシェイク・link.register）と
   muse_chat_session の subscribe 接続まで成功する回もあるのに、
   ハートビートの `raw` は 200 秒観測しても一度も up にならない。
   そして raw が down の間は voice.speak がタイムアウトし、
   ボタンのボイスノートも送れず端末に滞留する。
   → **コマンドとノートが実際に通るのは raw の経路**で、ウェイク版の残る
   本丸は「raw が上がらない理由」である。
3. 後倒し開始のゲート（noise_tunnel_is_connected）は raw と同じ信号を見ており、
   ウェイク版では永遠に true にならず、150秒の強制開始に落ちる。その時点では
   内部RAMの largest が 4608 まで細り、8KB の muse_wake_run タスク（実使用は
   high-water 実測 2324 バイト）が作れず「wake word off」になった。
4. メモリの定常差: ウェイク版 free 約13〜14KB / largest 3〜4KB / min 0KB。
   afaba78 は free 約40KB / largest 19KB。今日の障害はすべてこの差の下流症状。

## やること

1. **raw の正体をコードで特定する。** ハートビート（link.heartbeat）の raw が
   何の状態を指すのか、どのコンポーネントが上げ下げするのか、afaba78 で
   上がるまでの経路を追う。そのうえで、ウェイク版で失敗している箇所に
   失敗理由（esp_err / errno / 確保サイズと当時の free・largest）が残る
   ログを足す。推測で直さず、まず特定する。
2. 特定できたら直す。これまでの同型（tun_tx、muse_vm）は「内部スタックの
   タスク生成が largest 不足で無言失敗」だった。raw の経路でもタスク生成や
   連続確保があれば、PSRAM 化や開始タイミングの後倒しで解消する。
   NVS・フラッシュ書き込みと PSRAM スタックの衝突（cache assert）には注意する。
3. **内部RAMの余裕づくり（ウェイク側のダイエット）:**
   - esp-sr の PSRAM オプション（WakeNet モデルの PSRAM 配置、AFE の
     PSRAM 利用など、sdkconfig で選べるもの）を確認し、使えるものは使う。
   - muse_wake_run のスタックを 8192 → 4096 に下げる（実測使用 2324）。
   - 目標は定常状態で free internal 25KB 以上・largest 10KB 以上。
     到達可否は報告に書く（無理なら理由と到達値）。
4. 後倒しゲートの条件は、raw が上がるようになったら raw の状態そのものに
   合わせる。今の noise_tunnel_is_connected が raw と同一なら、その旨を
   報告に明記する。強制開始（150秒）は残すが、上記3で開始できる状態に
   なっていること。
5. 成功の判定はこちらで実機確認する: 起動後にハートビート raw=up、
   `ready model=wn10_himuse` と alive、voice.speak 完了、滞留ノート送信。

## 制約

- ビルドは /tmp/build_cores3.sh、BUILD_EXIT=0 まで。パーティション表は
  partitions_muse_cores3.csv。ビルド後の sdkconfig の CUSTOM_FILENAME と
  flash_args の srmodels.bin（0x830000）を必ず確認すること。
  sdkconfig の残留（古い表のまま）には特に注意する。
- 既存の診断ログ（warmup / deferred / vm lookup / chat session 段階ログ）は残す。
- フラッシュしない。push しない。cores3-custom にコミットする。
- 既存の挙動は変えない。確認範囲は raw の経路・ウェイクのメモリ配置・
  開始ゲート＋コンパイルまで。
- 実機でしか確定できない点は断定せず、「書き込み後に確認すべきログ行」に残す。

## 報告フォーマット

1. raw の正体（何の状態か、誰が上げるか）と、ウェイク版で失敗する箇所・理由
2. 採った方針と修正内容（ファイルと要点）
3. ウェイクのメモリ配置を変えた結果（sdkconfig の変更点と、想定される定常値）
4. コミットハッシュ
5. ビルド結果（BUILD_EXIT）とパーティション表・flash_args の確認結果
6. 書き込み後にシリアルで確認すべきログ行（成功時／失敗時）
