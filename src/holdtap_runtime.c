/*
 * DYA hold-tap timing runtime — Phase2b: hook + RAM + custom-settings永続化。
 *
 * Phase2a (RAMシャドウ+ダブルバッファ+irq_lock、DT seed、devポインタ同一性、
 * flavor 0-3、Kconfig 8項目) を包含し、以下を追加する:
 *  - ZMK_CUSTOM_SETTING_DEFINE_WITH_CONSTRAINTS ×8 (INT32 + RANGE)
 *    subsys "dya_ht"。Studio汎用 AdvancedSettings に出現する。
 *  - zmk_custom_setting_changed / zmk_custom_settings_initialized 購読。
 *    Studio/app からの書込みは changed で RAMシャドウへ即時反映
 *    (次ストロークから有効。press中はラッチ済み旧値のまま)。
 *    永続値のロード完了は initialized で RAMシャドウへ再読込み。
 *  - set_default は static/BSS 常駐 s_def_vals のみ (ポインタ保持・コピー
 *    しない仕様のためスタック一時変数禁止)。DT seed値を既定として登録し、
 *    未書込み時の有効値=DT実値にする。
 *  - MEMORY運用: 通常書込みは全て ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY。
 *    毎打鍵PERSIST禁止。Flash保存は dya_ht_save() (save_scope) のみ、
 *    破棄は dya_ht_discard() (discard_scope + 再読込み) のみ。
 *
 * CONFIG_ZMK_CUSTOM_SETTINGS=n でもビルド可 (Phase2a相当で動作、永続化なし)。
 *
 * in-tree インスタンスは mt: mod_tap / lt: layer_tap のみ。
 * dev->name 文字列比較は禁止。devポインタ同一性で判別する。
 */

#include <errno.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#if IS_ENABLED(CONFIG_ZMK_CUSTOM_SETTINGS)
#include <cormoran/zmk/custom_settings.h>
#include <zmk/event_manager.h>
#endif

#include "dya_holdtap_runtime.h"

LOG_MODULE_REGISTER(dya_holdtap_runtime, LOG_LEVEL_DBG);

#define DYA_HT_MT_NODE DT_NODELABEL(mt)
#define DYA_HT_LT_NODE DT_NODELABEL(lt)

BUILD_ASSERT(DT_NODE_EXISTS(DYA_HT_MT_NODE), "mt node missing");
BUILD_ASSERT(DT_NODE_EXISTS(DYA_HT_LT_NODE), "lt node missing");

static const struct device *s_mt_dev;
static const struct device *s_lt_dev;

/* RAMシャドウ + ダブルバッファ。liveがresolverの読み出し面。 */
static struct dya_ht_timing s_live[DYA_HT_COUNT];
static struct dya_ht_timing s_staged[DYA_HT_COUNT];

static int dya_ht_slot_of_dev(const struct device *dev) {
    if (dev != NULL) {
        if (dev == s_mt_dev) {
            return DYA_HT_MT;
        }
        if (dev == s_lt_dev) {
            return DYA_HT_LT;
        }
    }
    return -1;
}

static void dya_ht_commit_locked(int slot) {
    unsigned int key = irq_lock();
    s_live[slot] = s_staged[slot];
    irq_unlock(key);
}

static int32_t dya_ht_flavor_from_str(const char *s, int32_t fallback) {
    if (s == NULL) {
        return fallback;
    }
    if (strcmp(s, "hold-preferred") == 0) {
        return DYA_HT_FLAVOR_HOLD_PREFERRED;
    }
    if (strcmp(s, "tap-preferred") == 0) {
        return DYA_HT_FLAVOR_TAP_PREFERRED;
    }
    if (strcmp(s, "balanced") == 0) {
        return DYA_HT_FLAVOR_BALANCED;
    }
    if (strcmp(s, "tap-unless-interrupted") == 0) {
        return DYA_HT_FLAVOR_TAP_UNLESS_INTERRUPTED;
    }
    return fallback; /* 未知値 */
}

/* DT既定seed。prop無ければKconfig既定。 */
static void dya_ht_seed_from_dt(void) {
#if DT_NODE_HAS_PROP(DYA_HT_MT_NODE, tapping_term_ms)
    s_staged[DYA_HT_MT].tapping_term_ms = (int32_t)DT_PROP(DYA_HT_MT_NODE, tapping_term_ms);
#else
    s_staged[DYA_HT_MT].tapping_term_ms = CONFIG_ZMK_HOLDTAP_MT_TAPPING_TERM_MS;
#endif
#if DT_NODE_HAS_PROP(DYA_HT_MT_NODE, quick_tap_ms)
    s_staged[DYA_HT_MT].quick_tap_ms = (int32_t)DT_PROP(DYA_HT_MT_NODE, quick_tap_ms);
#else
    s_staged[DYA_HT_MT].quick_tap_ms = CONFIG_ZMK_HOLDTAP_MT_QUICK_TAP_MS;
#endif
#if DT_NODE_HAS_PROP(DYA_HT_MT_NODE, flavor)
    s_staged[DYA_HT_MT].flavor =
        dya_ht_flavor_from_str(DT_PROP(DYA_HT_MT_NODE, flavor), CONFIG_ZMK_HOLDTAP_MT_FLAVOR);
#else
    s_staged[DYA_HT_MT].flavor = CONFIG_ZMK_HOLDTAP_MT_FLAVOR;
#endif
#if DT_NODE_HAS_PROP(DYA_HT_MT_NODE, require_prior_idle_ms)
    s_staged[DYA_HT_MT].require_prior_idle_ms =
        (int32_t)DT_PROP(DYA_HT_MT_NODE, require_prior_idle_ms);
#else
    s_staged[DYA_HT_MT].require_prior_idle_ms = CONFIG_ZMK_HOLDTAP_MT_REQUIRE_PRIOR_IDLE_MS;
#endif

#if DT_NODE_HAS_PROP(DYA_HT_LT_NODE, tapping_term_ms)
    s_staged[DYA_HT_LT].tapping_term_ms = (int32_t)DT_PROP(DYA_HT_LT_NODE, tapping_term_ms);
#else
    s_staged[DYA_HT_LT].tapping_term_ms = CONFIG_ZMK_HOLDTAP_LT_TAPPING_TERM_MS;
#endif
#if DT_NODE_HAS_PROP(DYA_HT_LT_NODE, quick_tap_ms)
    s_staged[DYA_HT_LT].quick_tap_ms = (int32_t)DT_PROP(DYA_HT_LT_NODE, quick_tap_ms);
#else
    s_staged[DYA_HT_LT].quick_tap_ms = CONFIG_ZMK_HOLDTAP_LT_QUICK_TAP_MS;
#endif
#if DT_NODE_HAS_PROP(DYA_HT_LT_NODE, flavor)
    s_staged[DYA_HT_LT].flavor =
        dya_ht_flavor_from_str(DT_PROP(DYA_HT_LT_NODE, flavor), CONFIG_ZMK_HOLDTAP_LT_FLAVOR);
#else
    s_staged[DYA_HT_LT].flavor = CONFIG_ZMK_HOLDTAP_LT_FLAVOR;
#endif
#if DT_NODE_HAS_PROP(DYA_HT_LT_NODE, require_prior_idle_ms)
    s_staged[DYA_HT_LT].require_prior_idle_ms =
        (int32_t)DT_PROP(DYA_HT_LT_NODE, require_prior_idle_ms);
#else
    s_staged[DYA_HT_LT].require_prior_idle_ms = CONFIG_ZMK_HOLDTAP_LT_REQUIRE_PRIOR_IDLE_MS;
#endif

    s_live[DYA_HT_MT] = s_staged[DYA_HT_MT];
    s_live[DYA_HT_LT] = s_staged[DYA_HT_LT];
}

#if IS_ENABLED(CONFIG_ZMK_CUSTOM_SETTINGS)

/* ---- Phase2b: custom-settings 登録・購読 ---- */

#define DYA_HT_SUBSYS "dya__holdtap"

enum dya_ht_field {
    DYA_HT_F_TAPPING = 0,
    DYA_HT_F_QUICK_TAP = 1,
    DYA_HT_F_FLAVOR = 2,
    DYA_HT_F_IDLE = 3,
    DYA_HT_F_COUNT = 4,
};

static const char *const s_keys[DYA_HT_COUNT][DYA_HT_F_COUNT] = {
    [DYA_HT_MT] =
        {
            "mt_tapping_term_ms",
            "mt_quick_tap_ms",
            "mt_flavor",
            "mt_require_prior_idle_ms",
        },
    [DYA_HT_LT] =
        {
            "lt_tapping_term_ms",
            "lt_quick_tap_ms",
            "lt_flavor",
            "lt_require_prior_idle_ms",
        },
};

/* flavor は Studio でドロップダウン選択させるため OPTIONS制約。
 * 許容値 0-3 は従来の RANGE と同一なので保存値の移行不要。 */
static const struct zmk_custom_setting_value s_flavor_values[] = {
    {.type = ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32, .int32_value = 0},
    {.type = ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32, .int32_value = 1},
    {.type = ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32, .int32_value = 2},
    {.type = ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32, .int32_value = 3},
};
static const char *const s_flavor_labels[] = {
    "hold-preferred (0)",
    "balanced (1)",
    "tap-preferred (2)",
    "tap-unless-interrupted (3)",
};
static const struct zmk_custom_setting_constraint s_flavor_constraint = {
    .type = ZMK_CUSTOM_SETTING_CONSTRAINT_OPTIONS,
    .options = {.values = s_flavor_values, .labels = s_flavor_labels, .count = 4},
};

ZMK_CUSTOM_SETTING_DEFINE_WITH_CONSTRAINTS(
    dya_ht_mt_tapping_term_ms, DYA_HT_SUBSYS, "mt_tapping_term_ms",
    ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32,
    ZMK_CUSTOM_SETTING_VALUE_INT32(CONFIG_ZMK_HOLDTAP_MT_TAPPING_TERM_MS),
    ZMK_CUSTOM_SETTING_CONFIDENTIALITY_RPC_PUBLIC, ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE,
    ZMK_CUSTOM_SETTING_PERMISSION_SECURE, ZMK_CUSTOM_SETTING_RANGE_INT32(10, 1000));
ZMK_CUSTOM_SETTING_DEFINE_WITH_CONSTRAINTS(
    dya_ht_mt_quick_tap_ms, DYA_HT_SUBSYS, "mt_quick_tap_ms",
    ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32,
    ZMK_CUSTOM_SETTING_VALUE_INT32(CONFIG_ZMK_HOLDTAP_MT_QUICK_TAP_MS),
    ZMK_CUSTOM_SETTING_CONFIDENTIALITY_RPC_PUBLIC, ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE,
    ZMK_CUSTOM_SETTING_PERMISSION_SECURE, ZMK_CUSTOM_SETTING_RANGE_INT32(-1, 1000));
ZMK_CUSTOM_SETTING_DEFINE_WITH_CONSTRAINTS(
    dya_ht_mt_flavor, DYA_HT_SUBSYS, "mt_flavor", ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32,
    ZMK_CUSTOM_SETTING_VALUE_INT32(CONFIG_ZMK_HOLDTAP_MT_FLAVOR),
    ZMK_CUSTOM_SETTING_CONFIDENTIALITY_RPC_PUBLIC, ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE,
    ZMK_CUSTOM_SETTING_PERMISSION_SECURE, s_flavor_constraint);
ZMK_CUSTOM_SETTING_DEFINE_WITH_CONSTRAINTS(
    dya_ht_mt_require_prior_idle_ms, DYA_HT_SUBSYS, "mt_require_prior_idle_ms",
    ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32,
    ZMK_CUSTOM_SETTING_VALUE_INT32(CONFIG_ZMK_HOLDTAP_MT_REQUIRE_PRIOR_IDLE_MS),
    ZMK_CUSTOM_SETTING_CONFIDENTIALITY_RPC_PUBLIC, ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE,
    ZMK_CUSTOM_SETTING_PERMISSION_SECURE, ZMK_CUSTOM_SETTING_RANGE_INT32(-1, 1000));
ZMK_CUSTOM_SETTING_DEFINE_WITH_CONSTRAINTS(
    dya_ht_lt_tapping_term_ms, DYA_HT_SUBSYS, "lt_tapping_term_ms",
    ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32,
    ZMK_CUSTOM_SETTING_VALUE_INT32(CONFIG_ZMK_HOLDTAP_LT_TAPPING_TERM_MS),
    ZMK_CUSTOM_SETTING_CONFIDENTIALITY_RPC_PUBLIC, ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE,
    ZMK_CUSTOM_SETTING_PERMISSION_SECURE, ZMK_CUSTOM_SETTING_RANGE_INT32(10, 1000));
ZMK_CUSTOM_SETTING_DEFINE_WITH_CONSTRAINTS(
    dya_ht_lt_quick_tap_ms, DYA_HT_SUBSYS, "lt_quick_tap_ms",
    ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32,
    ZMK_CUSTOM_SETTING_VALUE_INT32(CONFIG_ZMK_HOLDTAP_LT_QUICK_TAP_MS),
    ZMK_CUSTOM_SETTING_CONFIDENTIALITY_RPC_PUBLIC, ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE,
    ZMK_CUSTOM_SETTING_PERMISSION_SECURE, ZMK_CUSTOM_SETTING_RANGE_INT32(-1, 1000));
ZMK_CUSTOM_SETTING_DEFINE_WITH_CONSTRAINTS(
    dya_ht_lt_flavor, DYA_HT_SUBSYS, "lt_flavor", ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32,
    ZMK_CUSTOM_SETTING_VALUE_INT32(CONFIG_ZMK_HOLDTAP_LT_FLAVOR),
    ZMK_CUSTOM_SETTING_CONFIDENTIALITY_RPC_PUBLIC, ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE,
    ZMK_CUSTOM_SETTING_PERMISSION_SECURE, s_flavor_constraint);
ZMK_CUSTOM_SETTING_DEFINE_WITH_CONSTRAINTS(
    dya_ht_lt_require_prior_idle_ms, DYA_HT_SUBSYS, "lt_require_prior_idle_ms",
    ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32,
    ZMK_CUSTOM_SETTING_VALUE_INT32(CONFIG_ZMK_HOLDTAP_LT_REQUIRE_PRIOR_IDLE_MS),
    ZMK_CUSTOM_SETTING_CONFIDENTIALITY_RPC_PUBLIC, ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE,
    ZMK_CUSTOM_SETTING_PERMISSION_SECURE, ZMK_CUSTOM_SETTING_RANGE_INT32(-1, 1000));

/* set_default用既定値。set_defaultはポインタ保持のみ (コピーしない) のため
 * static/BSS常駐必須。スタック一時変数のアドレス渡しは禁止。 */
static struct zmk_custom_setting_value s_def_vals[DYA_HT_COUNT][DYA_HT_F_COUNT];

static int32_t dya_ht_staged_field(int slot, int field) {
    switch (field) {
    case DYA_HT_F_TAPPING:
        return s_staged[slot].tapping_term_ms;
    case DYA_HT_F_QUICK_TAP:
        return s_staged[slot].quick_tap_ms;
    case DYA_HT_F_FLAVOR:
        return s_staged[slot].flavor;
    case DYA_HT_F_IDLE:
        return s_staged[slot].require_prior_idle_ms;
    default:
        return 0;
    }
}

/* DT seed値を実行時既定として登録。settings_load() より前に呼ぶこと
 * (SYS_INITで可)。永続値・書込み済み値がある場合は既定のみ差し替え。 */
static void dya_ht_install_defaults(void) {
    for (int slot = 0; slot < DYA_HT_COUNT; slot++) {
        for (int field = 0; field < DYA_HT_F_COUNT; field++) {
            s_def_vals[slot][field].type = ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32;
            s_def_vals[slot][field].int32_value = dya_ht_staged_field(slot, field);
            const struct zmk_custom_setting *st =
                zmk_custom_setting_find(DYA_HT_SUBSYS, s_keys[slot][field]);
            if (st == NULL) {
                LOG_WRN("dya_ht find %s failed", s_keys[slot][field]);
                continue;
            }
            int rc = zmk_custom_setting_set_default(st, &s_def_vals[slot][field]);
            if (rc < 0) {
                LOG_WRN("dya_ht set_default %s failed: %d", s_keys[slot][field], rc);
            }
        }
    }
}

static bool dya_ht_lookup_key(const char *key, int *slot_out, int *field_out) {
    if (key == NULL) {
        return false;
    }
    for (int slot = 0; slot < DYA_HT_COUNT; slot++) {
        for (int field = 0; field < DYA_HT_F_COUNT; field++) {
            if (strcmp(key, s_keys[slot][field]) == 0) {
                if (slot_out != NULL) {
                    *slot_out = slot;
                }
                if (field_out != NULL) {
                    *field_out = field;
                }
                return true;
            }
        }
    }
    return false;
}

/* store値をRAMシャドウへ反映 (範囲検査つきcommit)。 */
static int dya_ht_apply_value(int slot, int field, int32_t v) {
    if (slot < 0 || slot >= DYA_HT_COUNT || field < 0 || field >= DYA_HT_F_COUNT) {
        return -EINVAL;
    }
    switch (field) {
    case DYA_HT_F_TAPPING:
        if (v < 10 || v > 1000) {
            return -ERANGE;
        }
        s_staged[slot].tapping_term_ms = v;
        break;
    case DYA_HT_F_QUICK_TAP:
        if (v < -1 || v > 1000) {
            return -ERANGE;
        }
        s_staged[slot].quick_tap_ms = v;
        break;
    case DYA_HT_F_FLAVOR:
        if (v < DYA_HT_FLAVOR_HOLD_PREFERRED || v > DYA_HT_FLAVOR_TAP_UNLESS_INTERRUPTED) {
            return -EINVAL;
        }
        s_staged[slot].flavor = v;
        break;
    case DYA_HT_F_IDLE:
        if (v < -1 || v > 1000) {
            return -ERANGE;
        }
        s_staged[slot].require_prior_idle_ms = v;
        break;
    default:
        return -EINVAL;
    }
    dya_ht_commit_locked(slot);
    return 0;
}

/* 8キー全読込み。initialized時とdiscard後に呼ぶ。 */
static void dya_ht_reload_all(void) {
    for (int slot = 0; slot < DYA_HT_COUNT; slot++) {
        for (int field = 0; field < DYA_HT_F_COUNT; field++) {
            struct zmk_custom_setting_value v;
            int rc = zmk_custom_setting_read_by_key(DYA_HT_SUBSYS, s_keys[slot][field], &v);
            if (rc < 0) {
                LOG_WRN("dya_ht reload %s read failed: %d", s_keys[slot][field], rc);
                continue;
            }
            if (v.type != ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32) {
                LOG_WRN("dya_ht reload %s bad type %d", s_keys[slot][field], (int)v.type);
                continue;
            }
            rc = dya_ht_apply_value(slot, field, v.int32_value);
            if (rc < 0) {
                /* 永続値が範囲外の場合はRAM seedを維持。fail-closed。 */
                LOG_WRN("dya_ht reload %s out of range %d", s_keys[slot][field],
                        (int)v.int32_value);
            }
        }
    }
}

/* changed: 自キーなら値をRAMへ反映 (全kind一律。SAVED時も同値のため無害)。
 * initialized: 起動ロード完了後に全8キーをRAMへ反映。
 * listener内ではwrite系を呼ばない (再入防止)。 */
static int dya_ht_settings_listener(const zmk_event_t *eh) {
    const struct zmk_custom_setting_changed *ch = as_zmk_custom_setting_changed(eh);
    if (ch != NULL) {
        if (ch->setting == NULL || ch->setting->key == NULL) {
            return ZMK_EV_EVENT_BUBBLE;
        }
        if (ch->setting->custom_subsystem_id == NULL ||
            strcmp(ch->setting->custom_subsystem_id, DYA_HT_SUBSYS) != 0) {
            return ZMK_EV_EVENT_BUBBLE;
        }
        int slot = -1, field = -1;
        if (!dya_ht_lookup_key(ch->setting->key, &slot, &field)) {
            return ZMK_EV_EVENT_BUBBLE;
        }
        struct zmk_custom_setting_value v;
        if (zmk_custom_setting_read(ch->setting, &v) < 0) {
            return ZMK_EV_EVENT_BUBBLE;
        }
        if (v.type != ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32) {
            return ZMK_EV_EVENT_BUBBLE;
        }
        int rc = dya_ht_apply_value(slot, field, v.int32_value);
        if (rc < 0) {
            LOG_WRN("dya_ht apply %s=%d rejected: %d", ch->setting->key, (int)v.int32_value,
                    rc);
        }
        return ZMK_EV_EVENT_BUBBLE;
    }
    if (as_zmk_custom_settings_initialized(eh) != NULL) {
        dya_ht_reload_all();
        LOG_INF("dya_ht initialized: mt tap=%d quick=%d flav=%d idle=%d | lt tap=%d quick=%d "
                "flav=%d idle=%d",
                (int)s_live[DYA_HT_MT].tapping_term_ms, (int)s_live[DYA_HT_MT].quick_tap_ms,
                (int)s_live[DYA_HT_MT].flavor, (int)s_live[DYA_HT_MT].require_prior_idle_ms,
                (int)s_live[DYA_HT_LT].tapping_term_ms, (int)s_live[DYA_HT_LT].quick_tap_ms,
                (int)s_live[DYA_HT_LT].flavor, (int)s_live[DYA_HT_LT].require_prior_idle_ms);
    }
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(dya_ht_runtime, dya_ht_settings_listener);
ZMK_SUBSCRIPTION(dya_ht_runtime, zmk_custom_setting_changed);
ZMK_SUBSCRIPTION(dya_ht_runtime, zmk_custom_settings_initialized);

#endif /* IS_ENABLED(CONFIG_ZMK_CUSTOM_SETTINGS) */

int dya_ht_get(int slot, struct dya_ht_timing *out) {
    if (out == NULL || slot < 0 || slot >= DYA_HT_COUNT) {
        return -EINVAL;
    }
    unsigned int key = irq_lock();
    *out = s_live[slot];
    irq_unlock(key);
    return 0;
}

const struct device *dya_ht_dev(int slot) {
    if (slot == DYA_HT_MT) {
        return s_mt_dev;
    }
    if (slot == DYA_HT_LT) {
        return s_lt_dev;
    }
    return NULL;
}

/* set_*: 先にstoreへMEMORY書込み (制約はstore側でも再検証) し、成功時のみ
 * RAMへcommit。custom-settings無効時はRAMのみ (Phase2a動作)。 */
int dya_ht_set_tapping(int slot, int32_t ms) {
    if (slot < 0 || slot >= DYA_HT_COUNT) {
        return -EINVAL;
    }
    if (ms < 10 || ms > 1000) {
        return -ERANGE;
    }
#if IS_ENABLED(CONFIG_ZMK_CUSTOM_SETTINGS)
    struct zmk_custom_setting_value v = {.type = ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32,
                                         .int32_value = ms};
    int rc = zmk_custom_setting_write_by_key(DYA_HT_SUBSYS, s_keys[slot][DYA_HT_F_TAPPING], &v,
                                             ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY);
    if (rc < 0) {
        LOG_WRN("dya_ht write %s=%d failed: %d", s_keys[slot][DYA_HT_F_TAPPING], (int)ms, rc);
        return rc;
    }
    return dya_ht_apply_value(slot, DYA_HT_F_TAPPING, ms);
#else
    s_staged[slot].tapping_term_ms = ms;
    dya_ht_commit_locked(slot);
    return 0;
#endif
}

int dya_ht_set_quick_tap(int slot, int32_t ms) {
    if (slot < 0 || slot >= DYA_HT_COUNT) {
        return -EINVAL;
    }
    if (ms < -1 || ms > 1000) {
        return -ERANGE;
    }
#if IS_ENABLED(CONFIG_ZMK_CUSTOM_SETTINGS)
    struct zmk_custom_setting_value v = {.type = ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32,
                                         .int32_value = ms};
    int rc = zmk_custom_setting_write_by_key(DYA_HT_SUBSYS, s_keys[slot][DYA_HT_F_QUICK_TAP], &v,
                                             ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY);
    if (rc < 0) {
        LOG_WRN("dya_ht write %s=%d failed: %d", s_keys[slot][DYA_HT_F_QUICK_TAP], (int)ms, rc);
        return rc;
    }
    return dya_ht_apply_value(slot, DYA_HT_F_QUICK_TAP, ms);
#else
    s_staged[slot].quick_tap_ms = ms;
    dya_ht_commit_locked(slot);
    return 0;
#endif
}

int dya_ht_set_flavor(int slot, int32_t flavor) {
    if (slot < 0 || slot >= DYA_HT_COUNT) {
        return -EINVAL;
    }
    if (flavor < DYA_HT_FLAVOR_HOLD_PREFERRED || flavor > DYA_HT_FLAVOR_TAP_UNLESS_INTERRUPTED) {
        return -EINVAL;
    }
#if IS_ENABLED(CONFIG_ZMK_CUSTOM_SETTINGS)
    struct zmk_custom_setting_value v = {.type = ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32,
                                         .int32_value = flavor};
    int rc = zmk_custom_setting_write_by_key(DYA_HT_SUBSYS, s_keys[slot][DYA_HT_F_FLAVOR], &v,
                                             ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY);
    if (rc < 0) {
        LOG_WRN("dya_ht write %s=%d failed: %d", s_keys[slot][DYA_HT_F_FLAVOR], (int)flavor,
                rc);
        return rc;
    }
    return dya_ht_apply_value(slot, DYA_HT_F_FLAVOR, flavor);
#else
    s_staged[slot].flavor = flavor;
    dya_ht_commit_locked(slot);
    return 0;
#endif
}

int dya_ht_set_require_prior_idle(int slot, int32_t ms) {
    if (slot < 0 || slot >= DYA_HT_COUNT) {
        return -EINVAL;
    }
    if (ms < -1 || ms > 1000) {
        return -ERANGE;
    }
#if IS_ENABLED(CONFIG_ZMK_CUSTOM_SETTINGS)
    struct zmk_custom_setting_value v = {.type = ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32,
                                         .int32_value = ms};
    int rc = zmk_custom_setting_write_by_key(DYA_HT_SUBSYS, s_keys[slot][DYA_HT_F_IDLE], &v,
                                             ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY);
    if (rc < 0) {
        LOG_WRN("dya_ht write %s=%d failed: %d", s_keys[slot][DYA_HT_F_IDLE], (int)ms, rc);
        return rc;
    }
    return dya_ht_apply_value(slot, DYA_HT_F_IDLE, ms);
#else
    s_staged[slot].require_prior_idle_ms = ms;
    dya_ht_commit_locked(slot);
    return 0;
#endif
}

/* 明示保存パス (MEMORY運用のため永続化はここからのみ)。PERSIST直接書込みは
 * 行わない。custom-settings無効時は -ENOSYS。 */
int dya_ht_save(void) {
#if IS_ENABLED(CONFIG_ZMK_CUSTOM_SETTINGS)
    uint32_t affected = 0;
    int rc = zmk_custom_settings_save_scope(DYA_HT_SUBSYS, NULL, NULL, &affected);
    LOG_INF("dya_ht save rc=%d affected=%u", rc, (unsigned int)affected);
    return rc;
#else
    return -ENOSYS;
#endif
}

int dya_ht_discard(void) {
#if IS_ENABLED(CONFIG_ZMK_CUSTOM_SETTINGS)
    uint32_t affected = 0;
    int rc = zmk_custom_settings_discard_scope(DYA_HT_SUBSYS, NULL, NULL, &affected);
    LOG_INF("dya_ht discard rc=%d affected=%u", rc, (unsigned int)affected);
    if (rc == 0) {
        dya_ht_reload_all();
    }
    return rc;
#else
    return -ENOSYS;
#endif
}

bool dya_ht_resolve(const struct device *dev, int32_t *tapping, int32_t *quick,
                    int32_t *flavor, int32_t *idle) {
    int slot = dya_ht_slot_of_dev(dev);
    if (slot < 0) {
        return false; /* mt/lt 以外は DT のまま */
    }
    unsigned int key = irq_lock();
    struct dya_ht_timing cur = s_live[slot];
    irq_unlock(key);
    if (tapping != NULL) {
        *tapping = cur.tapping_term_ms;
    }
    if (quick != NULL) {
        *quick = cur.quick_tap_ms;
    }
    if (flavor != NULL) {
        *flavor = cur.flavor;
    }
    if (idle != NULL) {
        *idle = cur.require_prior_idle_ms;
    }
    return true;
}

static int dya_holdtap_runtime_init(void) {
    s_mt_dev = DEVICE_DT_GET(DYA_HT_MT_NODE);
    s_lt_dev = DEVICE_DT_GET(DYA_HT_LT_NODE);
    if (!device_is_ready(s_mt_dev)) {
        LOG_WRN("mt device not ready");
    }
    if (!device_is_ready(s_lt_dev)) {
        LOG_WRN("lt device not ready");
    }
    dya_ht_seed_from_dt();
#if IS_ENABLED(CONFIG_ZMK_CUSTOM_SETTINGS)
    dya_ht_install_defaults();
#endif
    LOG_INF("dya_ht init: mt tap=%d quick=%d flav=%d idle=%d | lt tap=%d quick=%d flav=%d idle=%d",
            (int)s_live[DYA_HT_MT].tapping_term_ms, (int)s_live[DYA_HT_MT].quick_tap_ms,
            (int)s_live[DYA_HT_MT].flavor, (int)s_live[DYA_HT_MT].require_prior_idle_ms,
            (int)s_live[DYA_HT_LT].tapping_term_ms, (int)s_live[DYA_HT_LT].quick_tap_ms,
            (int)s_live[DYA_HT_LT].flavor, (int)s_live[DYA_HT_LT].require_prior_idle_ms);
    return 0;
}

SYS_INIT(dya_holdtap_runtime_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
