# Phase2 計画書 (hook + store)

対象: `Daytona0306/zmk-feature-holdtap-config` + `cormoran/zmk (main+dya)` 最小hook。
前提: researcher確定事実 (custom_settings.h L41-45/519-524/793/837/855/930、scope複数形 L901-907、
イベント `ZMK_EVENT_DECLARE` 生成、hold_tap L610-611/L619-620/init L856、instanceはmt/mod_tapとlt/layer_tapのみ)。
checker訂正反映: includeは `<cormoran/zmk/custom_settings.h>`、config構造体は
`int32_t len + int32_t[]` フレキシブル配列 (size_t+ポインタ扱い禁止)、`dev->name` 比較禁止
(devポインタ同一性のみ)、行番号は目安扱い。

## 1. 本体改変の最小性

変更は `cormoran/zmk` の `app/src/behaviors/behavior_hold_tap.c` のみ、約15行。
内容: weak `dya_ht_resolve()` 宣言 + press時 (undecidedガード後・store_hold_tap直前) に
effective値ラッチ +
timer残量計算を `hold_tap->config` (ラッチ済み) 参照に寄せる + init報告ログ。
`Kconfig=n` (CONFIG_ZMK_HOLDTAP_RUNTIME未定義) では `#else` 旧式と同一のため動作同一。
kscan/debounce は本Phaseでは触らない。

## 2. 段階分け

### Phase2a (hook + RAMのみ、custom-settings不要)
- 含む: `include/dya_holdtap_runtime.h`、RAMシャドウ+ダブルバッファ+irq_lock commit、
  DT既定seed、get/set RAM fallback、cormoran patch、Kconfig 8項目。
- 除外: `ZMK_CUSTOM_SETTING_DEFINE_WITH_CONSTRAINTS`、イベント購読、永続化。
- 終了条件: `CONFIG_ZMK_HOLDTAP_RUNTIME=y` + `CONFIG_ZMK_CUSTOM_SETTINGS=n` で
  `west build -s app -b bmp_boost -- -DSHIELD=torabo_tsuki_lp_right` が通ること。

### Phase2b (永続化)
- Phase2a + DEFINE×8 (INT32+RANGE) + イベント購読
  (`as_zmk_custom_setting_changed/initialized`) + `read_by_key/write_by_key` +
  scope保存は複数形 `zmk_custom_settings_save_scope/discard_scope` を明示保存パスのみで呼ぶ。
- 要 `CONFIG_ZMK_CUSTOM_SETTINGS=y` (Phase2b のみ。Kconfig側は `depends on` 等で要求し、
  Phase2a の `select` は付けない)。`set_default` には static/BSS のアドレスのみ渡す
  (スタック一時変数禁止)。毎打鍵PERSIST禁止 (MEMORY運用+明示Save)。
- 終了条件: Phase2b conf でビルドが通り、Studio汎用 AdvancedSettings に
  `dya_ht` 8キーが出現すること。

## 3. ロールバック

- `CONFIG_ZMK_HOLDTAP_RUNTIME=n`: モジュールCMakeで除外 (既存guard維持)、
  本体patchは `#else` 旧式のみで旧動作と同一、DYAログなし。
- DT既定フォールバック: 未書込み時の有効値はDT実値 (report/init seed)。
  `settings_reset` でDT既定に戻る。

## 4. テスト計画

| # | 項目 | 手順 | 期待 |
|---|---|---|---|
| T1 | DT seed | Phase2a起動、ログ確認 | mt/lt=180/300/1/-1 |
| T2 | MT/LT分離 | MTのみ250ms変更 | LTは180msのまま |
| T3 | pressラッチ | 長押し中に値変更 | 当該ストロークは旧値、次から新値 |
| T4 | timer残量 | T3中にexpiry発火 | ラッチ値通り、クラッシュなし |
| T5 | 範囲外 | flavor 9書込み | -EINVALで拒否、旧値保持 |
| T6 | ロールバック+peripheral | nでビルド・キー動作+peripheral側にtiming無混入確認 | 旧動作同一 |
| T7 | 永続化 | Phase2bで変更→再起動 | 値維持 (T8と併せて確認) |
| T8 | 汎用UI | AdvancedSettingsで8キー読書き | 再起動なしで次ストロークから反映 |
| T9 | 同時押し | MT+LT同時press中に片方変更 | もう片方に干渉なし |

運用整合: Studioで変えた値はリポジトリに残らない (TODO既知)。確定値はDT/Kconfig既定へ書き戻す。

## 5. リスク対処

- Zephyr 4.1/HWMv2差分: hook位置はシンボル名基準で行番号に依存しない。
  kscan path未検証のため本Phaseでは触らない。
- split: hold-tap判定はcentralのみ。peripheralへtiming決定コードを載せない。
  `.conf` はcentral側snippet (`split-central.conf`) のみに置く。
- RAM: scalar 8件は数十B。positionsは本Phase対象外 (flexible配列に触らない)。
  RPC stagingをstackに置かない (runtime-macro前例の `K_ERR_STACK_CHK_FAIL` 回避)。
  `SYSTEM_WQ/STUDIO_RPC/LOW_PRIO` は現値維持し、`DEVTOOL_STACK_USAGE` で実測する。
- `feat/firmware-memory-reduction` との関係: 本Phaseではgate追加なし。
  LARGE/ARRAY等の同時変更はしない。

## 6. ファイル一覧 (予定)

- `holdtap/include/dya_holdtap_runtime.h` (新規、公開API)
- `holdtap/src/holdtap_runtime.c` (Phase2a: stub→RAM版、Phase2b: DEFINE+購読追加)
- `holdtap/Kconfig` (8項目追加: tapping 180、quick 300、flavor 1、idle -1)
- `holdtap/CMakeLists.txt` (変更なし、guard済み)
- cormoran/zmk側: `app/src/behaviors/behavior_hold_tap.c` のみ (+15行、guard付き)
- torabo側: `snippets/split-central/split-central.conf` (central側のみ。peripheral側には置かない) に2行
  (`CONFIG_ZMK_HOLDTAP_RUNTIME=y`, `CONFIG_ZMK_HOLDTAP_RUNTIME_STUDIO_RPC=y`)
