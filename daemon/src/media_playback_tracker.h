/* SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef MEDIA_PLAYBACK_TRACKER_H
#define MEDIA_PLAYBACK_TRACKER_H

#include <glib.h>
#include <stdbool.h>

typedef struct MediaPlaybackTracker MediaPlaybackTracker;

typedef enum {
    MEDIA_PLAYBACK_TRANSITION_NONE,
    MEDIA_PLAYBACK_TRANSITION_STARTED,
    MEDIA_PLAYBACK_TRANSITION_ALL_STOPPED,
} MediaPlaybackTransition;

MediaPlaybackTracker *media_playback_tracker_new(void);
void media_playback_tracker_free(MediaPlaybackTracker *tracker);

MediaPlaybackTransition media_playback_tracker_update(
    MediaPlaybackTracker *tracker,
    const char *player_id,
    bool playing);
MediaPlaybackTransition media_playback_tracker_remove(
    MediaPlaybackTracker *tracker,
    const char *player_id);

bool media_playback_tracker_is_playing(const MediaPlaybackTracker *tracker);
bool media_playback_tracker_player_is_playing(
    const MediaPlaybackTracker *tracker,
    const char *player_id);
GPtrArray *media_playback_tracker_dup_playing_ids(
    const MediaPlaybackTracker *tracker);

#endif /* MEDIA_PLAYBACK_TRACKER_H */
