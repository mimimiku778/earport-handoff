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
typedef void (*MediaPlaybackStoppedCallback)(void *user_data);

/* Create new media control instance */
MediaControl *media_control_new(void);

/* Free media control instance */
void media_control_free(MediaControl *mc);

/* Set ear pause mode */
void media_control_set_ear_pause_mode(MediaControl *mc, EarPauseMode mode);

/* Get current ear pause mode */
EarPauseMode media_control_get_ear_pause_mode(MediaControl *mc);

/* Notify a caller when aggregate MPRIS playback transitions from no active
 * players to at least one Playing player. */
void media_control_set_playback_started_callback(MediaControl *mc,
                                                 MediaPlaybackStartedCallback callback,
                                                 void *user_data);

/* Notify a caller when aggregate MPRIS playback transitions from at least
 * one Playing player to no Playing players. */
void media_control_set_playback_stopped_callback(MediaControl *mc,
                                                 MediaPlaybackStoppedCallback callback,
                                                 void *user_data);

/* Mark whether the current AirPods AAP control link is usable.  Wear state
 * is scoped to one live control session and is invalidated on link loss, so
 * an off-head value from a disconnected device can never gate speaker
 * playback.  Ear-pause ownership is retained for a removal/reconnect cycle. */
void media_control_set_airpods_link_active(MediaControl *mc, bool active);

/* Mark whether AirPods have positively reported Linux as their current audio
 * source.  A live control link alone is not enough to pause desktop media:
 * the user may be listening through speakers while an Apple host owns them. */
void media_control_set_airpods_audio_active(MediaControl *mc, bool active);

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

/* Forget handoff-pause ownership after the user explicitly starts a player.
 * This does not send Play and therefore cannot manufacture playback. */
void media_control_clear_handoff_pause(MediaControl *mc);

/* Check whether at least one MPRIS player currently reports Playing. */
bool media_control_is_playing(MediaControl *mc);

/* Whether the latest known wear state forbids playback in the configured
 * ear-pause mode. Unknown state and disabled ear-pause return false. */
bool media_control_wear_state_blocks_playback(MediaControl *mc);

/* Whether ownership may be claimed and audio routed to the current AirPods.
 * Unlike speaker-pause policy, this fails closed until a fresh wear report
 * says at least one side is worn on the live AAP link. */
bool media_control_can_claim_or_route_audio(MediaControl *mc);

/* Asynchronously select the device's A2DP sink after claiming AudioSource
 * ownership. Returns whether a bounded route attempt was accepted. */
bool media_control_reclaim_audio(MediaControl *mc, const char *device_address);

/* Cancel a pending audio-route attempt (normally on disconnect/switch). */
void media_control_cancel_audio_route(MediaControl *mc);

/* Release an EarPort-managed AirPods default and asynchronously restore the
 * previous non-Bluetooth output, without overriding a later manual choice. */
void media_control_restore_audio_route(MediaControl *mc);

/* Resume media players that were paused by us */
void media_control_resume(MediaControl *mc);

#endif /* MEDIA_CONTROL_H */
