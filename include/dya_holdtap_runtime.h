#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/device.h>

/* MT/LT スロット。devポインタ同一性で解決する。dev->name 比較禁止。 */
enum dya_ht_slot {
    DYA_HT_MT = 0,
    DYA_HT_LT = 1,
    DYA_HT_COUNT = 2,
};

enum dya_ht_flavor {
    DYA_HT_FLAVOR_HOLD_PREFERRED = 0,
    DYA_HT_FLAVOR_BALANCED = 1,
    DYA_HT_FLAVOR_TAP_PREFERRED = 2,
    DYA_HT_FLAVOR_TAP_UNLESS_INTERRUPTED = 3,
};

struct dya_ht_timing {
    int32_t tapping_term_ms;
    int32_t quick_tap_ms; /* -1 = 無効 */
    int32_t flavor;       /* 0..3 */
    int32_t require_prior_idle_ms; /* -1 = 無効 */
};

int dya_ht_get(int slot, struct dya_ht_timing *out);
const struct device *dya_ht_dev(int slot);

int dya_ht_set_tapping(int slot, int32_t ms);
int dya_ht_set_quick_tap(int slot, int32_t ms);
int dya_ht_set_flavor(int slot, int32_t flavor);
int dya_ht_set_require_prior_idle(int slot, int32_t ms);

/* cormoran behavior_hold_tap.c から press 時に呼ばれる。
 * true で 4値を値コピー (ラッチ)。false で dev->config に fallback。 */
bool dya_ht_resolve(const struct device *dev, int32_t *tapping, int32_t *quick,
                    int32_t *flavor, int32_t *idle);
