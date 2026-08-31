/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2024 EarPort Contributors
 */

#include <glib.h>

#include "wear_policy.h"

static void test_playback_gate(void)
{
    g_assert_false(wear_policy_blocks_playback(EAR_PAUSE_ONE_OUT,
                                               false, false, false));
    g_assert_false(wear_policy_blocks_playback(EAR_PAUSE_DISABLED,
                                               true, false, false));

    g_assert_false(wear_policy_blocks_playback(EAR_PAUSE_ONE_OUT,
                                               true, true, true));
    g_assert_true(wear_policy_blocks_playback(EAR_PAUSE_ONE_OUT,
                                              true, false, true));
    g_assert_true(wear_policy_blocks_playback(EAR_PAUSE_ONE_OUT,
                                              true, false, false));

    g_assert_false(wear_policy_blocks_playback(EAR_PAUSE_BOTH_OUT,
                                               true, false, true));
    g_assert_true(wear_policy_blocks_playback(EAR_PAUSE_BOTH_OUT,
                                              true, false, false));
}

static void test_initial_state(void)
{
    g_assert_cmpint(wear_policy_transition(EAR_PAUSE_ONE_OUT,
                                           false, false, false,
                                           false, false),
                    ==, WEAR_POLICY_ACTION_PAUSE);
    g_assert_cmpint(wear_policy_transition(EAR_PAUSE_ONE_OUT,
                                           false, false, false,
                                           true, true),
                    ==, WEAR_POLICY_ACTION_NONE);
    g_assert_cmpint(wear_policy_transition(EAR_PAUSE_DISABLED,
                                           false, false, false,
                                           false, false),
                    ==, WEAR_POLICY_ACTION_NONE);
}

static void test_transitions(void)
{
    g_assert_cmpint(wear_policy_transition(EAR_PAUSE_ONE_OUT,
                                           true, true, true,
                                           false, true),
                    ==, WEAR_POLICY_ACTION_PAUSE);
    g_assert_cmpint(wear_policy_transition(EAR_PAUSE_ONE_OUT,
                                           true, false, true,
                                           true, true),
                    ==, WEAR_POLICY_ACTION_RESUME);
    g_assert_cmpint(wear_policy_transition(EAR_PAUSE_ONE_OUT,
                                           true, false, false,
                                           false, true),
                    ==, WEAR_POLICY_ACTION_NONE);

    g_assert_cmpint(wear_policy_transition(EAR_PAUSE_BOTH_OUT,
                                           true, true, false,
                                           false, false),
                    ==, WEAR_POLICY_ACTION_PAUSE);
    g_assert_cmpint(wear_policy_transition(EAR_PAUSE_BOTH_OUT,
                                           true, false, false,
                                           true, false),
                    ==, WEAR_POLICY_ACTION_RESUME);
}

static void test_mode_changes(void)
{
    /* Unknown state remains fail-open even when enabling a strict mode. */
    g_assert_cmpint(wear_policy_mode_change(EAR_PAUSE_DISABLED,
                                            EAR_PAUSE_ONE_OUT,
                                            false, false, false),
                    ==, WEAR_POLICY_ACTION_NONE);

    /* Enabling or tightening the policy pauses if the known state is now
     * blocked. */
    g_assert_cmpint(wear_policy_mode_change(EAR_PAUSE_DISABLED,
                                            EAR_PAUSE_ONE_OUT,
                                            true, false, false),
                    ==, WEAR_POLICY_ACTION_PAUSE);
    g_assert_cmpint(wear_policy_mode_change(EAR_PAUSE_BOTH_OUT,
                                            EAR_PAUSE_ONE_OUT,
                                            true, false, true),
                    ==, WEAR_POLICY_ACTION_PAUSE);

    /* Disabling or relaxing the policy resumes pauses owned by the old mode. */
    g_assert_cmpint(wear_policy_mode_change(EAR_PAUSE_ONE_OUT,
                                            EAR_PAUSE_DISABLED,
                                            true, false, false),
                    ==, WEAR_POLICY_ACTION_RESUME);
    g_assert_cmpint(wear_policy_mode_change(EAR_PAUSE_ONE_OUT,
                                            EAR_PAUSE_BOTH_OUT,
                                            true, false, true),
                    ==, WEAR_POLICY_ACTION_RESUME);

    /* A change that leaves the current state in the same policy class does
     * not manufacture a playback edge. */
    g_assert_cmpint(wear_policy_mode_change(EAR_PAUSE_ONE_OUT,
                                            EAR_PAUSE_BOTH_OUT,
                                            true, false, false),
                    ==, WEAR_POLICY_ACTION_NONE);
    g_assert_cmpint(wear_policy_mode_change(EAR_PAUSE_ONE_OUT,
                                            EAR_PAUSE_ONE_OUT,
                                            true, true, true),
                    ==, WEAR_POLICY_ACTION_NONE);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/wear-policy/playback-gate", test_playback_gate);
    g_test_add_func("/wear-policy/initial-state", test_initial_state);
    g_test_add_func("/wear-policy/transitions", test_transitions);
    g_test_add_func("/wear-policy/mode-changes", test_mode_changes);
    return g_test_run();
}
