/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 EarPort Contributors
 */

#include <glib.h>

#include "audio_route.h"

static void test_find_matching_sink(void)
{
    const gchar *output =
        "Sink #42\n"
        "\tName: alsa_output.pci-0000_00_1f.3.analog-stereo\n"
        "Sink #57\n"
        "\tName: bluez_output.AA_BB_CC_DD_EE_FF.1\n"
        "\tProperties:\n"
        "\t\tapi.bluez5.profile = \"a2dp\"\n";
    gchar *sink = audio_route_find_bluez_sink(
        output, "aa:bb:cc:dd:ee:ff");

    g_assert_cmpstr(sink, ==, "bluez_output.AA_BB_CC_DD_EE_FF.1");
    g_free(sink);
}

static void test_find_colon_sink_case_insensitively(void)
{
    const gchar *output =
        "Sink #57\n"
        "\tName: BLUEZ_OUTPUT.aa:bb:cc:dd:ee:ff\n"
        "\tProperties:\n"
        "\t\tbluetooth.protocol = \"a2dp-sink\"\n";
    gchar *sink = audio_route_find_bluez_sink(
        output, "AA:BB:CC:DD:EE:FF");

    g_assert_cmpstr(sink, ==, "BLUEZ_OUTPUT.aa:bb:cc:dd:ee:ff");
    g_free(sink);
}

static void test_generic_short_sink_is_not_profile_safe(void)
{
    const gchar *output =
        "57\tbluez_output.AA_BB_CC_DD_EE_FF.1\tPipeWire\ts16le\n";

    g_assert_null(audio_route_find_bluez_sink(
        output, "aa:bb:cc:dd:ee:ff"));
}

static void test_verbose_call_profile_is_rejected(void)
{
    const gchar *output =
        "Sink #57\n"
        "\tName: bluez_output.AA_BB_CC_DD_EE_FF.1\n"
        "\tProperties:\n"
        "\t\tapi.bluez5.profile = \"headset-head-unit\"\n";

    g_assert_null(audio_route_find_bluez_sink(
        output, "aa:bb:cc:dd:ee:ff"));
}

static void test_prefers_a2dp_over_headset(void)
{
    const gchar *output =
        "57\tbluez_output.AA:BB:CC:DD:EE:FF.headset-head-unit\tPipeWire\ts16le\n"
        "58\tbluez_output.aa_bb_cc_dd_ee_ff.a2dp-sink\tPipeWire\ts24le\n";
    gchar *sink = audio_route_find_bluez_sink(
        output, "Aa:Bb:Cc:Dd:Ee:Ff");

    g_assert_cmpstr(sink, ==,
                    "bluez_output.aa_bb_cc_dd_ee_ff.a2dp-sink");
    g_free(sink);
}

static void test_headset_only_waits_for_a2dp(void)
{
    const gchar *output =
        "57\tbluez_output.AA:BB:CC:DD:EE:FF.headset-head-unit\tPipeWire\ts16le\n";

    g_assert_null(audio_route_find_bluez_sink(
        output, "aa:bb:cc:dd:ee:ff"));
}

static void test_rejects_all_explicit_call_profiles(void)
{
    const gchar *output =
        "57\tbluez_output.AA_BB_CC_DD_EE_FF.hfp-hf\tPipeWire\ts16le\n"
        "58\tbluez_output.AA_BB_CC_DD_EE_FF.HSP_HS\tPipeWire\ts16le\n"
        "59\tbluez_output.AA_BB_CC_DD_EE_FF.handsfree-head-unit\tPipeWire\ts16le\n";

    g_assert_null(audio_route_find_bluez_sink(
        output, "aa:bb:cc:dd:ee:ff"));
}

static void test_accepts_high_fidelity_names(void)
{
    const gchar *hyphen_output =
        "57\tbluez_output.AA_BB_CC_DD_EE_FF.high-fidelity-playback\tPipeWire\ts24le\n";
    const gchar *underscore_output =
        "58\tbluez_output.AA_BB_CC_DD_EE_FF.high_fidelity_playback\tPipeWire\ts24le\n";
    gchar *sink = audio_route_find_bluez_sink(
        hyphen_output, "aa:bb:cc:dd:ee:ff");

    g_assert_cmpstr(sink, ==,
                    "bluez_output.AA_BB_CC_DD_EE_FF.high-fidelity-playback");
    g_free(sink);

    sink = audio_route_find_bluez_sink(
        underscore_output, "aa:bb:cc:dd:ee:ff");
    g_assert_cmpstr(sink, ==,
                    "bluez_output.AA_BB_CC_DD_EE_FF.high_fidelity_playback");
    g_free(sink);
}

static void test_does_not_match_address_outside_sink_name(void)
{
    const gchar *output =
        "42\talsa_output.pci-0000_00_1f.3.analog-stereo\t"
        "bluez-driver-AA:BB:CC:DD:EE:FF\ts32le\n";

    g_assert_null(audio_route_find_bluez_sink(
        output, "AA:BB:CC:DD:EE:FF"));
}

static void test_rejects_other_device_and_bad_address(void)
{
    const gchar *output =
        "57\tbluez_output.AA_BB_CC_DD_EE_FF.1\tPipeWire\ts16le\n";

    g_assert_null(audio_route_find_bluez_sink(
        output, "11:22:33:44:55:66"));
    g_assert_null(audio_route_find_bluez_sink(output, "not-an-address"));
}

static void test_parse_sink_inputs(void)
{
    const gchar *output =
        "81\t120\t80\tPipeWire\tfloat32le\n"
        "garbage\n"
        "102\t120\t101\tPipeWire\tfloat32le\n";
    GPtrArray *ids = audio_route_parse_sink_input_ids(output);

    g_assert_cmpuint(ids->len, ==, 2);
    g_assert_cmpstr(g_ptr_array_index(ids, 0), ==, "81");
    g_assert_cmpstr(g_ptr_array_index(ids, 1), ==, "102");
    g_ptr_array_unref(ids);
}

static void test_parse_default_and_configured_sink(void)
{
    gchar *sink = audio_route_parse_default_sink(
        "alsa_output.usb-Speakers.analog-stereo\n");
    g_assert_cmpstr(sink, ==,
                    "alsa_output.usb-Speakers.analog-stereo");
    g_free(sink);

    const gchar *metadata =
        "Found \"default\" metadata 45\n"
        "update: id:0 key:'default.configured.audio.sink' "
        "value:'{ \"name\": \"bluez_output.AA_BB_CC_DD_EE_FF.1\" }' "
        "type:'Spa:String:JSON'\n";
    sink = audio_route_parse_configured_sink(metadata);
    g_assert_cmpstr(sink, ==,
                    "bluez_output.AA_BB_CC_DD_EE_FF.1");
    g_free(sink);

    g_assert_null(audio_route_parse_default_sink("bad sink name\n"));
    g_assert_null(audio_route_parse_configured_sink(
        "update: id:0 key:'default.audio.source' "
        "value:'{\"name\":\"not-a-sink\"}'\n"));
}

static void test_restore_sink_prefers_saved_live_non_bluetooth(void)
{
    const gchar *sinks =
        "63\talsa_output.pci-hdmi.stereo\tPipeWire\ts32le\n"
        "68\talsa_output.usb-Pebble.analog-stereo\tPipeWire\ts32le\n"
        "91\tbluez_output.AA_BB_CC_DD_EE_FF.1\tPipeWire\ts32le\n";
    gchar *sink = audio_route_find_restore_sink(
        sinks, "alsa_output.usb-Pebble.analog-stereo");
    g_assert_cmpstr(sink, ==,
                    "alsa_output.usb-Pebble.analog-stereo");
    g_free(sink);

    sink = audio_route_find_restore_sink(sinks, "alsa_output.missing");
    g_assert_cmpstr(sink, ==, "alsa_output.pci-hdmi.stereo");
    g_free(sink);

    g_assert_null(audio_route_find_restore_sink(
        "91\tbluez_output.AA_BB_CC_DD_EE_FF.1\tPipeWire\ts32le\n",
        NULL));
}

static void test_restore_filters_managed_sink_inputs(void)
{
    const gchar *sinks =
        "68\talsa_output.usb-Pebble.analog-stereo\tPipeWire\ts32le\n"
        "91\tbluez_output.AA_BB_CC_DD_EE_FF.1\tPipeWire\ts32le\n";
    gchar *index = audio_route_find_sink_index(
        sinks, "bluez_output.AA_BB_CC_DD_EE_FF.1");
    g_assert_cmpstr(index, ==, "91");

    const gchar *inputs =
        "81\t91\t80\tPipeWire\tfloat32le\n"
        "82\t68\t81\tPipeWire\tfloat32le\n"
        "103\t91\t102\tPipeWire\tfloat32le\n";
    GPtrArray *ids = audio_route_parse_sink_input_ids_for_sink(inputs, index);
    g_assert_cmpuint(ids->len, ==, 2);
    g_assert_cmpstr(g_ptr_array_index(ids, 0), ==, "81");
    g_assert_cmpstr(g_ptr_array_index(ids, 1), ==, "103");
    g_ptr_array_unref(ids);
    g_free(index);
}

static void test_restore_compare_and_set_guard(void)
{
    const gchar *managed = "bluez_output.AA_BB_CC_DD_EE_FF.1";
    g_assert_true(audio_route_should_restore_configured(managed, managed));
    g_assert_false(audio_route_should_restore_configured(
        "alsa_output.usb-Pebble.analog-stereo", managed));
    g_assert_false(audio_route_should_restore_configured(NULL, managed));
    g_assert_false(audio_route_should_restore_configured(managed, NULL));
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/audio-route/find-matching-sink", test_find_matching_sink);
    g_test_add_func("/audio-route/find-colon-case-insensitive",
                    test_find_colon_sink_case_insensitively);
    g_test_add_func("/audio-route/generic-short-is-untrusted",
                    test_generic_short_sink_is_not_profile_safe);
    g_test_add_func("/audio-route/verbose-call-profile-rejected",
                    test_verbose_call_profile_is_rejected);
    g_test_add_func("/audio-route/prefer-a2dp",
                    test_prefers_a2dp_over_headset);
    g_test_add_func("/audio-route/headset-waits-for-a2dp",
                    test_headset_only_waits_for_a2dp);
    g_test_add_func("/audio-route/reject-call-profiles",
                    test_rejects_all_explicit_call_profiles);
    g_test_add_func("/audio-route/high-fidelity-names",
                    test_accepts_high_fidelity_names);
    g_test_add_func("/audio-route/sink-field-only",
                    test_does_not_match_address_outside_sink_name);
    g_test_add_func("/audio-route/reject-invalid", test_rejects_other_device_and_bad_address);
    g_test_add_func("/audio-route/parse-sink-inputs", test_parse_sink_inputs);
    g_test_add_func("/audio-route/parse-defaults",
                    test_parse_default_and_configured_sink);
    g_test_add_func("/audio-route/restore-prefers-live-saved-sink",
                    test_restore_sink_prefers_saved_live_non_bluetooth);
    g_test_add_func("/audio-route/restore-filters-managed-inputs",
                    test_restore_filters_managed_sink_inputs);
    g_test_add_func("/audio-route/restore-compare-and-set",
                    test_restore_compare_and_set_guard);
    return g_test_run();
}
