# LED_FIX_TASK — 土台LEDが点かない原因の切り分け診断（led.diag）を追加する

リポジトリ: ~/dev/muse-gadget-sdk（ブランチ cores3-custom、先頭 9d25003）
対象ボード: muse-m5stack-cores3

## 実機で分かっていること

- 起動ログでは初期化がすべて通っている: BOOST_EN high (ESP_OK)、
  LED pin (bit13) の MODE/OUT/OD 読み戻し正常、LED_CFG は
  「wrote 0x0c, read back 0x0c (count took)」。
- led.set / led.off の命令も届いており、書き込みログ
  （`manual: rgb 0,0,64 (rgb565 0x0008)` 等）も出ている。
- しかし土台前面の LED 列（12個）は点灯していない（ユーザーが実機で
  確認）。ユーザーが「赤い」と言っていたのは土台背面の充電インジケータ
  （ハードウェア側のランプ）で、本件とは無関係と判明した。
- 候補（9d25003 の報告でも挙がっていたもの）:
  1. LED_CFG のリフレッシュ bit（bit6、0x40）を立てないと RAM の内容が
     列へ反映されない。
  2. LED データピンの番号付けが bit13 ではなく bit14。
  3. BOOST_EN のピン違い。

## やること

診断命令 `led.diag` を追加する。実行すると、候補の設定を順に切り替え、
各ステップで12個すべてを白色（明るさ上限いっぱい）にし、数秒保って
ログにステップ名を出す。ユーザーが実機を見て、どのステップで点いたか
を教えてくれる。

- ステップ例（順序はこの通り）:
  - A: 現行（データ pin bit13、LED_CFG 0x0c）
  - B: pin bit13、LED_CFG 0x4c（リフレッシュ bit あり）
  - C: pin bit14、LED_CFG 0x0c
  - D: pin bit14、LED_CFG 0x4c
  - 各ステップ 4 秒。開始と終了もログに出す（例: `led.diag step B:
    pin=14 cfg=0x4c`）。色は白 (255,255,255)。
  - B と D では、リフレッシュ bit を立てた直後に 0x0c へ戻す
    （トグル）も試す場合は、その旨をログで分かるようにする。
- 終了後は、診断前の表示（手動色があればそれ、なければ状態連動の色）
  へ戻す。戻せない場合は消灯する。
- 診断中は他の led 命令を busy にする。サーボや他の機能には触らない。
- あわせて、もし M5IOE1 の LED に関する確実な資料（リポジトリ内の
  他ボードのコード、ESPHome の m5ioe1 実装の知識など）から
  候補の優先度や別の候補が分かる場合は、ステップへ反映し、
  報告に根拠を書く。

## 制約

- ビルドは /tmp/build_cores3.sh、BUILD_EXIT=0 まで。パーティション表は
  partitions_muse_cores3.csv のまま（sdkconfig の CUSTOM_FILENAME と
  flash_args の 0x830000 srmodels.bin を確認すること）。
- フラッシュしない。push しない。cores3-custom にコミットする。
- 既存の led.set / led.off / 状態連動の挙動は変えない。
  確認範囲は led.diag の追加＋コンパイルまで。

## 報告フォーマット

1. 追加したステップの一覧と、各ステップで変えているもの
2. 候補の根拠や優先度について分かったこと（あれば）
3. コミットハッシュ
4. ビルド結果（BUILD_EXIT）とパーティション表・flash_args の確認結果
5. 書き込み後に確認すべきログ行
