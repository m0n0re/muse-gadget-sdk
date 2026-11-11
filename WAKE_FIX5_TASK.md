# WAKE_FIX5_TASK — 検知タスク生成の失敗（内部RAM不足）を直す

リポジトリ: ~/dev/muse-gadget-sdk（ブランチ cores3-custom、先頭 c0fa89f から作業）
対象ボード: muse-m5stack-cores3

## 実機で確認した現在地（c0fa89f の起動ログ）

```
muse_wake: warmup: scheduler state 2 (running), task "main", core 0
muse_wake: warmup: ModuleCreator singleton 0x3fcadae8 ready
muse_wake: start: scheduler state 2, task "muse_boot", core 1, in ISR 0
muse_wake: start: free internal 23591, psram 4553516
muse_wake: start: largest internal block 15360, heap integrity ok
muse_wake: C++ static-init probe ok
muse_wake: create wn10_himuse: begin (free internal 23255)
muse_wake: create wn10_himuse: returned (free internal 15247)   ← create は初めて成功
E muse_wake: no wake task (free internal 15247, largest block 4608): wake word off
W muse_voice: wake word not running (see muse_wake log)
```

- warm-up が効いて wakenetx_create が初めて正常に完了した。abort はゼロ、
  再起動ループもなし。本体は安定動作している（ウェイクだけ off の状態）。
- 残る失敗は1つだけ: **create の後では内部RAMの最大ブロックが 4608 バイトまで
  減っていて、検知タスクの生成（スタック確保）に失敗する。**

## やること

検知タスクが確実に作れるようにする。方針は実装側で選んでよいが、候補:

1. 検知タスクのスタックを PSRAM に置く（xTaskCreateWithCaps 等）。
   ただし検知処理がそのスタック上で動く前提で問題がないか（DMA・キャッシュ
   無効区間・割り込み文脈での使用がないか）を確認すること。
2. タスク生成の順序を変える（create より前に、内部RAMに余裕がある時点で
   検知タスクを作っておき、モデルは後から渡す）。
3. スタックサイズ自体の見直し（実測に対して過大なら）。
4. 1と2の併用でもよい。

成功の判定: 起動ログで `ready model=wn10_himuse` が出て、`alive: fed=...` の
fed が増え続けること。こちらで書き込んでシリアル確認する。

## 制約

- ビルドは /tmp/build_cores3.sh、BUILD_EXIT=0 まで。パーティション表は
  partitions_muse_cores3.csv。ビルドディレクトリ sdkconfig のスティッキーに
  注意し、ビルド後に CUSTOM_FILENAME を必ず確認すること。
  flash_args に srmodels.bin（0x830000）が入っている状態を維持すること。
- c0fa89f までの診断ログ（warmup / start / probe / create begin・returned /
  ready / alive / 失敗時の ERROR）は残す。
- フラッシュしない。push しない。cores3-custom にコミットする。
- 既存の挙動（ボタンのボイスノート、TTS、voice.speak、アバター、ボタンタップの
  通話経路）は変えない。確認範囲はウェイクまわりと共有部分＋コンパイルまで。

## 報告フォーマット

1. 採った方針と、その根拠（スタックの置き場所・生成順序・サイズ）
2. 修正内容（ファイルと要点）
3. コミットハッシュ
4. ビルド結果（BUILD_EXIT）とパーティション表・flash_args の確認結果
5. 書き込み後にシリアルで確認すべきログ行（成功時／失敗時）
