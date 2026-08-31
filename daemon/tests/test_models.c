/* SPDX-License-Identifier: GPL-3.0-or-later */

#include <glib.h>

#include "airpods_state.h"
#include "config.h"

static void test_airpods_max_2(void)
{
    g_assert_cmpint(airpods_model_from_number("A3454"), ==,
                    AIRPODS_MODEL_MAX_2);
    g_assert_cmpstr(airpods_model_to_string(AIRPODS_MODEL_MAX_2), ==,
                    "AirPods Max 2");
    g_assert_true(airpods_model_supports_anc(AIRPODS_MODEL_MAX_2));
    g_assert_true(airpods_model_supports_adaptive(AIRPODS_MODEL_MAX_2));
    g_assert_true(airpods_model_is_headphones(AIRPODS_MODEL_MAX_2));

    /* Max 2 has two AAP wear slots. Do not apply the Max 1 mirroring
     * workaround based only on its over-ear form factor. */
    g_assert_false(
        airpods_model_uses_single_aap_wear_sensor(AIRPODS_MODEL_MAX_2));
    g_assert_true(
        airpods_model_uses_single_aap_wear_sensor(AIRPODS_MODEL_MAX));
    g_assert_true(
        airpods_model_uses_single_aap_wear_sensor(AIRPODS_MODEL_MAX_USBC));
}

static void test_airpods_4_model_numbers(void)
{
    const char *plain_models[] = {"A3053", "A3050", "A3054", NULL};
    const char *anc_models[] = {"A3056", "A3055", "A3057", NULL};

    for (int i = 0; plain_models[i] != NULL; i++)
        g_assert_cmpint(airpods_model_from_number(plain_models[i]), ==,
                        AIRPODS_MODEL_4);

    for (int i = 0; anc_models[i] != NULL; i++)
        g_assert_cmpint(airpods_model_from_number(anc_models[i]), ==,
                        AIRPODS_MODEL_4_ANC);

    g_assert_false(airpods_model_supports_anc(AIRPODS_MODEL_4));
    g_assert_false(airpods_model_supports_adaptive(AIRPODS_MODEL_4));
    g_assert_true(airpods_model_supports_anc(AIRPODS_MODEL_4_ANC));
    g_assert_true(airpods_model_supports_adaptive(AIRPODS_MODEL_4_ANC));
    g_assert_false(airpods_model_is_headphones(AIRPODS_MODEL_4));
    g_assert_false(airpods_model_is_headphones(AIRPODS_MODEL_4_ANC));
}

static void test_handoff_default(void)
{
    EarPortConfig config;
    config_get_defaults(&config);
    g_assert_cmpint(config.ear_pause_mode, ==, 2);
    g_assert_true(config.handoff_enabled);
    g_assert_true(config.auto_connect_on_wear);
    g_assert_true(config.disconnect_on_removal);
}

static void test_state_reset_clears_per_device_features(void)
{
    AirPodsState state;
    airpods_state_init(&state);
    state.ear_pause_mode = 2;
    airpods_state_set_device(&state, "AirPods", "AA:BB:CC:DD:EE:FF",
                             AIRPODS_MODEL_MAX_2);
    airpods_state_set_listening_modes(&state, true, false, false, true);
    airpods_state_set_ear_detection(&state, true, true);

    airpods_state_reset(&state);

    g_assert_false(state.connected);
    g_assert_false(state.listening_modes.off_enabled);
    g_assert_true(state.listening_modes.transparency_enabled);
    g_assert_true(state.listening_modes.anc_enabled);
    g_assert_false(state.listening_modes.adaptive_enabled);
    g_assert_false(state.ear_detection.left_in_ear);
    g_assert_false(state.ear_detection.right_in_ear);
    /* Global preferences survive a per-device disconnect. */
    g_assert_cmpint(state.ear_pause_mode, ==, 2);

    airpods_state_cleanup(&state);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/models/airpods-max-2", test_airpods_max_2);
    g_test_add_func("/models/airpods-4", test_airpods_4_model_numbers);
    g_test_add_func("/config/handoff-default", test_handoff_default);
    g_test_add_func("/state/reset-clears-per-device-features",
                    test_state_reset_clears_per_device_features);
    return g_test_run();
}
