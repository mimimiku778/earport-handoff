/* SPDX-License-Identifier: GPL-3.0-or-later */

#include <glib.h>

#include "connection_policy.h"

static void test_two_device_switch_sequence(void)
{
    const char *airpods_4 = "AA:00:00:00:00:04";
    const char *airpods_max_2 = "BB:00:00:00:00:02";
    const char *current = NULL;

    g_assert_cmpint(connection_policy_device_connected(current, airpods_4), ==,
                    CONNECTION_DECISION_CONNECT);
    current = airpods_4;

    g_assert_cmpint(connection_policy_device_connected(current, airpods_max_2),
                    ==, CONNECTION_DECISION_SWITCH);
    current = airpods_max_2;

    /* A late disconnect from the old device must not tear down Max 2. */
    g_assert_cmpint(connection_policy_device_disconnected(current, airpods_4),
                    ==, CONNECTION_DECISION_IGNORE);
    g_assert_cmpint(connection_policy_device_disconnected(current,
                                                           airpods_max_2),
                    ==, CONNECTION_DECISION_DISCONNECT_CURRENT);
}

static void test_duplicate_and_invalid_events(void)
{
    g_assert_true(airpods_address_equal("aa:bb:cc:dd:ee:ff",
                                        "AA:BB:CC:DD:EE:FF"));
    g_assert_cmpint(connection_policy_device_connected(
                        "AA:BB:CC:DD:EE:FF", "aa:bb:cc:dd:ee:ff"),
                    ==, CONNECTION_DECISION_CURRENT);
    g_assert_cmpint(connection_policy_device_connected(NULL, NULL), ==,
                    CONNECTION_DECISION_IGNORE);
    g_assert_cmpint(connection_policy_device_disconnected(NULL,
                                                           "AA:BB:CC:DD:EE:FF"),
                    ==, CONNECTION_DECISION_IGNORE);
}

static void test_removal_lifecycle_same_device_reconnect(void)
{
    RemovalLifecycle lifecycle = {0};
    const char *airpods_4 = "AA:00:00:00:00:04";

    removal_lifecycle_mark(&lifecycle, airpods_4);
    g_assert_true(removal_lifecycle_matches(
        &lifecycle, "aa:00:00:00:00:04"));

    /* Disconnect and reconnect do not consume the cycle. The first worn AAP
     * state does, after media control has had a chance to resume its player. */
    g_assert_false(removal_lifecycle_note_disconnected(&lifecycle,
                                                       airpods_4));
    g_assert_true(removal_lifecycle_note_connected(&lifecycle, airpods_4));
    g_assert_false(removal_lifecycle_note_wear(&lifecycle, airpods_4,
                                               false, false));
    g_assert_true(removal_lifecycle_note_wear(&lifecycle, airpods_4,
                                              true, false));
    g_assert_null(lifecycle.address);

    removal_lifecycle_clear(&lifecycle);
}

static void test_removal_lifecycle_never_crosses_devices(void)
{
    RemovalLifecycle lifecycle = {0};
    const char *airpods_4 = "AA:00:00:00:00:04";
    const char *airpods_max_2 = "BB:00:00:00:00:02";

    removal_lifecycle_mark(&lifecycle, airpods_4);
    g_assert_false(removal_lifecycle_note_connected(&lifecycle,
                                                    airpods_max_2));
    g_assert_null(lifecycle.address);

    removal_lifecycle_mark(&lifecycle, airpods_4);
    g_assert_false(removal_lifecycle_note_wear(&lifecycle,
                                               airpods_max_2,
                                               true, true));
    g_assert_null(lifecycle.address);
    removal_lifecycle_clear(&lifecycle);
}

static void test_removal_lifecycle_rapid_rewear_during_disconnect(void)
{
    RemovalLifecycle lifecycle = {0};
    const char *airpods = "AA:00:00:00:00:04";

    removal_lifecycle_mark(&lifecycle, airpods);
    g_assert_true(lifecycle.disconnect_pending);

    /* The worn notification can arrive after Disconnect was sent but before
     * BlueZ publishes Connected=false. It must request an immediate reconnect
     * without consuming the removal lifecycle. */
    g_assert_false(removal_lifecycle_note_wear(&lifecycle, airpods,
                                               true, false));
    g_assert_true(removal_lifecycle_matches(&lifecycle, airpods));
    g_assert_true(removal_lifecycle_note_disconnected(&lifecycle, airpods));
    g_assert_false(lifecycle.disconnect_pending);

    g_assert_true(removal_lifecycle_note_connected(&lifecycle, airpods));
    /* The first post-reconnect worn notification completes the lifecycle; the
     * daemon uses this exact edge to re-claim already-playing Linux media. */
    g_assert_true(removal_lifecycle_note_wear(&lifecycle, airpods,
                                              true, false));
    g_assert_null(lifecycle.address);
}

static void test_removal_lifecycle_second_removal_cancels_rapid_rewear(void)
{
    RemovalLifecycle lifecycle = {0};
    const char *airpods = "AA:00:00:00:00:04";

    removal_lifecycle_mark(&lifecycle, airpods);
    g_assert_false(removal_lifecycle_note_wear(&lifecycle, airpods,
                                               false, true));
    g_assert_false(removal_lifecycle_note_wear(&lifecycle, airpods,
                                               false, false));
    g_assert_false(removal_lifecycle_note_disconnected(&lifecycle, airpods));
    removal_lifecycle_clear(&lifecycle);
}

static void test_removal_disconnect_retry_policy(void)
{
    g_assert_true(removal_disconnect_retry_should_schedule(1, true, true));
    g_assert_cmpuint(removal_disconnect_retry_delay_msec(1), ==, 250);
    g_assert_true(removal_disconnect_retry_should_schedule(2, true, true));
    g_assert_cmpuint(removal_disconnect_retry_delay_msec(2), ==, 500);
    g_assert_true(removal_disconnect_retry_should_schedule(3, true, true));
    g_assert_cmpuint(removal_disconnect_retry_delay_msec(3), ==, 1000);

    /* Four total attempts is the hard cap. Terminal errors and rewear cancel
     * retries immediately even before the cap is reached. */
    g_assert_false(removal_disconnect_retry_should_schedule(4, true, true));
    g_assert_false(removal_disconnect_retry_should_schedule(1, false, true));
    g_assert_false(removal_disconnect_retry_should_schedule(1, true, false));
    g_assert_false(removal_disconnect_retry_should_schedule(0, true, true));
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/connection-policy/two-device-switch",
                    test_two_device_switch_sequence);
    g_test_add_func("/connection-policy/duplicate-invalid",
                    test_duplicate_and_invalid_events);
    g_test_add_func("/connection-policy/removal-reconnect",
                    test_removal_lifecycle_same_device_reconnect);
    g_test_add_func("/connection-policy/removal-device-isolation",
                    test_removal_lifecycle_never_crosses_devices);
    g_test_add_func("/connection-policy/rapid-rewear-during-disconnect",
                    test_removal_lifecycle_rapid_rewear_during_disconnect);
    g_test_add_func("/connection-policy/second-removal-cancels-rewear",
                    test_removal_lifecycle_second_removal_cancels_rapid_rewear);
    g_test_add_func("/connection-policy/removal-disconnect-retry",
                    test_removal_disconnect_retry_policy);
    return g_test_run();
}
