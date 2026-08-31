/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2024 EarPort Contributors
 *
 * Media control via MPRIS D-Bus interface
 */

#include "media_control.h"
#include "audio_route.h"
#include "media_playback_tracker.h"
#include <gio/gio.h>
#include <string.h>

#define MPRIS_DBUS_NAME_PREFIX "org.mpris.MediaPlayer2."
#define MPRIS_DBUS_PATH "/org/mpris/MediaPlayer2"
#define MPRIS_PLAYER_INTERFACE "org.mpris.MediaPlayer2.Player"
#define DBUS_PROPERTIES_INTERFACE "org.freedesktop.DBus.Properties"
#define MPRIS_CALL_TIMEOUT_MSEC 2000

typedef enum {
    MPRIS_ACTION_PAUSE_FOR_EAR,
    MPRIS_ACTION_PAUSE_FOR_HANDOFF,
    MPRIS_ACTION_PLAY,
} MprisAction;

typedef struct {
    MediaControl *mc;
    guint generation;
    guint pending_serial;
    MprisAction action;
    gchar *player_name;
} MprisOperation;

typedef struct {
    MediaControl *mc;
    guint serial;
    bool suppress_transition;
    gchar *player_name;
} MprisStatusOperation;

struct MediaControl {
    gint ref_count;
    bool disposed;
    GDBusConnection *connection;
    AudioRoute *audio_route;
    guint properties_signal_id;
    guint name_owner_signal_id;
    EarPauseMode ear_pause_mode;
    MediaPlaybackTracker *playback_tracker;
    bool airpods_link_active;
    bool airpods_audio_active;

    MediaPlaybackStartedCallback playback_started_callback;
    void *playback_started_user_data;
    MediaPlaybackStoppedCallback playback_stopped_callback;
    void *playback_stopped_user_data;

    /* Track pause ownership separately so an ear event cannot resume a player
     * paused because an Apple device took the AirPods audio source. */
    GList *ear_paused_players;
    GList *handoff_paused_players;

    /* A Pause request becomes owned only after MPRIS acknowledges it.  These
     * tables cover the interval before that reply, allowing a quick remove ->
     * wear transition to queue Play after the in-flight Pause without ever
     * blocking the GLib main loop. Values are non-zero operation serials. */
    GHashTable *ear_pending_pauses;
    GHashTable *handoff_pending_pauses;
    GHashTable *pending_status_queries;
    GCancellable *mpris_cancellable;
    GCancellable *status_cancellable;
    guint mpris_generation;
    guint next_operation_serial;

    /* Previous ear state for edge detection */
    bool prev_left_in_ear;
    bool prev_right_in_ear;
    bool prev_state_valid;
};

static void pause_all_into(MediaControl *mc,
                           MprisAction action,
                           bool preserve_existing);
static void query_player_playback_status_async(MediaControl *mc,
                                               const gchar *player_name,
                                               bool suppress_transition);
static bool player_play_async(MediaControl *mc, const gchar *player_name);
static bool player_list_contains(GList *players, const gchar *player_name);
static void player_list_remove(GList **players, const gchar *player_name);

static MediaControl *media_control_ref(MediaControl *mc)
{
    g_atomic_int_inc(&mc->ref_count);
    return mc;
}

static void media_control_unref(MediaControl *mc)
{
    if (!g_atomic_int_dec_and_test(&mc->ref_count))
        return;

    g_list_free_full(mc->ear_paused_players, g_free);
    g_list_free_full(mc->handoff_paused_players, g_free);
    g_clear_pointer(&mc->ear_pending_pauses, g_hash_table_unref);
    g_clear_pointer(&mc->handoff_pending_pauses, g_hash_table_unref);
    g_clear_pointer(&mc->pending_status_queries, g_hash_table_unref);
    g_clear_object(&mc->mpris_cancellable);
    g_clear_object(&mc->status_cancellable);
    media_playback_tracker_free(mc->playback_tracker);
    audio_route_free(mc->audio_route);
    g_clear_object(&mc->connection);
    g_free(mc);
}

static void reconcile_explicit_player_start(MediaControl *mc,
                                            const gchar *player_name)
{
    if (player_name == NULL || player_name[0] == '\0')
        return;

    bool ear_pause_pending =
        g_hash_table_contains(mc->ear_pending_pauses, player_name);
    bool ear_pause_owned =
        player_list_contains(mc->ear_paused_players, player_name);
    bool handoff_pause_pending =
        g_hash_table_contains(mc->handoff_pending_pauses, player_name);
    bool handoff_pause_owned =
        player_list_contains(mc->handoff_paused_players, player_name);
    if (!ear_pause_pending && !ear_pause_owned &&
        !handoff_pause_pending && !handoff_pause_owned) {
        return;
    }

    /* The user explicitly restarted this player. If either Pause is still in
     * flight, queue one Play for this same D-Bus owner so it is ordered after
     * both calls. Never wake another player. */
    if (ear_pause_pending || handoff_pause_pending)
        player_play_async(mc, player_name);

    g_hash_table_remove(mc->ear_pending_pauses, player_name);
    g_hash_table_remove(mc->handoff_pending_pauses, player_name);
    player_list_remove(&mc->ear_paused_players, player_name);
    player_list_remove(&mc->handoff_paused_players, player_name);
    g_debug("Explicit playback start cleared pause ownership for %s",
            player_name);
}

static void handle_playback_update(MediaControl *mc,
                                   MediaPlaybackTransition transition,
                                   const gchar *player_name,
                                   bool player_started)
{
    if (player_started)
        reconcile_explicit_player_start(mc, player_name);

    if (transition == MEDIA_PLAYBACK_TRANSITION_STARTED) {
        /* Ear removal is an edge-triggered pause. A later explicit MPRIS
         * start is allowed to continue on speakers; claim/reroute policy in
         * main independently fails closed while wear is unknown or blocked. */
        if (mc->playback_started_callback != NULL) {
            MediaPlaybackStartedCallback callback =
                mc->playback_started_callback;
            void *user_data = mc->playback_started_user_data;
            callback(user_data);
        }
    } else if (transition == MEDIA_PLAYBACK_TRANSITION_ALL_STOPPED &&
               mc->playback_stopped_callback != NULL) {
        MediaPlaybackStoppedCallback callback =
            mc->playback_stopped_callback;
        void *user_data = mc->playback_stopped_user_data;
        callback(user_data);
    }
}

static bool player_list_contains(GList *players, const gchar *player_name)
{
    return g_list_find_custom(players,
                              player_name,
                              (GCompareFunc)g_strcmp0) != NULL;
}

static void player_list_add_unique(GList **players, const gchar *player_name)
{
    if (players == NULL || player_name == NULL)
        return;

    if (!player_list_contains(*players, player_name))
        *players = g_list_append(*players, g_strdup(player_name));
}

static GList **paused_players_for_action(MediaControl *mc,
                                         MprisAction action)
{
    if (action == MPRIS_ACTION_PAUSE_FOR_EAR)
        return &mc->ear_paused_players;
    if (action == MPRIS_ACTION_PAUSE_FOR_HANDOFF)
        return &mc->handoff_paused_players;
    return NULL;
}

static GHashTable *pending_pauses_for_action(MediaControl *mc,
                                             MprisAction action)
{
    if (action == MPRIS_ACTION_PAUSE_FOR_EAR)
        return mc->ear_pending_pauses;
    if (action == MPRIS_ACTION_PAUSE_FOR_HANDOFF)
        return mc->handoff_pending_pauses;
    return NULL;
}

static guint next_operation_serial(MediaControl *mc)
{
    mc->next_operation_serial++;
    if (mc->next_operation_serial == 0)
        mc->next_operation_serial++;
    return mc->next_operation_serial;
}

static void on_mpris_properties_changed(GDBusConnection *connection G_GNUC_UNUSED,
                                        const gchar *sender_name,
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
    MediaPlaybackTransition transition = MEDIA_PLAYBACK_TRANSITION_NONE;
    bool player_started = false;
    bool playing = false;

    g_variant_get(parameters, "(&s@a{sv}@as)",
                  &changed_interface, &changed, &invalidated);

    if (g_strcmp0(changed_interface, MPRIS_PLAYER_INTERFACE) == 0) {
        GVariant *status = g_variant_lookup_value(changed, "PlaybackStatus",
                                                  G_VARIANT_TYPE_STRING);
        if (status != NULL) {
            /* A signal is newer and more authoritative than an outstanding
             * one-shot Get issued when this owner appeared. */
            g_hash_table_remove(mc->pending_status_queries, sender_name);
            playing = g_strcmp0(g_variant_get_string(status, NULL),
                                "Playing") == 0;
            bool was_playing = media_playback_tracker_player_is_playing(
                mc->playback_tracker, sender_name);
            transition = media_playback_tracker_update(mc->playback_tracker,
                                                       sender_name,
                                                       playing);
            player_started = playing && !was_playing;
            g_variant_unref(status);
        }
    }

    g_variant_unref(changed);
    g_variant_unref(invalidated);
    handle_playback_update(mc,
                           transition,
                           player_started ? sender_name : NULL,
                           player_started);
}

static void player_list_remove(GList **players, const gchar *player_name)
{
    GList *entry = g_list_find_custom(*players, player_name,
                                      (GCompareFunc)g_strcmp0);
    if (entry == NULL)
        return;

    g_free(entry->data);
    *players = g_list_delete_link(*players, entry);
}

static void on_name_owner_changed(GDBusConnection *connection G_GNUC_UNUSED,
                                  const gchar *sender_name G_GNUC_UNUSED,
                                  const gchar *object_path G_GNUC_UNUSED,
                                  const gchar *interface_name G_GNUC_UNUSED,
                                  const gchar *signal_name G_GNUC_UNUSED,
                                  GVariant *parameters,
                                  gpointer user_data)
{
    MediaControl *mc = user_data;
    const gchar *name = NULL;
    const gchar *old_owner = NULL;
    const gchar *new_owner = NULL;

    g_variant_get(parameters, "(&s&s&s)", &name, &old_owner, &new_owner);
    if (!g_str_has_prefix(name, MPRIS_DBUS_NAME_PREFIX))
        return;

    bool old_owner_was_playing = false;
    MediaPlaybackTransition removal_transition =
        MEDIA_PLAYBACK_TRANSITION_NONE;
    if (old_owner[0] != '\0') {
        old_owner_was_playing =
            media_playback_tracker_player_is_playing(mc->playback_tracker,
                                                       old_owner);
        removal_transition = media_playback_tracker_remove(
            mc->playback_tracker, old_owner);
        player_list_remove(&mc->ear_paused_players, old_owner);
        player_list_remove(&mc->handoff_paused_players, old_owner);
        g_hash_table_remove(mc->ear_pending_pauses, old_owner);
        g_hash_table_remove(mc->handoff_pending_pauses, old_owner);
        g_hash_table_remove(mc->pending_status_queries, old_owner);
    }

    if (new_owner[0] != '\0') {
        /* Preserve the old owner's cached state while its replacement is
         * queried. This avoids a false all-stopped -> started edge on normal
         * MPRIS owner replacement, without blocking the main loop. */
        media_playback_tracker_update(mc->playback_tracker,
                                      new_owner,
                                      old_owner_was_playing);
        query_player_playback_status_async(mc, new_owner, false);
    } else {
        handle_playback_update(mc, removal_transition, NULL, false);
    }
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

static void mpris_status_operation_free(MprisStatusOperation *operation)
{
    g_free(operation->player_name);
    media_control_unref(operation->mc);
    g_free(operation);
}

static void on_player_status_finished(GObject *source_object,
                                      GAsyncResult *result,
                                      gpointer user_data)
{
    MprisStatusOperation *operation = user_data;
    MediaControl *mc = operation->mc;
    GError *error = NULL;
    GVariant *reply = g_dbus_connection_call_finish(
        G_DBUS_CONNECTION(source_object), result, &error);
    gpointer value = !mc->disposed
                         ? g_hash_table_lookup(mc->pending_status_queries,
                                               operation->player_name)
                         : NULL;
    bool current = !mc->disposed &&
                   GPOINTER_TO_UINT(value) == operation->serial;

    if (current) {
        g_hash_table_remove(mc->pending_status_queries,
                            operation->player_name);

        bool valid_status = false;
        bool playing = false;
        if (reply != NULL && g_variant_n_children(reply) == 1) {
            GVariant *boxed = g_variant_get_child_value(reply, 0);
            if (g_variant_is_of_type(boxed, G_VARIANT_TYPE_VARIANT)) {
                GVariant *status = g_variant_get_variant(boxed);
                if (g_variant_is_of_type(status, G_VARIANT_TYPE_STRING)) {
                    playing = g_strcmp0(g_variant_get_string(status, NULL),
                                        "Playing") == 0;
                    valid_status = true;
                }
                g_variant_unref(status);
            }
            g_variant_unref(boxed);
        }

        bool was_playing = media_playback_tracker_player_is_playing(
            mc->playback_tracker, operation->player_name);
        MediaPlaybackTransition transition = valid_status
            ? media_playback_tracker_update(mc->playback_tracker,
                                            operation->player_name,
                                            playing)
            : media_playback_tracker_remove(mc->playback_tracker,
                                            operation->player_name);
        if (!operation->suppress_transition)
            handle_playback_update(mc,
                                   transition,
                                   operation->player_name,
                                   valid_status && playing && !was_playing);

        if (!valid_status && error != NULL &&
            !g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
            g_debug("Failed to get playback status from %s: %s",
                    operation->player_name,
                    error->message);
        }
    }

    g_clear_pointer(&reply, g_variant_unref);
    g_clear_error(&error);
    mpris_status_operation_free(operation);
}

static void query_player_playback_status_async(MediaControl *mc,
                                               const gchar *player_name,
                                               bool suppress_transition)
{
    if (mc == NULL || mc->disposed || player_name == NULL ||
        player_name[0] == '\0') {
        return;
    }

    guint serial = next_operation_serial(mc);
    g_hash_table_insert(mc->pending_status_queries,
                        g_strdup(player_name),
                        GUINT_TO_POINTER(serial));

    MprisStatusOperation *operation = g_new0(MprisStatusOperation, 1);
    operation->mc = media_control_ref(mc);
    operation->serial = serial;
    operation->suppress_transition = suppress_transition;
    operation->player_name = g_strdup(player_name);

    g_dbus_connection_call(mc->connection,
                           player_name,
                           MPRIS_DBUS_PATH,
                           DBUS_PROPERTIES_INTERFACE,
                           "Get",
                           g_variant_new("(ss)",
                                         MPRIS_PLAYER_INTERFACE,
                                         "PlaybackStatus"),
                           G_VARIANT_TYPE("(v)"),
                           G_DBUS_CALL_FLAGS_NONE,
                           MPRIS_CALL_TIMEOUT_MSEC,
                           mc->status_cancellable,
                           on_player_status_finished,
                           operation);
}

static gchar *get_name_owner(MediaControl *mc, const gchar *name)
{
    GError *error = NULL;
    GVariant *result = g_dbus_connection_call_sync(
        mc->connection,
        "org.freedesktop.DBus",
        "/org/freedesktop/DBus",
        "org.freedesktop.DBus",
        "GetNameOwner",
        g_variant_new("(s)", name),
        G_VARIANT_TYPE("(s)"),
        G_DBUS_CALL_FLAGS_NONE,
        MPRIS_CALL_TIMEOUT_MSEC,
        NULL,
        &error);

    if (error != NULL) {
        g_debug("Failed to resolve D-Bus owner for %s: %s", name,
                error->message);
        g_error_free(error);
        return NULL;
    }

    const gchar *owner = NULL;
    g_variant_get(result, "(&s)", &owner);
    gchar *copy = g_strdup(owner);
    g_variant_unref(result);
    return copy;
}

static void seed_playback_cache(MediaControl *mc)
{
    GList *players = get_mpris_players(mc);
    for (GList *l = players; l != NULL; l = l->next) {
        gchar *owner = get_name_owner(mc, l->data);
        if (owner == NULL)
            continue;

        /* Player calls are intentionally asynchronous even during startup.
         * The local bus-daemon lookups above are bounded and inexpensive;
         * unresponsive media players cannot delay daemon readiness. */
        query_player_playback_status_async(mc, owner, true);
        g_free(owner);
    }
    g_list_free_full(players, g_free);
}

static void mpris_operation_free(MprisOperation *operation)
{
    g_free(operation->player_name);
    media_control_unref(operation->mc);
    g_free(operation);
}

static void on_mpris_method_finished(GObject *source_object,
                                     GAsyncResult *result,
                                     gpointer user_data)
{
    MprisOperation *operation = user_data;
    MediaControl *mc = operation->mc;
    GError *error = NULL;
    GVariant *reply = g_dbus_connection_call_finish(
        G_DBUS_CONNECTION(source_object), result, &error);
    bool successful = reply != NULL;
    bool current = !mc->disposed &&
                   operation->generation == mc->mpris_generation;

    if (current && operation->action != MPRIS_ACTION_PLAY &&
        operation->pending_serial != 0) {
        GHashTable *pending = pending_pauses_for_action(
            mc, operation->action);
        gpointer value = pending != NULL
            ? g_hash_table_lookup(pending, operation->player_name)
            : NULL;
        bool pending_is_current =
            pending != NULL &&
            GPOINTER_TO_UINT(value) == operation->pending_serial;
        if (pending_is_current) {
            g_hash_table_remove(pending, operation->player_name);
            if (successful) {
                GList **paused = paused_players_for_action(
                    mc, operation->action);
                player_list_add_unique(paused, operation->player_name);
            }
        }
    }

    if (current && successful) {
        g_message("%s media player: %s",
                  operation->action == MPRIS_ACTION_PLAY
                      ? "Resumed"
                      : "Paused",
                  operation->player_name);
    } else if (current && error != NULL &&
               !g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
        g_debug("Failed to %s %s: %s",
                operation->action == MPRIS_ACTION_PLAY ? "play" : "pause",
                operation->player_name,
                error->message);
    }

    g_clear_pointer(&reply, g_variant_unref);
    g_clear_error(&error);
    mpris_operation_free(operation);
}

static bool queue_mpris_method(MediaControl *mc,
                               const gchar *player_name,
                               MprisAction action,
                               guint pending_serial)
{
    if (mc == NULL || mc->disposed || mc->connection == NULL ||
        player_name == NULL || player_name[0] == '\0') {
        return false;
    }

    MprisOperation *operation = g_new0(MprisOperation, 1);
    operation->mc = media_control_ref(mc);
    operation->generation = mc->mpris_generation;
    operation->pending_serial = pending_serial;
    operation->action = action;
    operation->player_name = g_strdup(player_name);

    g_dbus_connection_call(
        mc->connection,
        player_name,
        MPRIS_DBUS_PATH,
        MPRIS_PLAYER_INTERFACE,
        action == MPRIS_ACTION_PLAY ? "Play" : "Pause",
        NULL,
        NULL,
        G_DBUS_CALL_FLAGS_NONE,
        MPRIS_CALL_TIMEOUT_MSEC,
        mc->mpris_cancellable,
        on_mpris_method_finished,
        operation);
    return true;
}

static bool player_pause_async(MediaControl *mc,
                               const gchar *player_name,
                               MprisAction action)
{
    GList **paused = paused_players_for_action(mc, action);
    GHashTable *pending = pending_pauses_for_action(mc, action);
    if (paused == NULL || pending == NULL)
        return false;

    guint serial = 0;
    if (!player_list_contains(*paused, player_name)) {
        /* Do not issue duplicate Pauses while ownership is awaiting an ACK.
         * Once owned, issuing another Pause is intentional: the player may
         * have been manually restarted while the AirPods are off-head. */
        if (g_hash_table_contains(pending, player_name))
            return true;

        serial = next_operation_serial(mc);
        g_hash_table_insert(pending,
                            g_strdup(player_name),
                            GUINT_TO_POINTER(serial));
    }

    if (queue_mpris_method(mc, player_name, action, serial))
        return true;

    if (serial != 0)
        g_hash_table_remove(pending, player_name);
    return false;
}

static bool player_play_async(MediaControl *mc, const gchar *player_name)
{
    return queue_mpris_method(mc,
                              player_name,
                              MPRIS_ACTION_PLAY,
                              0);
}

/* ============================================================================
 * Public API
 * ========================================================================== */

MediaControl *media_control_new(void)
{
    MediaControl *mc = g_new0(MediaControl, 1);
    GError *error = NULL;

    mc->ref_count = 1;

    mc->connection = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &error);
    if (error != NULL) {
        g_warning("Failed to connect to session bus: %s", error->message);
        g_error_free(error);
        g_free(mc);
        return NULL;
    }

    mc->audio_route = audio_route_new();
    mc->playback_tracker = media_playback_tracker_new();
    mc->ear_pending_pauses = g_hash_table_new_full(g_str_hash,
                                                    g_str_equal,
                                                    g_free,
                                                    NULL);
    mc->handoff_pending_pauses = g_hash_table_new_full(g_str_hash,
                                                        g_str_equal,
                                                        g_free,
                                                        NULL);
    mc->pending_status_queries = g_hash_table_new_full(g_str_hash,
                                                        g_str_equal,
                                                        g_free,
                                                        NULL);
    mc->mpris_cancellable = g_cancellable_new();
    mc->status_cancellable = g_cancellable_new();

    mc->ear_pause_mode = EAR_PAUSE_ONE_OUT;  /* Default: pause when one pod is removed */
    mc->airpods_link_active = false;
    mc->airpods_audio_active = false;
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

    mc->name_owner_signal_id = g_dbus_connection_signal_subscribe(
        mc->connection,
        "org.freedesktop.DBus",
        "org.freedesktop.DBus",
        "NameOwnerChanged",
        "/org/freedesktop/DBus",
        NULL,
        G_DBUS_SIGNAL_FLAGS_NONE,
        on_name_owner_changed,
        mc,
        NULL);

    seed_playback_cache(mc);

    return mc;
}

void media_control_free(MediaControl *mc)
{
    if (mc == NULL || mc->disposed) {
        return;
    }

    mc->disposed = true;
    mc->mpris_generation++;
    g_cancellable_cancel(mc->mpris_cancellable);
    g_cancellable_cancel(mc->status_cancellable);
    audio_route_cancel(mc->audio_route);

    if (mc->connection) {
        if (mc->properties_signal_id > 0) {
            g_dbus_connection_signal_unsubscribe(mc->connection,
                                                  mc->properties_signal_id);
        }
        if (mc->name_owner_signal_id > 0) {
            g_dbus_connection_signal_unsubscribe(mc->connection,
                                                  mc->name_owner_signal_id);
        }
    }

    mc->properties_signal_id = 0;
    mc->name_owner_signal_id = 0;
    mc->playback_started_callback = NULL;
    mc->playback_started_user_data = NULL;
    mc->playback_stopped_callback = NULL;
    mc->playback_stopped_user_data = NULL;
    media_control_unref(mc);
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

void media_control_set_playback_stopped_callback(MediaControl *mc,
                                                 MediaPlaybackStoppedCallback callback,
                                                 void *user_data)
{
    if (mc == NULL)
        return;

    mc->playback_stopped_callback = callback;
    mc->playback_stopped_user_data = user_data;
}

void media_control_set_airpods_link_active(MediaControl *mc, bool active)
{
    if (mc == NULL || mc->airpods_link_active == active)
        return;

    mc->airpods_link_active = active;

    if (!active) {
        /* AAP wear reports describe only the session that delivered them.
         * Fail open immediately when that session ends.  Pause ownership is
         * intentionally kept: if removal caused the disconnect, the first
         * fresh worn report after reconnect resumes only our own players. */
        mc->prev_state_valid = false;
        mc->airpods_audio_active = false;
        audio_route_restore_previous(mc->audio_route);
    }
}

void media_control_set_airpods_audio_active(MediaControl *mc, bool active)
{
    if (mc == NULL)
        return;

    /* Source notifications are meaningful only inside the current AAP
     * session.  Refuse to carry a positive value across link generations. */
    mc->airpods_audio_active = active && mc->airpods_link_active;
}

void media_control_reset_device_state(MediaControl *mc)
{
    if (mc == NULL)
        return;

    audio_route_restore_previous(mc->audio_route);

    /* Invalidate callbacks from the previous AirPods session. Calls already
     * delivered to a player may still finish, but their replies cannot claim
     * pause ownership in the new session. */
    mc->mpris_generation++;
    g_cancellable_cancel(mc->mpris_cancellable);
    g_clear_object(&mc->mpris_cancellable);
    mc->mpris_cancellable = g_cancellable_new();

    mc->airpods_link_active = false;
    mc->airpods_audio_active = false;
    mc->prev_state_valid = false;
    g_list_free_full(mc->ear_paused_players, g_free);
    mc->ear_paused_players = NULL;
    g_list_free_full(mc->handoff_paused_players, g_free);
    mc->handoff_paused_players = NULL;
    g_hash_table_remove_all(mc->ear_pending_pauses);
    g_hash_table_remove_all(mc->handoff_pending_pauses);
}

void media_control_on_ear_detection_changed(MediaControl *mc,
                                            bool left_in_ear,
                                            bool right_in_ear)
{
    if (mc == NULL || !mc->airpods_link_active) {
        return;
    }

    bool previous_state_valid = mc->prev_state_valid;
    WearPolicyAction action = wear_policy_transition(
        mc->ear_pause_mode,
        previous_state_valid,
        mc->prev_left_in_ear,
        mc->prev_right_in_ear,
        left_in_ear,
        right_in_ear);

    /* Update previous state */
    mc->prev_left_in_ear = left_in_ear;
    mc->prev_right_in_ear = right_in_ear;
    mc->prev_state_valid = true;

    /* Link loss invalidates wear state but deliberately retains the players
     * paused for a removal cycle.  A fresh first report that says "worn"
     * completes that cycle without relying on stale sensor values. */
    if (!previous_state_valid && action == WEAR_POLICY_ACTION_NONE &&
        !wear_policy_blocks_playback(mc->ear_pause_mode,
                                     true,
                                     left_in_ear,
                                     right_in_ear) &&
        (mc->ear_paused_players != NULL ||
         g_hash_table_size(mc->ear_pending_pauses) > 0)) {
        action = WEAR_POLICY_ACTION_RESUME;
    }

    /* Execute actions */
    if (action == WEAR_POLICY_ACTION_PAUSE) {
        if (mc->airpods_audio_active) {
            g_message("Ear detection: pods removed, pausing media");
            media_control_pause_all(mc);
        } else {
            g_debug("AirPods are not the confirmed Linux audio source; leaving speaker media untouched");
        }
    } else if (action == WEAR_POLICY_ACTION_RESUME) {
        g_message("Ear detection: pods inserted, resuming media");
        media_control_resume(mc);
    }
}

static void pause_all_into(MediaControl *mc,
                           MprisAction action,
                           bool preserve_existing)
{
    if (mc == NULL || mc->connection == NULL) {
        return;
    }

    GList **paused_players = paused_players_for_action(mc, action);
    GHashTable *pending = pending_pauses_for_action(mc, action);
    if (paused_players == NULL || pending == NULL)
        return;

    if (!preserve_existing) {
        g_list_free_full(*paused_players, g_free);
        *paused_players = NULL;
        g_hash_table_remove_all(pending);
    }

    /* Work from the signal-maintained cache so a playback event never fans
     * out into synchronous ListNames/Get calls. */
    GPtrArray *players =
        media_playback_tracker_dup_playing_ids(mc->playback_tracker);
    for (guint i = 0; i < players->len; i++) {
        const gchar *player_name = g_ptr_array_index(players, i);
        player_pause_async(mc, player_name, action);
    }
    g_ptr_array_unref(players);
}

void media_control_pause_all(MediaControl *mc)
{
    if (mc != NULL && mc->airpods_link_active && mc->airpods_audio_active)
        pause_all_into(mc, MPRIS_ACTION_PAUSE_FOR_EAR, true);
}

void media_control_pause_all_for_handoff(MediaControl *mc)
{
    if (mc != NULL && mc->airpods_link_active)
        pause_all_into(mc, MPRIS_ACTION_PAUSE_FOR_HANDOFF, true);
}

bool media_control_is_playing(MediaControl *mc)
{
    return mc != NULL &&
           media_playback_tracker_is_playing(mc->playback_tracker);
}

void media_control_clear_handoff_pause(MediaControl *mc)
{
    if (mc == NULL)
        return;

    g_list_free_full(mc->handoff_paused_players, g_free);
    mc->handoff_paused_players = NULL;
    g_hash_table_remove_all(mc->handoff_pending_pauses);
}

bool media_control_wear_state_blocks_playback(MediaControl *mc)
{
    if (mc == NULL || !mc->airpods_link_active)
        return false;

    return wear_policy_blocks_playback(mc->ear_pause_mode,
                                       mc->prev_state_valid,
                                       mc->prev_left_in_ear,
                                       mc->prev_right_in_ear);
}

bool media_control_can_claim_or_route_audio(MediaControl *mc)
{
    return mc != NULL &&
           mc->airpods_link_active &&
           mc->prev_state_valid &&
           (mc->prev_left_in_ear || mc->prev_right_in_ear);
}

bool media_control_reclaim_audio(MediaControl *mc, const char *device_address)
{
    if (!media_control_can_claim_or_route_audio(mc))
        return false;

    /* Reuse the bounded, cancellable A2DP route selector.  The old reclaim
     * path synchronously ran pactl twice and slept on the GLib main thread;
     * it could also suspend a headset-profile sink. */
    return audio_route_start(mc->audio_route, device_address);
}

void media_control_cancel_audio_route(MediaControl *mc)
{
    if (mc == NULL)
        return;

    audio_route_cancel(mc->audio_route);
}

void media_control_restore_audio_route(MediaControl *mc)
{
    if (mc == NULL)
        return;

    audio_route_restore_previous(mc->audio_route);
}

static GPtrArray *dup_pause_targets(GList *paused_players,
                                    GHashTable *pending_pauses)
{
    GPtrArray *targets = g_ptr_array_new_with_free_func(g_free);
    GHashTable *seen = g_hash_table_new(g_str_hash, g_str_equal);

    for (GList *l = paused_players; l != NULL; l = l->next) {
        if (g_hash_table_add(seen, l->data))
            g_ptr_array_add(targets, g_strdup(l->data));
    }

    GHashTableIter iter;
    gpointer key = NULL;
    g_hash_table_iter_init(&iter, pending_pauses);
    while (g_hash_table_iter_next(&iter, &key, NULL)) {
        if (g_hash_table_add(seen, key))
            g_ptr_array_add(targets, g_strdup(key));
    }

    g_hash_table_unref(seen);
    return targets;
}

static bool pause_reason_contains(GList *paused_players,
                                  GHashTable *pending_pauses,
                                  const gchar *player_name)
{
    return player_list_contains(paused_players, player_name) ||
           g_hash_table_contains(pending_pauses, player_name);
}

void media_control_resume(MediaControl *mc)
{
    if (mc == NULL || mc->connection == NULL) {
        return;
    }

    /* Include pending Pause calls. D-Bus preserves call order on this
     * connection, so a quick remove -> wear queues Play behind Pause and
     * cannot leave the player stopped due to a late Pause reply. */
    GPtrArray *targets = dup_pause_targets(mc->ear_paused_players,
                                           mc->ear_pending_pauses);
    for (guint i = 0; i < targets->len; i++) {
        const gchar *player_name = g_ptr_array_index(targets, i);
        if (!pause_reason_contains(mc->handoff_paused_players,
                                   mc->handoff_pending_pauses,
                                   player_name)) {
            player_play_async(mc, player_name);
        }
    }
    g_ptr_array_unref(targets);

    g_list_free_full(mc->ear_paused_players, g_free);
    mc->ear_paused_players = NULL;
    g_hash_table_remove_all(mc->ear_pending_pauses);
}
