/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2024 EarPort Contributors
 *
 * Media control via MPRIS D-Bus interface
 */

#include "media_control.h"
#include <gio/gio.h>
#include <string.h>

#define MPRIS_DBUS_NAME_PREFIX "org.mpris.MediaPlayer2."
#define MPRIS_DBUS_PATH "/org/mpris/MediaPlayer2"
#define MPRIS_PLAYER_INTERFACE "org.mpris.MediaPlayer2.Player"
#define DBUS_PROPERTIES_INTERFACE "org.freedesktop.DBus.Properties"
#define MPRIS_CALL_TIMEOUT_MSEC 2000

struct MediaControl {
    GDBusConnection *connection;
    guint properties_signal_id;
    EarPauseMode ear_pause_mode;

    MediaPlaybackStartedCallback playback_started_callback;
    void *playback_started_user_data;

    /* Track pause ownership separately so an ear event cannot resume a player
     * paused because an Apple device took the AirPods audio source. */
    GList *ear_paused_players;
    GList *handoff_paused_players;

    /* Previous ear state for edge detection */
    bool prev_left_in_ear;
    bool prev_right_in_ear;
    bool prev_state_valid;
};

static void pause_all_into(MediaControl *mc,
                           GList **paused_players,
                           bool preserve_existing);

static bool player_list_contains(GList *players, const gchar *player_name)
{
    return g_list_find_custom(players,
                              player_name,
                              (GCompareFunc)g_strcmp0) != NULL;
}

static void player_list_add_unique(GList **players, const gchar *player_name)
{
    if (!player_list_contains(*players, player_name))
        *players = g_list_append(*players, g_strdup(player_name));
}

static void transfer_player_ownership_unique(GList **destination,
                                             GList **source)
{
    for (GList *l = *source; l != NULL; l = l->next)
        player_list_add_unique(destination, l->data);

    g_list_free_full(*source, g_free);
    *source = NULL;
}

static void on_mpris_properties_changed(GDBusConnection *connection G_GNUC_UNUSED,
                                        const gchar *sender_name G_GNUC_UNUSED,
                                        const gchar *object_path G_GNUC_UNUSED,
                                        const gchar *interface_name G_GNUC_UNUSED,
                                        const gchar *signal_name G_GNUC_UNUSED,
                                        GVariant *parameters,
                                        gpointer user_data)
{
    MediaControl *mc = user_data;
    const gchar *changed_interface = NULL;
    GVariant *changed = NULL;
    GVariant *invalidated = NULL;

    g_variant_get(parameters, "(&s@a{sv}@as)",
                  &changed_interface, &changed, &invalidated);

    if (g_strcmp0(changed_interface, MPRIS_PLAYER_INTERFACE) == 0) {
        GVariant *status = g_variant_lookup_value(changed, "PlaybackStatus",
                                                  G_VARIANT_TYPE_STRING);
        if (status != NULL) {
            if (g_strcmp0(g_variant_get_string(status, NULL), "Playing") == 0) {
                if (wear_policy_blocks_playback(mc->ear_pause_mode,
                                                mc->prev_state_valid,
                                                mc->prev_left_in_ear,
                                                mc->prev_right_in_ear)) {
                    /* A player may be restarted repeatedly while the AirPods
                     * are off-head. Pause it again, retain pause ownership,
                     * and suppress the handoff callback so Linux does not
                     * claim the AirPods for inaudible playback. */
                    g_message("Media started while AirPods are not worn; pausing");
                    pause_all_into(mc, &mc->ear_paused_players, true);
                } else if (mc->playback_started_callback != NULL) {
                    mc->playback_started_callback(mc->playback_started_user_data);
                }
            }
            g_variant_unref(status);
        }
    }

    g_variant_unref(changed);
    g_variant_unref(invalidated);
}

/* ============================================================================
 * Helper functions
 * ========================================================================== */

static GList *get_mpris_players(MediaControl *mc)
{
    GList *players = NULL;
    GError *error = NULL;

    GVariant *result = g_dbus_connection_call_sync(
        mc->connection,
        "org.freedesktop.DBus",
        "/org/freedesktop/DBus",
        "org.freedesktop.DBus",
        "ListNames",
        NULL,
        G_VARIANT_TYPE("(as)"),
        G_DBUS_CALL_FLAGS_NONE,
        MPRIS_CALL_TIMEOUT_MSEC,
        NULL,
        &error);

    if (error != NULL) {
        g_warning("Failed to list D-Bus names: %s", error->message);
        g_error_free(error);
        return NULL;
    }

    GVariantIter *iter;
    const gchar *name;
    g_variant_get(result, "(as)", &iter);

    while (g_variant_iter_loop(iter, "&s", &name)) {
        if (g_str_has_prefix(name, MPRIS_DBUS_NAME_PREFIX)) {
            players = g_list_append(players, g_strdup(name));
        }
    }

    g_variant_iter_free(iter);
    g_variant_unref(result);

    return players;
}

static gchar *get_player_playback_status(MediaControl *mc, const gchar *player_name)
{
    GError *error = NULL;
    gchar *status = NULL;

    GVariant *result = g_dbus_connection_call_sync(
        mc->connection,
        player_name,
        MPRIS_DBUS_PATH,
        DBUS_PROPERTIES_INTERFACE,
        "Get",
        g_variant_new("(ss)", MPRIS_PLAYER_INTERFACE, "PlaybackStatus"),
        G_VARIANT_TYPE("(v)"),
        G_DBUS_CALL_FLAGS_NONE,
        MPRIS_CALL_TIMEOUT_MSEC,
        NULL,
        &error);

    if (error != NULL) {
        g_debug("Failed to get playback status from %s: %s", player_name, error->message);
        g_error_free(error);
        return NULL;
    }

    GVariant *variant;
    g_variant_get(result, "(v)", &variant);
    status = g_strdup(g_variant_get_string(variant, NULL));
    g_variant_unref(variant);
    g_variant_unref(result);

    return status;
}

static bool player_pause(MediaControl *mc, const gchar *player_name)
{
    GError *error = NULL;

    g_dbus_connection_call_sync(
        mc->connection,
        player_name,
        MPRIS_DBUS_PATH,
        MPRIS_PLAYER_INTERFACE,
        "Pause",
        NULL,
        NULL,
        G_DBUS_CALL_FLAGS_NONE,
        MPRIS_CALL_TIMEOUT_MSEC,
        NULL,
        &error);

    if (error != NULL) {
        g_debug("Failed to pause %s: %s", player_name, error->message);
        g_error_free(error);
        return false;
    }

    g_message("Paused media player: %s", player_name);
    return true;
}

static bool player_play(MediaControl *mc, const gchar *player_name)
{
    GError *error = NULL;

    g_dbus_connection_call_sync(
        mc->connection,
        player_name,
        MPRIS_DBUS_PATH,
        MPRIS_PLAYER_INTERFACE,
        "Play",
        NULL,
        NULL,
        G_DBUS_CALL_FLAGS_NONE,
        MPRIS_CALL_TIMEOUT_MSEC,
        NULL,
        &error);

    if (error != NULL) {
        g_debug("Failed to play %s: %s", player_name, error->message);
        g_error_free(error);
        return false;
    }

    g_message("Resumed media player: %s", player_name);
    return true;
}

/* ============================================================================
 * Public API
 * ========================================================================== */

MediaControl *media_control_new(void)
{
    MediaControl *mc = g_new0(MediaControl, 1);
    GError *error = NULL;

    mc->connection = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &error);
    if (error != NULL) {
        g_warning("Failed to connect to session bus: %s", error->message);
        g_error_free(error);
        g_free(mc);
        return NULL;
    }

    mc->ear_pause_mode = EAR_PAUSE_ONE_OUT;  /* Default: pause when one pod is removed */
    mc->ear_paused_players = NULL;
    mc->handoff_paused_players = NULL;
    mc->prev_state_valid = false;

    mc->properties_signal_id = g_dbus_connection_signal_subscribe(
        mc->connection,
        NULL,
        DBUS_PROPERTIES_INTERFACE,
        "PropertiesChanged",
        MPRIS_DBUS_PATH,
        MPRIS_PLAYER_INTERFACE,
        G_DBUS_SIGNAL_FLAGS_NONE,
        on_mpris_properties_changed,
        mc,
        NULL);

    return mc;
}

void media_control_free(MediaControl *mc)
{
    if (mc == NULL) {
        return;
    }

    g_list_free_full(mc->ear_paused_players, g_free);
    g_list_free_full(mc->handoff_paused_players, g_free);

    if (mc->connection) {
        if (mc->properties_signal_id > 0) {
            g_dbus_connection_signal_unsubscribe(mc->connection,
                                                  mc->properties_signal_id);
        }
        g_object_unref(mc->connection);
    }

    g_free(mc);
}

void media_control_set_ear_pause_mode(MediaControl *mc, EarPauseMode mode)
{
    if (mc == NULL)
        return;

    WearPolicyAction action = wear_policy_mode_change(
        mc->ear_pause_mode,
        mode,
        mc->prev_state_valid,
        mc->prev_left_in_ear,
        mc->prev_right_in_ear);

    mc->ear_pause_mode = mode;
    g_message("Ear pause mode set to: %d", mode);

    if (action == WEAR_POLICY_ACTION_PAUSE) {
        g_message("Ear pause mode now blocks the current wear state; pausing media");
        media_control_pause_all(mc);
    } else if (action == WEAR_POLICY_ACTION_RESUME) {
        g_message("Ear pause mode now allows the current wear state; resuming owned media");
        media_control_resume(mc);
    }
}

EarPauseMode media_control_get_ear_pause_mode(MediaControl *mc)
{
    return mc ? mc->ear_pause_mode : EAR_PAUSE_DISABLED;
}

void media_control_set_playback_started_callback(MediaControl *mc,
                                                 MediaPlaybackStartedCallback callback,
                                                 void *user_data)
{
    if (mc == NULL)
        return;

    mc->playback_started_callback = callback;
    mc->playback_started_user_data = user_data;
}

void media_control_reset_device_state(MediaControl *mc)
{
    if (mc == NULL)
        return;

    mc->prev_state_valid = false;
    g_list_free_full(mc->ear_paused_players, g_free);
    mc->ear_paused_players = NULL;
    g_list_free_full(mc->handoff_paused_players, g_free);
    mc->handoff_paused_players = NULL;
}

void media_control_on_ear_detection_changed(MediaControl *mc,
                                            bool left_in_ear,
                                            bool right_in_ear)
{
    if (mc == NULL) {
        return;
    }

    WearPolicyAction action = wear_policy_transition(
        mc->ear_pause_mode,
        mc->prev_state_valid,
        mc->prev_left_in_ear,
        mc->prev_right_in_ear,
        left_in_ear,
        right_in_ear);

    /* Update previous state */
    mc->prev_left_in_ear = left_in_ear;
    mc->prev_right_in_ear = right_in_ear;
    mc->prev_state_valid = true;

    /* Execute actions */
    if (action == WEAR_POLICY_ACTION_PAUSE) {
        g_message("Ear detection: pods removed, pausing media");
        media_control_pause_all(mc);
    } else if (action == WEAR_POLICY_ACTION_RESUME) {
        g_message("Ear detection: pods inserted, resuming media");
        media_control_resume(mc);
    }
}

static void pause_all_into(MediaControl *mc,
                           GList **paused_players,
                           bool preserve_existing)
{
    if (mc == NULL || mc->connection == NULL) {
        return;
    }

    if (!preserve_existing) {
        g_list_free_full(*paused_players, g_free);
        *paused_players = NULL;
    }

    /* Get all MPRIS players */
    GList *players = get_mpris_players(mc);

    for (GList *l = players; l != NULL; l = l->next) {
        const gchar *player_name = l->data;

        /* Check if player is currently playing */
        gchar *status = get_player_playback_status(mc, player_name);
        if (status != NULL && g_strcmp0(status, "Playing") == 0) {
            /* Pause this player and remember it */
            if (player_pause(mc, player_name))
                player_list_add_unique(paused_players, player_name);
        }
        g_free(status);
    }

    g_list_free_full(players, g_free);
}

void media_control_pause_all(MediaControl *mc)
{
    if (mc != NULL)
        pause_all_into(mc, &mc->ear_paused_players, true);
}

void media_control_pause_all_for_handoff(MediaControl *mc)
{
    if (mc != NULL)
        pause_all_into(mc, &mc->handoff_paused_players, true);
}

bool media_control_is_playing(MediaControl *mc)
{
    if (mc == NULL || mc->connection == NULL)
        return false;

    bool playing = false;
    GList *players = get_mpris_players(mc);

    for (GList *l = players; l != NULL && !playing; l = l->next) {
        gchar *status = get_player_playback_status(mc, l->data);
        playing = status != NULL && g_strcmp0(status, "Playing") == 0;
        g_free(status);
    }

    g_list_free_full(players, g_free);
    return playing;
}

bool media_control_wear_state_blocks_playback(MediaControl *mc)
{
    if (mc == NULL)
        return false;

    return wear_policy_blocks_playback(mc->ear_pause_mode,
                                       mc->prev_state_valid,
                                       mc->prev_left_in_ear,
                                       mc->prev_right_in_ear);
}

static gchar *find_bluez_sink(const char *device_address)
{
    if (device_address == NULL || device_address[0] == '\0')
        return NULL;

    gchar *address_token = g_strdup(device_address);
    for (gchar *p = address_token; *p != '\0'; p++) {
        if (*p == ':')
            *p = '_';
    }

    gchar *standard_output = NULL;
    gchar *standard_error = NULL;
    gint wait_status = 0;
    GError *error = NULL;
    gchar *argv[] = {"pactl", "list", "sinks", "short", NULL};

    gboolean spawned = g_spawn_sync(NULL, argv, NULL, G_SPAWN_SEARCH_PATH,
                                    NULL, NULL, &standard_output,
                                    &standard_error, &wait_status, &error);
    if (!spawned || !g_spawn_check_wait_status(wait_status, &error)) {
        g_debug("Could not query audio sinks for handoff: %s",
                error ? error->message : "pactl failed");
        g_clear_error(&error);
        g_free(standard_output);
        g_free(standard_error);
        g_free(address_token);
        return NULL;
    }

    gchar *sink_name = NULL;
    gchar **lines = g_strsplit(standard_output, "\n", -1);
    for (gchar **line = lines; *line != NULL; line++) {
        if (g_strstr_len(*line, -1, "bluez") == NULL ||
            g_strstr_len(*line, -1, address_token) == NULL) {
            continue;
        }

        gchar *first_tab = strchr(*line, '\t');
        gchar *second_tab = first_tab ? strchr(first_tab + 1, '\t') : NULL;
        if (first_tab != NULL && second_tab != NULL && second_tab > first_tab + 1) {
            sink_name = g_strndup(first_tab + 1, (gsize)(second_tab - first_tab - 1));
            break;
        }
    }

    g_strfreev(lines);
    g_free(standard_output);
    g_free(standard_error);
    g_free(address_token);
    return sink_name;
}

static bool set_sink_suspended(const char *sink_name, bool suspended)
{
    gchar *standard_error = NULL;
    gint wait_status = 0;
    GError *error = NULL;
    gchar *argv[] = {
        "pactl", "suspend-sink", (gchar *)sink_name,
        suspended ? "1" : "0", NULL
    };

    gboolean spawned = g_spawn_sync(NULL, argv, NULL, G_SPAWN_SEARCH_PATH,
                                    NULL, NULL, NULL, &standard_error,
                                    &wait_status, &error);
    bool success = spawned && g_spawn_check_wait_status(wait_status, &error);
    if (!success) {
        g_debug("Could not %s AirPods sink: %s%s%s",
                suspended ? "suspend" : "resume",
                error ? error->message : "pactl failed",
                standard_error && standard_error[0] ? ": " : "",
                standard_error && standard_error[0] ? standard_error : "");
    }

    g_clear_error(&error);
    g_free(standard_error);
    return success;
}

bool media_control_reclaim_audio(MediaControl *mc, const char *device_address)
{
    if (mc == NULL)
        return false;

    gchar *sink_name = find_bluez_sink(device_address);
    if (sink_name == NULL) {
        g_debug("No active AirPods sink found for handoff");
        return false;
    }

    bool suspended = set_sink_suspended(sink_name, true);
    if (suspended) {
        g_usleep(200000);
    }
    bool resumed = suspended && set_sink_suspended(sink_name, false);
    g_free(sink_name);

    if (resumed)
        g_message("Restarted AirPods audio sink after ownership claim");

    return resumed;
}

void media_control_resume(MediaControl *mc)
{
    if (mc == NULL || mc->connection == NULL) {
        return;
    }

    /* Resume only players that we paused */
    for (GList *l = mc->ear_paused_players; l != NULL; l = l->next) {
        const gchar *player_name = l->data;
        if (!player_list_contains(mc->handoff_paused_players, player_name))
            player_play(mc, player_name);
    }

    /* Clear the paused list */
    g_list_free_full(mc->ear_paused_players, g_free);
    mc->ear_paused_players = NULL;
}

MediaHandoffResumeResult media_control_resume_handoff(MediaControl *mc)
{
    if (mc == NULL || mc->connection == NULL)
        return MEDIA_HANDOFF_RESUME_NONE;

    if (mc->handoff_paused_players == NULL)
        return MEDIA_HANDOFF_RESUME_NONE;

    bool ear_state_allows_resume = true;
    if (mc->prev_state_valid && mc->ear_pause_mode != EAR_PAUSE_DISABLED) {
        if (mc->ear_pause_mode == EAR_PAUSE_ONE_OUT) {
            ear_state_allows_resume = mc->prev_left_in_ear &&
                                      mc->prev_right_in_ear;
        } else if (mc->ear_pause_mode == EAR_PAUSE_BOTH_OUT) {
            ear_state_allows_resume = mc->prev_left_in_ear ||
                                      mc->prev_right_in_ear;
        }
    }

    if (!ear_state_allows_resume) {
        /* Preserve pause ownership, but transfer it to the ear-detection
         * reason. The next out->in transition resumes exactly these players. */
        transfer_player_ownership_unique(&mc->ear_paused_players,
                                         &mc->handoff_paused_players);
        g_message("Deferring handoff resume until AirPods are worn again");
        return MEDIA_HANDOFF_RESUME_DEFERRED_FOR_EAR_STATE;
    }

    bool resumed = false;
    bool deferred_for_other_reason = false;
    for (GList *l = mc->handoff_paused_players; l != NULL; l = l->next) {
        if (player_list_contains(mc->ear_paused_players, l->data)) {
            deferred_for_other_reason = true;
        } else if (player_play(mc, l->data)) {
            resumed = true;
        }
    }

    g_list_free_full(mc->handoff_paused_players, g_free);
    mc->handoff_paused_players = NULL;
    if (resumed)
        return MEDIA_HANDOFF_RESUME_STARTED;
    if (deferred_for_other_reason)
        return MEDIA_HANDOFF_RESUME_DEFERRED_FOR_EAR_STATE;
    return MEDIA_HANDOFF_RESUME_NONE;
}

bool media_control_has_handoff_paused(MediaControl *mc)
{
    return mc != NULL && mc->handoff_paused_players != NULL;
}
