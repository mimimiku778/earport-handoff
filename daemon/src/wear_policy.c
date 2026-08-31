/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2024 EarPort Contributors
 */

#include "wear_policy.h"

bool wear_policy_blocks_playback(EarPauseMode mode,
                                 bool state_valid,
                                 bool left_in_ear,
                                 bool right_in_ear)
{
    if (!state_valid || mode == EAR_PAUSE_DISABLED)
        return false;

    switch (mode) {
    case EAR_PAUSE_ONE_OUT:
        return !left_in_ear || !right_in_ear;
    case EAR_PAUSE_BOTH_OUT:
        return !left_in_ear && !right_in_ear;
    case EAR_PAUSE_DISABLED:
    default:
        return false;
    }
}

WearPolicyAction wear_policy_transition(EarPauseMode mode,
                                        bool previous_state_valid,
                                        bool previous_left_in_ear,
                                        bool previous_right_in_ear,
                                        bool left_in_ear,
                                        bool right_in_ear)
{
    bool now_blocked = wear_policy_blocks_playback(mode, true,
                                                   left_in_ear,
                                                   right_in_ear);

    if (mode == EAR_PAUSE_DISABLED)
        return WEAR_POLICY_ACTION_NONE;

    /* The first AAP state is authoritative. If it says the AirPods are out,
     * pause media immediately instead of waiting for a synthetic in->out
     * edge that may never arrive. */
    if (!previous_state_valid)
        return now_blocked ? WEAR_POLICY_ACTION_PAUSE :
                             WEAR_POLICY_ACTION_NONE;

    bool was_blocked = wear_policy_blocks_playback(mode, true,
                                                   previous_left_in_ear,
                                                   previous_right_in_ear);
    if (!was_blocked && now_blocked)
        return WEAR_POLICY_ACTION_PAUSE;
    if (was_blocked && !now_blocked)
        return WEAR_POLICY_ACTION_RESUME;

    return WEAR_POLICY_ACTION_NONE;
}

WearPolicyAction wear_policy_mode_change(EarPauseMode previous_mode,
                                         EarPauseMode new_mode,
                                         bool state_valid,
                                         bool left_in_ear,
                                         bool right_in_ear)
{
    if (!state_valid)
        return WEAR_POLICY_ACTION_NONE;

    bool was_blocked = wear_policy_blocks_playback(previous_mode, true,
                                                   left_in_ear,
                                                   right_in_ear);
    bool now_blocked = wear_policy_blocks_playback(new_mode, true,
                                                   left_in_ear,
                                                   right_in_ear);

    if (!was_blocked && now_blocked)
        return WEAR_POLICY_ACTION_PAUSE;
    if (was_blocked && !now_blocked)
        return WEAR_POLICY_ACTION_RESUME;

    return WEAR_POLICY_ACTION_NONE;
}
