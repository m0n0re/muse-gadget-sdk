# WAKE_FIX3_TASK — wakenetx_create 内の abort を直す

リポジトリ: ~/dev/muse-gadget-sdk（ブランチ cores3-custom、先頭 32656a8 から作業すること。
現在ツリーは切り分けのため afaba78 にチェックアウトされているので、まず cores3-custom に戻す）
対象ボード: muse-m5stack-cores3

## 確定した事実（実機ログと addr2line で確認済み）

### 1. クラッシュの場所（32656a8 のバックトレースを復号）

```
abort() ← __cxa_guard_acquire (esp-idf cxx/cxx_guards.cpp:123)
← dl::module::ModuleCreator::get_instance() (esp-dl dl_module_creator.hpp:27)
← wakenet::WakeNetX::build_model() (wakenetx.cpp:314)
← wakenet::WakeNetX::WakeNetX() ← wakenetx_create (wakenetx.cpp:1146)
← muse_wake_start (muse_wake.c:113)
← muse_voice_start (muse_voice.c:1510)
← muse_app_run (muse_app.c:103) ← boot_task (main/muse_glue.c:640)
```

クラッシュした ELF は Studio の /tmp/crash_32656a8.elf に保管してある。
起動後 約3.2秒、`muse_wake: start:` の直後に毎回落ちて再起動ループになった。

### 2. 「安定して見えた版」も同じ場所で落ちる

- 切り分け版 b3a06c9 でも、同じ形の abort（復号は未実施だが先頭フレームが同型）が
  実機ログに複数回出ている。
- b3a06c9 で落ちなかった起動では、muse_wake_start のリングバッファ確保
  （内部RAM・16KB）が失敗して無言で return false し、wakenetx_create に
  到達していなかった。つまり「安定していた」のではなく「ウェイクが起動せずに
  済んでいた」だけ。確保できるメモリ状態の起動では必ず create で落ちる。
- 32656a8 の修正（リングを PSRAM にフォールバック）で確保が常時成功するように
  なったため、毎回落ちるようになって露見した。

### 3. 周辺の経緯

- 最初のウェイク書き込み（2026-11-11 13:40ごろ）でも別形態のクラッシュ
  （tlsf の assert）が同ライブラリ周辺で起き、04c146d で esp-dl をピン留めした。
  そのピン留め版でも今回の abort が起きる。esp-sr 2.6.1 のプリビルド済み
  wakenet ライブラリと、ピン留めした esp-dl の版の組み合わせが疑わしい。
- __cxa_guard_acquire の abort は、関数内 static の初期化ガードの再入でも起きる。
  ModuleCreator::get_instance() は Meyers singleton。ウェイク以外で esp-dl を
  使う処理（ノイズ制御まわり等）が別タスクで同時に singleton 初期化へ入る
  可能性も含めて見てほしい。起動ログでは noise_ctrl_init が voice start より
  前に走っている。

## やること

1. 原因を特定する。候補は少なくとも:
   - esp-dl と esp-sr（プリビルド wakenet）の版の不一致
   - singleton 初期化の競合・再入（初期化の直列化や、create の単一化で解けるか）
   - C++ ランタイムまわりの sdkconfig（ガードや例外の設定）
2. wakenetx_create が正常に完了し、`muse_wake: ready model=wn10_himuse` と
   `alive: fed=...`（fed が増える）が出る状態にする。
3. 32656a8 で入れたログ（start / ready / alive / 失敗時の ERROR）は残す。
   create の前後にも、通過が分かるログを必要なら足す。

## 制約

- ビルドは /tmp/build_cores3.sh、BUILD_EXIT=0 まで。パーティション表は
  partitions_muse_cores3.csv（ビルドディレクトリ sdkconfig の値を確認すること。
  スティッキー問題に注意）。
- フラッシュしない。push しない。cores3-custom にコミットする。
- 既存の挙動（ボタンのボイスノート、TTS、voice.speak、アバター、ボタンタップの
  通話経路）は変えない。確認範囲はウェイクの初期化と共有する部分＋コンパイルまで。
- 実機なしで断定できない部分は、断定せず「書き込み後にシリアルで確認すべき
  ログ行」として報告に残すこと。前回のように推測の修正だけで済ませず、
  少なくとも create の成否がログで判別できる状態にして返すこと。

## 報告フォーマット

1. 原因（特定できたか、できなければ残る候補とその根拠）
2. 修正内容（ファイルと要点、版を変えた場合は変更前後）
3. コミットハッシュ
4. ビルド結果（BUILD_EXIT）とパーティション表の確認結果
5. 書き込み後にシリアルで確認すべきログ行（成功時／失敗時）
