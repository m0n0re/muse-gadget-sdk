# LED_FIX2_TASK — M5IOE1 のレジスタ番地の誤りと BUS_OUT_EN の不足を直す

リポジトリ: ~/dev/muse-gadget-sdk（ブランチ cores3-custom、先頭 4ad0b29）
対象ボード: muse-m5stack-cores3

## 公式資料で確定した事実（M5IOE1 データシート / M5Stack 公式 ESPHome 文書）

1. **LED_CFG はレジスタ 0x24**（現行実装は 0x30 に書いていて誤り）。
   ビット配置: bit6 = REFRESH、bit5〜0 = LED の個数。
2. **LED_RAM は 0x30 から**（現行実装は 0x40 から書いていて誤り）。
   1 個あたり 2 バイトの RGB565。最大 32 個（0x30〜0x6F）。
   → 現行は「個数」を RAM[0] に書き、色を RAM の途中から書いていたため、
   個数が設定されず点かなかった、という説明が付く。
3. Neopixel 出力は IO14（レジスタのビットでは bit13）。データシートでも
   IO14 に Neopixel の注記がある。M5IOE1 の pin 14 は有効に保つ
   （ESPHome 公式例: m5ioe1 number 13、output、RESTORE_DEFAULT_ON）。
4. **土台 LED の 5V は BOOST_EN と BUS_OUT_EN の両方が必要**
   （ESPHome 公式文書の RGB Light Bar の項）。どちらも CoreS3 側
   AW9523B の GPIO:
   - BOOST_EN = number 15（現行で設定済み）
   - **BUS_OUT_EN = number 1（現行では未設定）**
   - 順序: BOOST_EN を先に有効にしてから BUS_OUT_EN。
   - 公式の工場設定では BUS_OUT_EN は RESTORE_DEFAULT_ON（常時オン）。

## やること

1. muse_led.c のレジスタ番地を直す:
   - LED_CFG → 0x24。初期化時に個数 12（0x0c）を書き、読み戻してログ。
   - LED_RAM → 0x30 から 2 バイト×12。
   - 色の書き込み後は REFRESH を立てる（0x24 に 0x4c）。REFRESH が
     自己クリア型かどうかは読み戻しでログに残す。
   - led.diag のステップも新しい番地に合わせて直す（cfg の値の意味は
     同じ。トグルのステップも 0x24 で行う）。
2. LED 初期化で BUS_OUT_EN（AW9523B number 1）を BOOST_EN の後に
   HIGH にする。変更前に AW9523B の該当ピンの状態を読み、ログに残す。
   BSP が既にオンにしている場合はその旨が分かるようにする。
3. 成功の判定はこちらで実機確認する: 起動後の状態色の点灯、
   led.set の色変化、led.off、声かけ→発話の連動。

## 制約

- ビルドは /tmp/build_cores3.sh、BUILD_EXIT=0 まで。パーティション表は
  partitions_muse_cores3.csv のまま（sdkconfig の CUSTOM_FILENAME と
  flash_args の 0x830000 srmodels.bin を確認すること）。
- サーボ（VM EN、IO14 以外の IOE ピン）や既存の LED 命令の仕様は変えない。
- フラッシュしない。push しない。cores3-custom にコミットする。
- 確認範囲は LED の初期化と書き込み経路＋コンパイルまで。

## 報告フォーマット

1. 直した箇所（レジスタ番地、BUS_OUT_EN の追加、REFRESH の扱い）
2. AW9523B 側の変更前後の状態（ログで分かる形）
3. コミットハッシュ
4. ビルド結果（BUILD_EXIT）とパーティション表・flash_args の確認結果
5. 書き込み後に確認すべきログ行
