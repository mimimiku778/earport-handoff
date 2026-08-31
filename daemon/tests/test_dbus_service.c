/* SPDX-License-Identifier: GPL-3.0-or-later */

#include <glib.h>

#include "airpods_state.h"
#include "dbus_service.h"

static void test_second_process_cannot_start(void)
{
    if (!g_test_subprocess())
        return;

    AirPodsState state;
    airpods_state_init(&state);
    DbusService *service = dbus_service_new(&state);
    g_assert_nonnull(service);

    g_test_expect_message(NULL, G_LOG_LEVEL_WARNING,
                          "*Another EarPort daemon already owns*");
    g_assert_false(dbus_service_start(service));
    g_test_assert_expected_messages();

    dbus_service_free(service);
    airpods_state_cleanup(&state);
}

static void test_singleton_ownership(void)
{
    AirPodsState state;
    airpods_state_init(&state);
    DbusService *service = dbus_service_new(&state);
    g_assert_nonnull(service);
    g_assert_true(dbus_service_start(service));

    g_test_trap_subprocess("/dbus/singleton/second-process",
                           5 * G_USEC_PER_SEC,
                           G_TEST_SUBPROCESS_DEFAULT);
    g_test_trap_assert_passed();

    dbus_service_free(service);
    airpods_state_cleanup(&state);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/dbus/singleton/ownership", test_singleton_ownership);
    g_test_add_func("/dbus/singleton/second-process",
                    test_second_process_cannot_start);
    return g_test_run();
}
