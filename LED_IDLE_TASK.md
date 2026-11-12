# LED_IDLE_TASK — 待機中は土台 LED を消す

リポジトリ: ~/dev/muse-gadget-sdk（ブランチ cores3-custom、先頭 e8c1411）
対象ボード: muse-m5stack-cores3

## ユーザーの希望

- 待機中（idle）は土台 LED を点けっぱなしにしない。消灯にする。
- 聞き取り・考え中・発話・エラーなど、状態が動いているときの
  色の連動は今までどおり残す。

## やること

- muse_led.c の状態連動で、idle（と起動直後の boot 相当）の色を
  消灯（rgb 0,0,0）に変える。現在は暖色 (255,150,70)×0.30。
- 他の状態の色は変えない: listening = 緑、thinking = 青、
  speaking = 暖色の呼吸、error = 赤、sleep/off = 消灯。
- 手動の led.set / led.off の仕様は変えない（idle で led.set した
  色は、次の状態変化まで残る現行仕様のまま）。

## 制約

- ビルドは /tmp/build_cores3.sh、BUILD_EXIT=0 まで。パーティション表は
  partitions_muse_cores3.csv のまま（sdkconfig の CUSTOM_FILENAME と
  flash_args の 0x830000 srmodels.bin を確認すること）。
- フラッシュしない。push しない。cores3-custom にコミットする。
- 確認範囲は LED の状態連動＋コンパイルまで。

## 報告フォーマット

1. 変えた箇所
2. コミットハッシュ
3. ビルド結果（BUILD_EXIT）とパーティション表・flash_args の確認結果
4. 書き込み後に確認すべきログ行
