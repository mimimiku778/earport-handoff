/* SPDX-License-Identifier: GPL-3.0-or-later */

#include <glib.h>

#include "handoff_policy.h"

static void test_remote_none_stays_yielded(void)
{
    HandoffPolicy policy;

    handoff_policy_reset(&policy);
    handoff_policy_note_source(&policy, HANDOFF_SOURCE_LOCAL);
    g_assert_false(handoff_policy_is_yielded(&policy));

    handoff_policy_note_source(&policy, HANDOFF_SOURCE_REMOTE);
    g_assert_true(handoff_policy_is_yielded(&policy));

    handoff_policy_note_source(&policy, HANDOFF_SOURCE_NONE);
    g_assert_true(handoff_policy_is_yielded(&policy));

    handoff_policy_note_linux_claim(&policy);
    g_assert_false(handoff_policy_is_yielded(&policy));
}

static void test_smart_routing_request_stays_yielded(void)
{
    HandoffPolicy policy;

    handoff_policy_reset(&policy);
    handoff_policy_note_remote_request(&policy);
    handoff_policy_note_source(&policy, HANDOFF_SOURCE_NONE);
    g_assert_true(handoff_policy_is_yielded(&policy));
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/handoff/remote-none-stays-yielded",
                    test_remote_none_stays_yielded);
    g_test_add_func("/handoff/smart-routing-stays-yielded",
                    test_smart_routing_request_stays_yielded);
    return g_test_run();
}
