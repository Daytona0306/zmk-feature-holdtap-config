/*
 * DYA hold-tap timing runtime — Studio custom subsystem stub登録。
 *
 * custom-settings の LIST は、設定の custom_subsystem_id 文字列と一致する
 * Studio custom subsystem identifier が登録されていない設定を除外する
 * (未登録は -ENOENT で通知スキップ)。そのため Phase2b の 8キーを
 * AdvancedSettings に出すには、本モジュール側で identifier 登録が必須。
 *
 * Phase3 で本物ハンドラ (get/set/rpc) に置き換えるまでの stub。
 * 現状 Studio から本 subsystem への Call は想定しないため false を返す。
 */

#include <stdbool.h>

#include <zmk/studio/custom.h>

static struct zmk_rpc_custom_subsystem_meta dya_holdtap_rpc_meta = {
    ZMK_RPC_CUSTOM_SUBSYSTEM_UI_URLS(),
    .security = ZMK_STUDIO_RPC_HANDLER_UNSECURED,
};

static bool dya_holdtap_rpc_handler(const zmk_custom_CallRequest *req, pb_callback_t *res) {
    ARG_UNUSED(req);
    ARG_UNUSED(res);
    /* Phase3 までの stub。Call されても何もしない (失敗応答)。 */
    return false;
}

ZMK_RPC_CUSTOM_SUBSYSTEM(dya__holdtap, &dya_holdtap_rpc_meta, dya_holdtap_rpc_handler);
