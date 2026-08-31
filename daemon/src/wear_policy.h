/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2024 EarPort Contributors
 *
 * Pure policy helpers for translating AAP wear state into media actions.
 */

#ifndef WEAR_POLICY_H
#define WEAR_POLICY_H

#include <stdbool.h>

/* Ear detection mode for auto-pause behavior */
typedef enum {
    EAR_PAUSE_DISABLED = 0,    /* Don't pause on ear removal */
    EAR_PAUSE_ONE_OUT = 1,     /* Pause when one pod is removed */
    EAR_PAUSE_BOTH_OUT = 2,    /* Pause when both pods are removed */
} EarPauseMode;

typedef enum {
    WEAR_POLICY_ACTION_NONE,
    WEAR_POLICY_ACTION_PAUSE,
    WEAR_POLICY_ACTION_RESUME,
} WearPolicyAction;

/* Unknown wear state and disabled auto-pause never gate playback. */
bool wear_policy_blocks_playback(EarPauseMode mode,
                                 bool state_valid,
                                 bool left_in_ear,
                                 bool right_in_ear);

/* Return the action for a newly received AAP wear state. */
WearPolicyAction wear_policy_transition(EarPauseMode mode,
                                        bool previous_state_valid,
                                        bool previous_left_in_ear,
                                        bool previous_right_in_ear,
                                        bool left_in_ear,
                                        bool right_in_ear);

/* Return the action required when the configured mode changes while the
 * latest AAP wear state remains the same. Unknown wear state remains
 * fail-open and therefore never produces an action. */
WearPolicyAction wear_policy_mode_change(EarPauseMode previous_mode,
                                         EarPauseMode new_mode,
                                         bool state_valid,
                                         bool left_in_ear,
                                         bool right_in_ear);

/* BlueZ removal-disconnect is deliberately independent of the media pause
 * mode: it is eligible only when both normalized AAP wear slots are out. */
bool wear_policy_fully_removed(bool left_in_ear, bool right_in_ear);

#endif /* WEAR_POLICY_H */
