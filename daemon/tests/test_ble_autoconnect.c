/* SPDX-License-Identifier: GPL-3.0-or-later */

#include <glib.h>
#include <string.h>

#include "airpods_state.h"
#include "ble_autoconnect.h"

static void build_advertisement(uint16_t model, uint8_t status, uint8_t data[27])
{
    memset(data, 0, 27);
    data[0] = 0x07;
    data[1] = 25;
    data[2] = 0x01;
    data[3] = (uint8_t)(model >> 8);
    data[4] = (uint8_t)model;
    data[5] = status;
}

static void test_parse_max_2(void)
{
    uint8_t data[27];
    BleAirPodsAdvertisement advertisement;

    build_advertisement(AIRPODS_MODEL_MAX_2, 0x20, data);
    g_assert_true(ble_airpods_parse_manufacturer_data(
        data, sizeof(data), &advertisement));
    g_assert_cmphex(advertisement.model, ==, AIRPODS_MODEL_MAX_2);
    g_assert_false(advertisement.worn);

    build_advertisement(AIRPODS_MODEL_MAX_2, 0x22, data);
    g_assert_true(ble_airpods_parse_manufacturer_data(
        data, sizeof(data), &advertisement));
    g_assert_true(advertisement.worn);

    build_advertisement(AIRPODS_MODEL_MAX_2, 0x28, data);
    g_assert_true(ble_airpods_parse_manufacturer_data(
        data, sizeof(data), &advertisement));
    g_assert_true(advertisement.worn);
}

static void test_parse_airpods_4(void)
{
    uint8_t data[27];
    BleAirPodsAdvertisement advertisement;

    build_advertisement(AIRPODS_MODEL_4_ANC, 0x02, data);
    g_assert_true(ble_airpods_parse_manufacturer_data(
        data, sizeof(data), &advertisement));
    g_assert_cmphex(advertisement.model, ==, AIRPODS_MODEL_4_ANC);
    g_assert_true(advertisement.worn);

    build_advertisement(AIRPODS_MODEL_4_ANC, 0x08, data);
    g_assert_true(ble_airpods_parse_manufacturer_data(
        data, sizeof(data), &advertisement));
    g_assert_true(advertisement.worn);

    build_advertisement(AIRPODS_MODEL_4_ANC, 0x20, data);
    g_assert_true(ble_airpods_parse_manufacturer_data(
        data, sizeof(data), &advertisement));
    g_assert_false(advertisement.worn);

    build_advertisement(AIRPODS_MODEL_4_ANC, 0x0a, data);
    g_assert_true(ble_airpods_parse_manufacturer_data(
        data, sizeof(data), &advertisement));
    g_assert_true(advertisement.worn);
    g_assert_cmphex(ble_airpods_bluez_product_id(advertisement.model), ==,
                    0x201b);
    g_assert_cmphex(ble_airpods_bluez_product_id(AIRPODS_MODEL_MAX_2), ==,
                    0x202d);
}

static void test_reject_malformed_and_unknown(void)
{
    uint8_t data[27];
    BleAirPodsAdvertisement advertisement;

    build_advertisement(AIRPODS_MODEL_MAX_2, 0x22, data);
    g_assert_false(ble_airpods_parse_manufacturer_data(data, 26,
                                                       &advertisement));

    build_advertisement(0xffff, 0x22, data);
    g_assert_false(ble_airpods_parse_manufacturer_data(
        data, sizeof(data), &advertisement));

    build_advertisement(AIRPODS_MODEL_MAX_2, 0x22, data);
    data[2] = 0x00;
    g_assert_false(ble_airpods_parse_manufacturer_data(
        data, sizeof(data), &advertisement));
}

static void test_supported_model_gate_matches_reconnect_coverage(void)
{
    g_assert_true(ble_airpods_model_supports_wear_autoconnect(
        AIRPODS_MODEL_MAX_2));
    g_assert_true(ble_airpods_model_supports_wear_autoconnect(
        AIRPODS_MODEL_4));
    g_assert_true(ble_airpods_model_supports_wear_autoconnect(
        AIRPODS_MODEL_4_ANC));

    g_assert_false(ble_airpods_model_supports_wear_autoconnect(
        AIRPODS_MODEL_MAX));
    g_assert_false(ble_airpods_model_supports_wear_autoconnect(
        AIRPODS_MODEL_MAX_USBC));
    g_assert_false(ble_airpods_model_supports_wear_autoconnect(
        AIRPODS_MODEL_PRO_2_USBC));
    g_assert_false(ble_airpods_model_supports_wear_autoconnect(
        AIRPODS_MODEL_UNKNOWN));
}

static void test_confirmation_and_rearm(void)
{
    BleAutoConnectState state = {0};

    g_assert_false(ble_autoconnect_observe(&state, false, false, 0, 200, 1000));
    g_assert_false(ble_autoconnect_observe(&state, true, false, 100, 200, 1000));
    g_assert_true(ble_autoconnect_observe(&state, true, false, 200, 200, 1000));
    g_assert_false(ble_autoconnect_observe(&state, true, false, 250, 200, 1000));

    g_assert_false(ble_autoconnect_observe(&state, false, false, 300, 200, 1000));
    g_assert_false(ble_autoconnect_observe(&state, true, false, 400, 200, 1000));
    /* Rearmed, but the prior attempt is still inside the cooldown. */
    g_assert_false(ble_autoconnect_observe(&state, true, false, 500, 200, 1000));

    g_assert_false(ble_autoconnect_observe(&state, false, false, 1200, 200, 1000));
    g_assert_false(ble_autoconnect_observe(&state, true, false, 1300, 200, 1000));
    g_assert_true(ble_autoconnect_observe(&state, true, false, 1400, 200, 1000));
}

static void test_confirmation_window(void)
{
    BleAutoConnectState state = {0};

    g_assert_false(ble_autoconnect_observe(&state, false, false, 0, 200, 1000));
    g_assert_false(ble_autoconnect_observe(&state, true, false, 100, 200, 1000));
    /* Too late: this becomes the first observation of a new window. */
    g_assert_false(ble_autoconnect_observe(&state, true, false, 301, 200, 1000));
    g_assert_true(ble_autoconnect_observe(&state, true, false, 350, 200, 1000));
}

static void test_requires_unworn_baseline(void)
{
    BleAutoConnectState state = {0};

    g_assert_false(ble_autoconnect_observe(&state, true, true, 100, 200, 1000));
    g_assert_false(ble_autoconnect_observe(&state, true, true, 150, 200, 1000));
    g_assert_false(ble_autoconnect_observe(&state, false, false, 200, 200, 1000));
    g_assert_false(ble_autoconnect_observe(&state, true, false, 250, 200, 1000));
    g_assert_true(ble_autoconnect_observe(&state, true, false, 300, 200, 1000));
}

static void test_strong_rssi_fast_path(void)
{
    BleAutoConnectState state = {0};

    /* A strong first worn frame may confirm, but never without a prior
     * unworn frame on this exact advertising object. */
    g_assert_false(ble_autoconnect_observe(&state, true, true,
                                           100, 200, 1000));
    g_assert_false(ble_autoconnect_observe(&state, false, false,
                                           150, 200, 1000));
    g_assert_true(ble_autoconnect_observe(&state, true, true,
                                          200, 200, 1000));
    g_assert_false(ble_autoconnect_observe(&state, true, true,
                                           250, 200, 1000));
}

static void test_continuous_wear_never_reconnects(void)
{
    BleAutoConnectState state = {0};

    g_assert_false(ble_autoconnect_observe(&state, false, false,
                                           0, 200, 1000));
    g_assert_true(ble_autoconnect_observe(&state, true, true,
                                          100, 200, 1000));

    /* Neither an expired confirmation window nor an expired cooldown is a
     * new wear edge. This is what prevents Linux stealing AirPods back after
     * an iPhone handoff while they remain in the ear. */
    g_assert_false(ble_autoconnect_observe(&state, true, true,
                                           500, 200, 1000));
    g_assert_false(ble_autoconnect_observe(&state, true, true,
                                           5000, 200, 1000));

    g_assert_false(ble_autoconnect_observe(&state, false, false,
                                           5100, 200, 1000));
    g_assert_true(ble_autoconnect_observe(&state, true, true,
                                          5200, 200, 1000));
}

static void test_handoff_suppression_requires_stable_removal(void)
{
    BleHandoffSuppressionState state = {0};

    ble_handoff_suppression_activate(&state);
    g_assert_true(ble_handoff_suppression_observe(&state, true,
                                                  100, 500));
    g_assert_true(ble_handoff_suppression_observe(&state, false,
                                                  200, 500));
    g_assert_true(ble_handoff_suppression_observe(&state, false,
                                                  600, 500));

    /* A worn frame means the earlier unworn frames were a transient status
     * from the other bud/case, not a completed removal cycle. */
    g_assert_true(ble_handoff_suppression_observe(&state, true,
                                                  650, 500));
    g_assert_true(ble_handoff_suppression_observe(&state, false,
                                                  700, 500));
    g_assert_false(ble_handoff_suppression_observe(&state, false,
                                                   1200, 500));
    g_assert_false(state.active);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/ble-autoconnect/parse-max-2", test_parse_max_2);
    g_test_add_func("/ble-autoconnect/parse-airpods-4", test_parse_airpods_4);
    g_test_add_func("/ble-autoconnect/reject-invalid",
                    test_reject_malformed_and_unknown);
    g_test_add_func("/ble-autoconnect/supported-model-gate",
                    test_supported_model_gate_matches_reconnect_coverage);
    g_test_add_func("/ble-autoconnect/confirmation", test_confirmation_and_rearm);
    g_test_add_func("/ble-autoconnect/window", test_confirmation_window);
    g_test_add_func("/ble-autoconnect/unworn-baseline",
                    test_requires_unworn_baseline);
    g_test_add_func("/ble-autoconnect/strong-rssi-fast-path",
                    test_strong_rssi_fast_path);
    g_test_add_func("/ble-autoconnect/continuous-wear-does-not-reconnect",
                    test_continuous_wear_never_reconnects);
    g_test_add_func("/ble-autoconnect/handoff-suppression",
                    test_handoff_suppression_requires_stable_removal);
    return g_test_run();
}
