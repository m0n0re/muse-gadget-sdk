# WAKE_FIX4_TASK — __cxa_guard_acquire の line 123 abort を直す

リポジトリ: ~/dev/muse-gadget-sdk（ブランチ cores3-custom、先頭 e5a96fa から作業すること。
ツリーは切り分けのため一時的に afaba78 へ退避してあるので、まず cores3-custom に戻す）
対象ボード: muse-m5stack-cores3

## 前回（e5a96fa）の実機結果

- 起動ログ: `start:` → `largest internal block 15872, heap integrity ok` →
  `C++ static-init probe ok` → `create wn10_himuse: begin` → **abort**（再起動ループ）。
- つまりプローブ（mutex と counting semaphore の生成）は成功するのに create は落ちる。
  「static_init_prepare の生成失敗」説は実機で否定された。

## abort の正確な場所（今回の核心）

e5a96fa のバックトレースを /tmp/crash_e5a96fa.elf で復号済み:

```
__cxa_guard_acquire (esp-idf-v6/components/cxx/cxx_guards.cpp:123)
← dl::module::ModuleCreator::get_instance() (dl_module_creator.hpp:27)
← wakenet::WakeNetX::build_model() (wakenetx.cpp:314)
← wakenetx_create (wakenetx.cpp:1146)
← muse_wake_start (muse_wake.c:139) ← muse_voice_start (muse_voice.c:1510)
← muse_app_run (muse_app.c:103) ← boot_task (main/muse_glue.c:640)
```

cxx_guards.cpp の 123 行目は、**スケジューラ未起動ブランチの abort()** であることを
ソースで確認した（118〜123行目）:

```c
const auto scheduler_started = xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED;
if (!scheduler_started) {
    if (g->pending) {
        abort();   // ← 123行目
    }
}
```

すなわち abort の条件は次の2つが同時に成立したこと:
1. 呼び出し時点で `xTaskGetSchedulerState() == taskSCHEDULER_NOT_STARTED`
   （スケジューラが動いていない文脈から呼ばれた）
2. その static（ModuleCreator の singleton）のガードが既に `pending`
   （初期化が開始済みだが完了していない）

前回の報告では (a) を「boot_task の最中なので除外」としたが、実機はこの行で
落ちている。boot_task と呼ばれる経路が本当に通常タスク文脈なのか、再検証が要る。

## 調べてほしいこと

1. **muse_app_run / muse_voice_start の呼び出し文脈**: main/muse_glue.c の boot_task は
   本当にスケジューラ起動後に走る FreeRTOS タスクか。app_main から直接呼ばれる関数、
   ESP_SYSTEM_INIT 系の初期化関数、コンストラクタ等ではないか。呼び出しが複数回
   起きていないか（過去の起動ログで `muse_wake: start:` が1回の起動に2回出た
   記録がある）。
2. **pending が立ったままになる経路**: スケジューラ未起動の文脈で
   ModuleCreator::get_instance()（または同じ singleton を使う esp-dl の処理）が
   先に呼ばれ、初期化が完了しないまま残る箇所はないか。esp-dl を使う他の処理
   （ノイズ制御、AFE まわり）の初期化タイミングと文脈も含めて。
3. **xTaskGetSchedulerState の見え方**: 呼び出しが割り込み文脈や、スケジューラ
   起動前の初期化フェーズにいないか。

## 直し方の方向（原因に合わせて選ぶこと）

- ウェイクの初期化（少なくとも最初の esp-dl static 初期化）を、スケジューラ起動後の
  通常タスク文脈で、ちょうど1回だけ実行する。
- 先行して pending を残す文脈があるなら、その文脈側の呼び出しを後段へ移すか、
  singleton の初期化を安全な文脈で先に済ませる（warm-up）。
- 二重起動があるなら一本化する。

## 制約

- ビルドは /tmp/build_cores3.sh、BUILD_EXIT=0 まで。パーティション表は
  partitions_muse_cores3.csv（ビルドディレクトリ sdkconfig の値を確認すること。
  スティッキー問題に注意。前回、build 側 sdkconfig が partitions_muse.csv に
  固定されていた件も再発防止を確認）。
- e5a96fa で入れた診断ログ（start / largest block / heap integrity / probe /
  create begin・returned・FAILED / ready / alive）は残す。文脈の切り分けに
  必要なら、muse_wake_start 入口で xTaskGetSchedulerState の値と
  呼び出しタスク名を出すログを足すこと。
- フラッシュしない。push しない。cores3-custom にコミットする。
- 既存の挙動（ボタンのボイスノート、TTS、voice.speak、アバター、ボタンタップの
  通話経路）は変えない。確認範囲はウェイクの初期化経路と共有部分＋コンパイルまで。
- 実機でしか確定できない点は、断定せず「書き込み後に確認すべきログ行」として残す。

## 報告フォーマット

1. 呼び出し文脈の実態（boot_task の正体、呼び出し回数、スケジューラとの関係）
2. pending が残る経路の特定結果
3. 原因
4. 修正内容（ファイルと要点）
5. コミットハッシュ
6. ビルド結果（BUILD_EXIT）とパーティション表の確認結果
7. 書き込み後にシリアルで確認すべきログ行（成功時／失敗時）
