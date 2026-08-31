/* SPDX-License-Identifier: GPL-3.0-or-later */

#include <glib.h>

#include "media_playback_tracker.h"

static void test_aggregate_transitions(void)
{
    MediaPlaybackTracker *tracker = media_playback_tracker_new();

    g_assert_cmpint(media_playback_tracker_update(tracker, ":1.1", false),
                    ==, MEDIA_PLAYBACK_TRANSITION_NONE);
    g_assert_cmpint(media_playback_tracker_update(tracker, ":1.1", true),
                    ==, MEDIA_PLAYBACK_TRANSITION_STARTED);
    g_assert_cmpint(media_playback_tracker_update(tracker, ":1.1", true),
                    ==, MEDIA_PLAYBACK_TRANSITION_NONE);

    g_assert_cmpint(media_playback_tracker_update(tracker, ":1.2", true),
                    ==, MEDIA_PLAYBACK_TRANSITION_NONE);
    g_assert_cmpint(media_playback_tracker_update(tracker, ":1.1", false),
                    ==, MEDIA_PLAYBACK_TRANSITION_NONE);
    g_assert_true(media_playback_tracker_is_playing(tracker));
    g_assert_cmpint(media_playback_tracker_update(tracker, ":1.2", false),
                    ==, MEDIA_PLAYBACK_TRANSITION_ALL_STOPPED);
    g_assert_false(media_playback_tracker_is_playing(tracker));
    g_assert_cmpint(media_playback_tracker_update(tracker, ":1.2", false),
                    ==, MEDIA_PLAYBACK_TRANSITION_NONE);

    media_playback_tracker_free(tracker);
}

static void test_remove_and_snapshot(void)
{
    MediaPlaybackTracker *tracker = media_playback_tracker_new();

    media_playback_tracker_update(tracker, ":1.1", true);
    media_playback_tracker_update(tracker, ":1.2", false);
    media_playback_tracker_update(tracker, ":1.3", true);

    GPtrArray *ids = media_playback_tracker_dup_playing_ids(tracker);
    g_assert_cmpuint(ids->len, ==, 2);
    g_assert_true(g_ptr_array_find_with_equal_func(ids, ":1.1",
                                                   g_str_equal, NULL));
    g_assert_true(g_ptr_array_find_with_equal_func(ids, ":1.3",
                                                   g_str_equal, NULL));
    g_ptr_array_unref(ids);

    g_assert_cmpint(media_playback_tracker_remove(tracker, ":1.1"),
                    ==, MEDIA_PLAYBACK_TRANSITION_NONE);
    g_assert_cmpint(media_playback_tracker_remove(tracker, ":1.3"),
                    ==, MEDIA_PLAYBACK_TRANSITION_ALL_STOPPED);
    g_assert_cmpint(media_playback_tracker_remove(tracker, ":1.3"),
                    ==, MEDIA_PLAYBACK_TRANSITION_NONE);

    media_playback_tracker_free(tracker);
}

static void test_player_status_lookup(void)
{
    MediaPlaybackTracker *tracker = media_playback_tracker_new();

    media_playback_tracker_update(tracker, ":1.20", true);
    media_playback_tracker_update(tracker, ":1.21", false);

    g_assert_true(media_playback_tracker_player_is_playing(tracker,
                                                            ":1.20"));
    g_assert_false(media_playback_tracker_player_is_playing(tracker,
                                                             ":1.21"));
    g_assert_false(media_playback_tracker_player_is_playing(tracker,
                                                             ":1.22"));
    g_assert_false(media_playback_tracker_player_is_playing(NULL,
                                                             ":1.20"));

    media_playback_tracker_free(tracker);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/media-playback/aggregate-transitions",
                    test_aggregate_transitions);
    g_test_add_func("/media-playback/remove-and-snapshot",
                    test_remove_and_snapshot);
    g_test_add_func("/media-playback/player-status-lookup",
                    test_player_status_lookup);
    return g_test_run();
}
