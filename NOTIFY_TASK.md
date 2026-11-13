# NOTIFY_TASK — お知らせ読み上げの ON/OFF と時間帯を本体設定にする

リポジトリ: ~/dev/muse-gadget-sdk（ブランチ cores3-custom、先頭 32563b0）
対象ボード: muse-m5stack-cores3

## 背景

Muse からのお知らせ（プロアクティブな連絡）を届けるとき、椿が
voice.speak で一言の要約を読み上げる運用が始まる。その ON/OFF と
読み上げ時間帯を、ホスト側ではなく **本体の設定** として持たせ、
本体の画面で変更できるようにする。椿は読み上げ前に新しい照会命令で
設定を確認し、OFF または時間外なら読み上げない。

## やること

1. 設定ストア（muse_settings.c）に、次の3つを既存の load/save の
   流儀で追加する（NVS 永続化）:
   - `notify_speak`（u8、既定 1 = ON）
   - `notify_from`（u16、その日の 0:00 からの分、既定 540 = 9:00）
   - `notify_to`（u16、同、既定 1320 = 22:00）
2. 本体の設定画面（muse_settings_ui.c）に項目を足す。置き場所は
   既存ページの構成に合わせて判断してよい（sound ページが近い）。
   - 「お知らせ読み上げ」の ON/OFF 切り替え
   - 開始時刻と終了時刻の調整（30分刻みの増減でよい）
   - 表記は既存 UI の流儀に合わせる（このビルドは CJK フォント有効）
   - 設定画面の開き方・戻り方は既存のまま変えない
3. 照会命令 `notify.get` を追加する:
   - 返信は既存命令の流儀（成功時は payload の下に
     `{"enabled": true, "from_min": 540, "to_min": 1320}` の形）
   - head.state と同じ「照会系」として扱い、sleep タイマーを
     リセットしない・sleep から起こさない
4. voice.speak の挙動は変えない。

## 制約

- ビルドは /tmp/build_cores3.sh、BUILD_EXIT=0 まで。パーティション表は
  partitions_muse_cores3.csv のまま（sdkconfig の CUSTOM_FILENAME と
  flash_args の 0x830000 srmodels.bin を確認すること）。
- 既存機能（ウェイク検知・TTS・サーボ・LED・sleep・カメラ）は変えない。
- フラッシュしない。push しない。cores3-custom にコミットする。
- 確認範囲は設定ストア＋設定画面＋notify.get＋コンパイルまで。

## 報告フォーマット

1. 追加した設定キーと既定値、保存のされ方
2. 設定画面のどこに何を足したか（開き方に変更があれば、それも）
3. notify.get の返信形と、sleep を起こさない扱いの確認方法
4. コミットハッシュ
5. ビルド結果（BUILD_EXIT）とパーティション表・flash_args の確認結果
6. 書き込み後に確認すべきログ行
