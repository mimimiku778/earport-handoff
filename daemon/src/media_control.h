/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2024 EarPort Contributors
 *
 * Media control via MPRIS D-Bus interface
 * Handles pause/play when AirPods are removed from ears
 */

#ifndef MEDIA_CONTROL_H
#define MEDIA_CONTROL_H

#include <glib.h>
#include <stdbool.h>
#include "wear_policy.h"

/* Media control context */
typedef struct MediaControl MediaControl;

typedef void (*MediaPlaybackStartedCallback)(void *user_data);

typedef enum {
    MEDIA_HANDOFF_RESUME_NONE,
    MEDIA_HANDOFF_RESUME_STARTED,
    MEDIA_HANDOFF_RESUME_DEFERRED_FOR_EAR_STATE,
} MediaHandoffResumeResult;

/* Create new media control instance */
MediaControl *media_control_new(void);

/* Free media control instance */
void media_control_free(MediaControl *mc);

/* Set ear pause mode */
void media_control_set_ear_pause_mode(MediaControl *mc, EarPauseMode mode);

/* Get current ear pause mode */
EarPauseMode media_control_get_ear_pause_mode(MediaControl *mc);

/* Notify a caller whenever an MPRIS player transitions to Playing. */
void media_control_set_playback_started_callback(MediaControl *mc,
                                                 MediaPlaybackStartedCallback callback,
                                                 void *user_data);

/* Forget per-device edge and resume tracking when switching AirPods. */
void media_control_reset_device_state(MediaControl *mc);

/* Update ear detection state - will trigger pause/play as needed */
void media_control_on_ear_detection_changed(MediaControl *mc,
                                            bool left_in_ear,
                                            bool right_in_ear);

/* Pause all playing media players */
void media_control_pause_all(MediaControl *mc);

/* Pause and track players specifically for an AudioSource handoff. */
void media_control_pause_all_for_handoff(MediaControl *mc);

/* Resume only players paused for handoff, deferring while AirPods are out. */
MediaHandoffResumeResult media_control_resume_handoff(MediaControl *mc);

/* Whether at least one player is still owned by the handoff pause reason. */
bool media_control_has_handoff_paused(MediaControl *mc);

/* Check whether at least one MPRIS player currently reports Playing. */
bool media_control_is_playing(MediaControl *mc);

/* Whether the latest known wear state forbids playback in the configured
 * ear-pause mode. Unknown state and disabled ear-pause return false. */
bool media_control_wear_state_blocks_playback(MediaControl *mc);

/* Restart the AirPods audio sink after claiming AudioSource ownership. */
bool media_control_reclaim_audio(MediaControl *mc, const char *device_address);

/* Resume media players that were paused by us */
void media_control_resume(MediaControl *mc);

#endif /* MEDIA_CONTROL_H */
