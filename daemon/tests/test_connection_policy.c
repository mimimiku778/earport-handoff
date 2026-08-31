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

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/connection-policy/two-device-switch",
                    test_two_device_switch_sequence);
    g_test_add_func("/connection-policy/duplicate-invalid",
                    test_duplicate_and_invalid_events);
    return g_test_run();
}
