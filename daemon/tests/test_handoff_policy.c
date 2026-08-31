/* SPDX-License-Identifier: GPL-3.0-or-later */

#include <glib.h>

#include "handoff_policy.h"

static void test_remote_none_stays_yielded(void)
{
    HandoffPolicy policy;

    handoff_policy_reset(&policy);
    handoff_policy_note_source(&policy, HANDOFF_SOURCE_LOCAL);
    g_assert_false(handoff_policy_is_yielded(&policy));
    g_assert_true(handoff_policy_has_confirmed_linux_source(&policy));

    handoff_policy_note_source(&policy, HANDOFF_SOURCE_REMOTE);
    g_assert_true(handoff_policy_is_yielded(&policy));
    g_assert_false(handoff_policy_has_confirmed_linux_source(&policy));

    handoff_policy_note_source(&policy, HANDOFF_SOURCE_NONE);
    g_assert_true(handoff_policy_is_yielded(&policy));
    g_assert_false(handoff_policy_has_confirmed_linux_source(&policy));

    handoff_policy_note_linux_claim(&policy);
    g_assert_false(handoff_policy_is_yielded(&policy));
    g_assert_false(handoff_policy_has_confirmed_linux_source(&policy));
}

static void test_smart_routing_request_stays_yielded(void)
{
    HandoffPolicy policy;

    handoff_policy_reset(&policy);
    handoff_policy_note_remote_request(&policy);
    handoff_policy_note_source(&policy, HANDOFF_SOURCE_NONE);
    g_assert_true(handoff_policy_is_yielded(&policy));
    g_assert_false(handoff_policy_has_confirmed_linux_source(&policy));
}

static void test_confirmed_linux_source_survives_none(void)
{
    HandoffPolicy policy;

    handoff_policy_reset(&policy);
    g_assert_false(handoff_policy_has_confirmed_linux_source(&policy));

    /* OwnsConnection is optimistic and must not classify speaker playback as
     * AirPods playback before the device confirms the source. */
    handoff_policy_note_linux_claim(&policy);
    g_assert_false(handoff_policy_has_confirmed_linux_source(&policy));

    handoff_policy_note_source(&policy, HANDOFF_SOURCE_LOCAL);
    g_assert_true(handoff_policy_has_confirmed_linux_source(&policy));

    /* A duplicate local claim does not erase an already confirmed source. */
    handoff_policy_note_linux_claim(&policy);
    g_assert_true(handoff_policy_has_confirmed_linux_source(&policy));

    handoff_policy_note_source(&policy, HANDOFF_SOURCE_NONE);
    g_assert_true(handoff_policy_has_confirmed_linux_source(&policy));

    handoff_policy_reset(&policy);
    g_assert_false(handoff_policy_has_confirmed_linux_source(&policy));
}

static void test_remote_events_clear_linux_source(void)
{
    HandoffPolicy policy;

    handoff_policy_reset(&policy);
    handoff_policy_note_source(&policy, HANDOFF_SOURCE_LOCAL);
    g_assert_true(handoff_policy_has_confirmed_linux_source(&policy));

    handoff_policy_note_source(&policy, HANDOFF_SOURCE_REMOTE);
    g_assert_false(handoff_policy_has_confirmed_linux_source(&policy));

    handoff_policy_note_source(&policy, HANDOFF_SOURCE_LOCAL);
    handoff_policy_note_remote_request(&policy);
    g_assert_false(handoff_policy_has_confirmed_linux_source(&policy));
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/handoff/remote-none-stays-yielded",
                    test_remote_none_stays_yielded);
    g_test_add_func("/handoff/smart-routing-stays-yielded",
                    test_smart_routing_request_stays_yielded);
    g_test_add_func("/handoff/confirmed-linux-survives-none",
                    test_confirmed_linux_source_survives_none);
    g_test_add_func("/handoff/remote-clears-confirmed-linux",
                    test_remote_events_clear_linux_source);
    return g_test_run();
}
