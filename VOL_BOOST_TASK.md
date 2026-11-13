# VOL_BOOST_TASK — スピーカー音量の上限を引き上げる（100% の先をデジタル増幅で足す）

リポジトリ: ~/dev/muse-gadget-sdk（ブランチ cores3-custom、先頭 9540f92）
対象ボード: muse-m5stack-cores3

## 背景

音量設定は 0〜100% で、100% はコーデック（esp_codec_dev_set_out_vol）の
上限いっぱい。マイク感度で入れたデジタル増幅（9540f92）と同じ考え方で、
100% を超えた分を再生側のデジタル増幅で足し、上限を引き上げる。

## やること

1. 音量の上限を 100 → 150 に広げる（muse_settings.h に
   MUSE_VOLUME_MAX=150 を置き、読み込み時に丸める）。
   - 0〜100: 今までどおりコーデック音量。
   - 101〜150: コーデックは 100 に固定し、デジタル増幅
     db = (v-100) × 0.24（150 で +12 dB）を足す。
2. デジタル増幅は muse_audio の再生経路に置く:
   - muse_audio_write() で、モノラルサンプルを s_out_stereo へ
     広げる際に倍率を掛け、int16 の範囲で飽和させる。
   - muse_audio.c 内の他の esp_codec_dev_write の呼び出し
     （310/314/376 行付近）も確認し、スピーカーへ出る音すべてに
     同じ増幅が掛かるようにする（経路ごとに漏れがないこと）。
   - 再生経路に新たなバッファ確保はしない。
3. muse_audio_set_volume でコーデック分とデジタル分を分けて適用し、
   設定変更が再起動なしで効くようにする（マイク側の apply_mic_gain
   の流儀に合わせる）。
4. 設定画面（muse_settings_ui.c の Sound ページ）の音量スライダーの
   上限を 150 にする。表示は % のまま。他の変更経路（muse_menu.c 等）
   も上限に追随すること。
5. 適用時にログを出す: 合計と内訳が分かる形
   （例: volume 130% (codec 100 + digital 7.2 dB)）。

## 制約

- ビルドは /tmp/build_cores3.sh、BUILD_EXIT=0 まで。パーティション表は
  partitions_muse_cores3.csv のまま（sdkconfig の CUSTOM_FILENAME と
  flash_args の 0x830000 srmodels.bin を確認すること）。
- フラッシュしない。push しない。cores3-custom にコミットする。
- 確認範囲は再生経路＋設定＋コンパイルまで。

## 報告フォーマット

1. デジタル増幅を置いた場所と、スピーカー出力すべてに効くと言える理由
   （他の write 呼び出しの扱いを含む）
2. set_volume の分け方と、スライダー範囲の変え方
3. ログ行の形
4. コミットハッシュ
5. ビルド結果（BUILD_EXIT）とパーティション表・flash_args の確認結果
