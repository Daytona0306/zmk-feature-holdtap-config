/*
 * DYA hold-tap timing runtime — Phase2a: hook + RAM のみ。
 * custom-settings 不要 (CONFIG_ZMK_CUSTOM_SETTINGS=n でもビルド可)。
 * Phase2b で DEFINE×8 + イベント購読 + 永続化を追加する。
 *
 * in-tree インスタンスは mt: mod_tap / lt: layer_tap のみ。
 * dev->name 文字列比較は禁止。devポインタ同一性で判別する。
 */

#include <string.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

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

/* DT既定seed。prop無ければKconfig既定。idleのDT prop名は未確定のためKconfig値。 */
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
    s_staged[DYA_HT_MT].require_prior_idle_ms = CONFIG_ZMK_HOLDTAP_MT_REQUIRE_PRIOR_IDLE_MS;

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
    s_staged[DYA_HT_LT].require_prior_idle_ms = CONFIG_ZMK_HOLDTAP_LT_REQUIRE_PRIOR_IDLE_MS;

    s_live[DYA_HT_MT] = s_staged[DYA_HT_MT];
    s_live[DYA_HT_LT] = s_staged[DYA_HT_LT];
}

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

int dya_ht_set_tapping(int slot, int32_t ms) {
    if (slot < 0 || slot >= DYA_HT_COUNT) {
        return -EINVAL;
    }
    /* Phase2b で RANGE (10..1000) 検査を custom-settings 制約に寄せる。暫定で同範囲。 */
    if (ms < 10 || ms > 1000) {
        return -ERANGE;
    }
    s_staged[slot].tapping_term_ms = ms;
    dya_ht_commit_locked(slot);
    return 0;
}

int dya_ht_set_quick_tap(int slot, int32_t ms) {
    if (slot < 0 || slot >= DYA_HT_COUNT) {
        return -EINVAL;
    }
    if (ms < -1 || ms > 1000) {
        return -ERANGE;
    }
    s_staged[slot].quick_tap_ms = ms;
    dya_ht_commit_locked(slot);
    return 0;
}

int dya_ht_set_flavor(int slot, int32_t flavor) {
    if (slot < 0 || slot >= DYA_HT_COUNT) {
        return -EINVAL;
    }
    if (flavor < DYA_HT_FLAVOR_HOLD_PREFERRED || flavor > DYA_HT_FLAVOR_TAP_UNLESS_INTERRUPTED) {
        return -EINVAL;
    }
    s_staged[slot].flavor = flavor;
    dya_ht_commit_locked(slot);
    return 0;
}

int dya_ht_set_require_prior_idle(int slot, int32_t ms) {
    if (slot < 0 || slot >= DYA_HT_COUNT) {
        return -EINVAL;
    }
    if (ms < -1 || ms > 1000) {
        return -ERANGE;
    }
    s_staged[slot].require_prior_idle_ms = ms;
    dya_ht_commit_locked(slot);
    return 0;
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
    LOG_INF("dya_ht init: mt tap=%d quick=%d flav=%d idle=%d | lt tap=%d quick=%d flav=%d idle=%d",
            (int)s_live[DYA_HT_MT].tapping_term_ms, (int)s_live[DYA_HT_MT].quick_tap_ms,
            (int)s_live[DYA_HT_MT].flavor, (int)s_live[DYA_HT_MT].require_prior_idle_ms,
            (int)s_live[DYA_HT_LT].tapping_term_ms, (int)s_live[DYA_HT_LT].quick_tap_ms,
            (int)s_live[DYA_HT_LT].flavor, (int)s_live[DYA_HT_LT].require_prior_idle_ms);
    return 0;
}

SYS_INIT(dya_holdtap_runtime_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
