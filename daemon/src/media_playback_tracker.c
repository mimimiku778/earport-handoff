/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "media_playback_tracker.h"

#define PLAYER_NOT_PLAYING GINT_TO_POINTER(1)
#define PLAYER_PLAYING     GINT_TO_POINTER(2)

struct MediaPlaybackTracker {
    GHashTable *players;
    guint playing_count;
};

MediaPlaybackTracker *media_playback_tracker_new(void)
{
    MediaPlaybackTracker *tracker = g_new0(MediaPlaybackTracker, 1);
    tracker->players = g_hash_table_new_full(g_str_hash, g_str_equal,
                                             g_free, NULL);
    return tracker;
}

void media_playback_tracker_free(MediaPlaybackTracker *tracker)
{
    if (tracker == NULL)
        return;

    g_hash_table_unref(tracker->players);
    g_free(tracker);
}

MediaPlaybackTransition media_playback_tracker_update(
    MediaPlaybackTracker *tracker,
    const char *player_id,
    bool playing)
{
    if (tracker == NULL || player_id == NULL || player_id[0] == '\0')
        return MEDIA_PLAYBACK_TRANSITION_NONE;

    gpointer old_value = NULL;
    bool known = g_hash_table_lookup_extended(tracker->players, player_id,
                                               NULL, &old_value);
    bool was_playing = known && old_value == PLAYER_PLAYING;
    if (known && was_playing == playing)
        return MEDIA_PLAYBACK_TRANSITION_NONE;

    guint previous_count = tracker->playing_count;
    if (was_playing)
        tracker->playing_count--;
    if (playing)
        tracker->playing_count++;

    g_hash_table_replace(tracker->players, g_strdup(player_id),
                         playing ? PLAYER_PLAYING : PLAYER_NOT_PLAYING);

    if (previous_count == 0 && tracker->playing_count > 0)
        return MEDIA_PLAYBACK_TRANSITION_STARTED;
    if (previous_count > 0 && tracker->playing_count == 0)
        return MEDIA_PLAYBACK_TRANSITION_ALL_STOPPED;
    return MEDIA_PLAYBACK_TRANSITION_NONE;
}

MediaPlaybackTransition media_playback_tracker_remove(
    MediaPlaybackTracker *tracker,
    const char *player_id)
{
    if (tracker == NULL || player_id == NULL)
        return MEDIA_PLAYBACK_TRANSITION_NONE;

    gpointer value = NULL;
    if (!g_hash_table_lookup_extended(tracker->players, player_id,
                                      NULL, &value)) {
        return MEDIA_PLAYBACK_TRANSITION_NONE;
    }

    bool was_playing = value == PLAYER_PLAYING;
    g_hash_table_remove(tracker->players, player_id);
    if (!was_playing)
        return MEDIA_PLAYBACK_TRANSITION_NONE;

    tracker->playing_count--;
    return tracker->playing_count == 0
               ? MEDIA_PLAYBACK_TRANSITION_ALL_STOPPED
               : MEDIA_PLAYBACK_TRANSITION_NONE;
}

bool media_playback_tracker_is_playing(const MediaPlaybackTracker *tracker)
{
    return tracker != NULL && tracker->playing_count > 0;
}

bool media_playback_tracker_player_is_playing(
    const MediaPlaybackTracker *tracker,
    const char *player_id)
{
    if (tracker == NULL || player_id == NULL)
        return false;

    return g_hash_table_lookup(tracker->players, player_id) == PLAYER_PLAYING;
}

GPtrArray *media_playback_tracker_dup_playing_ids(
    const MediaPlaybackTracker *tracker)
{
    GPtrArray *ids = g_ptr_array_new_with_free_func(g_free);
    if (tracker == NULL)
        return ids;

    GHashTableIter iter;
    gpointer key = NULL;
    gpointer value = NULL;
    g_hash_table_iter_init(&iter, tracker->players);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        if (value == PLAYER_PLAYING)
            g_ptr_array_add(ids, g_strdup(key));
    }
    return ids;
}
