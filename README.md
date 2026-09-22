# zmk-feature-holdtap-config

DYA Studio 向け hold-tap タイミング実行時調整モジュール。
`&mt` / `&lt` の tapping-term / quick-tap / flavor 等を再フラッシュなしで変える。

## 段階

- Phase1: 土台 (本雛形)。Kconfig 登録のみでビルド確認
- Phase2: hook + store (RAM シャドウ + custom-settings 永続化)
- Phase3: RPC (Studio custom RPC)
- Phase4: Studio パネル (専用 UI。なくても AdvancedSettings 汎用 UI で運用可)

## 使い方 (torabo-tsuki 側)

`config/west.yml` に追加:

```yaml
    - name: zmk-feature-holdtap-config
      remote: Daytona0306
      revision: <SHA>  # track: main
```

`snippets/split-central/split-central.conf` に追加:

```ini
CONFIG_ZMK_HOLDTAP_RUNTIME=y
CONFIG_ZMK_HOLDTAP_RUNTIME_STUDIO_RPC=y
```
