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

static void test_confirmation_and_rearm(void)
{
    BleAutoConnectState state = {0};

    g_assert_false(ble_autoconnect_observe(&state, false, 0, 200, 1000));
    g_assert_false(ble_autoconnect_observe(&state, true, 100, 200, 1000));
    g_assert_true(ble_autoconnect_observe(&state, true, 200, 200, 1000));
    g_assert_false(ble_autoconnect_observe(&state, true, 250, 200, 1000));

    g_assert_false(ble_autoconnect_observe(&state, false, 300, 200, 1000));
    g_assert_false(ble_autoconnect_observe(&state, true, 400, 200, 1000));
    /* Rearmed, but the prior attempt is still inside the cooldown. */
    g_assert_false(ble_autoconnect_observe(&state, true, 500, 200, 1000));

    g_assert_false(ble_autoconnect_observe(&state, false, 1200, 200, 1000));
    g_assert_false(ble_autoconnect_observe(&state, true, 1300, 200, 1000));
    g_assert_true(ble_autoconnect_observe(&state, true, 1400, 200, 1000));
}

static void test_confirmation_window(void)
{
    BleAutoConnectState state = {0};

    g_assert_false(ble_autoconnect_observe(&state, false, 0, 200, 1000));
    g_assert_false(ble_autoconnect_observe(&state, true, 100, 200, 1000));
    /* Too late: this becomes the first observation of a new window. */
    g_assert_false(ble_autoconnect_observe(&state, true, 301, 200, 1000));
    g_assert_true(ble_autoconnect_observe(&state, true, 350, 200, 1000));
}

static void test_requires_unworn_baseline(void)
{
    BleAutoConnectState state = {0};

    g_assert_false(ble_autoconnect_observe(&state, true, 100, 200, 1000));
    g_assert_false(ble_autoconnect_observe(&state, true, 150, 200, 1000));
    g_assert_false(ble_autoconnect_observe(&state, false, 200, 200, 1000));
    g_assert_false(ble_autoconnect_observe(&state, true, 250, 200, 1000));
    g_assert_true(ble_autoconnect_observe(&state, true, 300, 200, 1000));
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/ble-autoconnect/parse-max-2", test_parse_max_2);
    g_test_add_func("/ble-autoconnect/parse-airpods-4", test_parse_airpods_4);
    g_test_add_func("/ble-autoconnect/reject-invalid",
                    test_reject_malformed_and_unknown);
    g_test_add_func("/ble-autoconnect/confirmation", test_confirmation_and_rearm);
    g_test_add_func("/ble-autoconnect/window", test_confirmation_window);
    g_test_add_func("/ble-autoconnect/unworn-baseline",
                    test_requires_unworn_baseline);
    return g_test_run();
}
