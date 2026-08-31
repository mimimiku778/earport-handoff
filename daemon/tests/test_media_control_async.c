/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 EarPort Contributors
 */

#include <gio/gio.h>
#include <glib.h>

#include "media_control.h"

#define TEST_PLAYER_NAME "org.mpris.MediaPlayer2.EarPortAsyncTest"
#define TEST_PLAYER_ONE_NAME "org.mpris.MediaPlayer2.EarPortAsyncOne"
#define TEST_PLAYER_TWO_NAME "org.mpris.MediaPlayer2.EarPortAsyncTwo"
#define TEST_PLAYER_PATH "/org/mpris/MediaPlayer2"

typedef struct {
    guint pause_count;
    guint play_count;
    GDBusMethodInvocation *pending_pause;
    GString *call_order;
    const gchar *playback_status;
    GDBusConnection *connection;
    guint get_count;
} FakePlayer;

typedef struct {
    FakePlayer player;
    GDBusConnection *connection;
    GDBusNodeInfo *node_info;
    guint registration_id;
} FakePlayerService;

static const gchar introspection_xml[] =
    "<node>"
    " <interface name='org.mpris.MediaPlayer2.Player'>"
    "  <method name='Pause'/>"
    "  <method name='Play'/>"
    "  <property name='PlaybackStatus' type='s' access='read'/>"
    " </interface>"
    "</node>";

static void on_fake_method_call(
    GDBusConnection *connection G_GNUC_UNUSED,
    const gchar *sender G_GNUC_UNUSED,
    const gchar *object_path G_GNUC_UNUSED,
    const gchar *interface_name G_GNUC_UNUSED,
    const gchar *method_name,
    GVariant *parameters G_GNUC_UNUSED,
    GDBusMethodInvocation *invocation,
    gpointer user_data)
{
    FakePlayer *player = user_data;

    if (g_strcmp0(method_name, "Pause") == 0) {
        g_assert_null(player->pending_pause);
        player->pause_count++;
        g_string_append(player->call_order, "Pause,");
        /* Deliberately hold the reply so wear can race the Pause ACK. */
        player->pending_pause = g_object_ref(invocation);
    } else if (g_strcmp0(method_name, "Play") == 0) {
        player->play_count++;
        g_string_append(player->call_order, "Play,");
        g_dbus_method_invocation_return_value(invocation, NULL);
    } else {
        g_dbus_method_invocation_return_dbus_error(
            invocation,
            "org.freedesktop.DBus.Error.UnknownMethod",
            "Unknown test method");
    }
}

static GVariant *on_fake_get_property(
    GDBusConnection *connection G_GNUC_UNUSED,
    const gchar *sender G_GNUC_UNUSED,
    const gchar *object_path G_GNUC_UNUSED,
    const gchar *interface_name G_GNUC_UNUSED,
    const gchar *property_name,
    GError **error G_GNUC_UNUSED,
    gpointer user_data)
{
    FakePlayer *player = user_data;
    if (g_strcmp0(property_name, "PlaybackStatus") == 0) {
        player->get_count++;
        return g_variant_new_string(player->playback_status);
    }
    return NULL;
}

static const GDBusInterfaceVTable fake_player_vtable = {
    .method_call = on_fake_method_call,
    .get_property = on_fake_get_property,
};

static gboolean spin_until(gboolean (*condition)(gpointer),
                           gpointer user_data,
                           guint timeout_msec)
{
    gint64 deadline = g_get_monotonic_time() +
                      (gint64)timeout_msec * G_TIME_SPAN_MILLISECOND;
    while (!condition(user_data) && g_get_monotonic_time() < deadline) {
        while (g_main_context_iteration(NULL, FALSE)) {
        }
        g_usleep(1000);
    }
    return condition(user_data);
}

static gboolean media_is_playing(gpointer user_data)
{
    return media_control_is_playing(user_data);
}

static gboolean media_is_not_playing(gpointer user_data)
{
    return !media_control_is_playing(user_data);
}

typedef struct {
    FakePlayer *player;
    guint expected;
} CountCondition;

static gboolean pause_count_reached(gpointer user_data)
{
    CountCondition *condition = user_data;
    return condition->player->pause_count >= condition->expected;
}

static gboolean play_count_reached(gpointer user_data)
{
    CountCondition *condition = user_data;
    return condition->player->play_count >= condition->expected;
}

static gboolean player_was_queried(gpointer user_data)
{
    FakePlayer *player = user_data;
    return player->get_count > 0;
}

static void reply_to_pending_pause(FakePlayer *player)
{
    g_assert_nonnull(player->pending_pause);
    g_dbus_method_invocation_return_value(player->pending_pause, NULL);
    g_clear_object(&player->pending_pause);
}

static void drain_main_context(void)
{
    gint64 deadline = g_get_monotonic_time() + 100 * G_TIME_SPAN_MILLISECOND;
    do {
        while (g_main_context_iteration(NULL, FALSE)) {
        }
        g_usleep(1000);
    } while (g_get_monotonic_time() < deadline);
}

static void fake_player_service_start(FakePlayerService *service,
                                      GTestDBus *test_bus,
                                      const gchar *bus_name,
                                      const gchar *initial_status)
{
    GError *error = NULL;
    service->connection = g_dbus_connection_new_for_address_sync(
        g_test_dbus_get_bus_address(test_bus),
        G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
            G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION,
        NULL,
        NULL,
        &error);
    g_assert_no_error(error);
    g_assert_nonnull(service->connection);

    service->node_info = g_dbus_node_info_new_for_xml(
        introspection_xml, &error);
    g_assert_no_error(error);
    g_assert_nonnull(service->node_info);

    service->player.call_order = g_string_new(NULL);
    service->player.playback_status = initial_status;
    service->player.connection = service->connection;
    service->registration_id = g_dbus_connection_register_object(
        service->connection,
        TEST_PLAYER_PATH,
        service->node_info->interfaces[0],
        &fake_player_vtable,
        &service->player,
        NULL,
        &error);
    g_assert_no_error(error);
    g_assert_cmpuint(service->registration_id, >, 0);

    GVariant *request_result = g_dbus_connection_call_sync(
        service->connection,
        "org.freedesktop.DBus",
        "/org/freedesktop/DBus",
        "org.freedesktop.DBus",
        "RequestName",
        g_variant_new("(su)", bus_name, 0u),
        G_VARIANT_TYPE("(u)"),
        G_DBUS_CALL_FLAGS_NONE,
        2000,
        NULL,
        &error);
    g_assert_no_error(error);
    g_assert_nonnull(request_result);
    g_variant_unref(request_result);
}

static void fake_player_service_stop(FakePlayerService *service)
{
    g_assert_null(service->player.pending_pause);
    g_assert_true(g_dbus_connection_unregister_object(
        service->connection, service->registration_id));
    g_string_free(service->player.call_order, TRUE);
    g_dbus_node_info_unref(service->node_info);
    g_dbus_connection_close_sync(service->connection, NULL, NULL);
    g_object_unref(service->connection);
}

static void fake_player_emit_status(FakePlayer *player, const gchar *status)
{
    player->playback_status = status;

    GVariantBuilder changed;
    GVariantBuilder invalidated;
    g_variant_builder_init(&changed, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&changed,
                          "{sv}",
                          "PlaybackStatus",
                          g_variant_new_string(status));
    g_variant_builder_init(&invalidated, G_VARIANT_TYPE("as"));

    GError *error = NULL;
    g_assert_true(g_dbus_connection_emit_signal(
        player->connection,
        NULL,
        TEST_PLAYER_PATH,
        "org.freedesktop.DBus.Properties",
        "PropertiesChanged",
        g_variant_new("(s@a{sv}@as)",
                      "org.mpris.MediaPlayer2.Player",
                      g_variant_builder_end(&changed),
                      g_variant_builder_end(&invalidated)),
        &error));
    g_assert_no_error(error);
}

typedef struct {
    FakePlayer *first;
    FakePlayer *second;
} PlayerPair;

static gboolean both_players_were_queried(gpointer user_data)
{
    PlayerPair *pair = user_data;
    return pair->first->get_count > 0 && pair->second->get_count > 0;
}

static gboolean both_players_were_paused(gpointer user_data)
{
    PlayerPair *pair = user_data;
    return pair->first->pause_count > 0 && pair->second->pause_count > 0;
}

static void count_playback_started(void *user_data)
{
    guint *count = user_data;
    (*count)++;
}

static void test_claim_requires_fresh_allowed_wear_state(void)
{
    GTestDBus *test_bus = g_test_dbus_new(G_TEST_DBUS_NONE);
    g_test_dbus_up(test_bus);

    MediaControl *mc = media_control_new();
    g_assert_nonnull(mc);

    /* Speaker pausing fails open, but ownership/rerouting fails closed. */
    g_assert_false(media_control_wear_state_blocks_playback(mc));
    g_assert_false(media_control_can_claim_or_route_audio(mc));

    media_control_set_airpods_link_active(mc, true);
    g_assert_false(media_control_wear_state_blocks_playback(mc));
    g_assert_false(media_control_can_claim_or_route_audio(mc));

    media_control_on_ear_detection_changed(mc, false, false);
    g_assert_true(media_control_wear_state_blocks_playback(mc));
    g_assert_false(media_control_can_claim_or_route_audio(mc));

    /* Routing follows physical wear, not the optional auto-pause mode. */
    media_control_set_ear_pause_mode(mc, EAR_PAUSE_DISABLED);
    g_assert_false(media_control_can_claim_or_route_audio(mc));

    media_control_set_airpods_link_active(mc, false);
    media_control_set_airpods_link_active(mc, true);
    media_control_on_ear_detection_changed(mc, true, false);
    g_assert_true(media_control_can_claim_or_route_audio(mc));

    media_control_set_airpods_link_active(mc, false);
    g_assert_false(media_control_wear_state_blocks_playback(mc));
    g_assert_false(media_control_can_claim_or_route_audio(mc));

    media_control_set_airpods_link_active(mc, true);
    g_assert_false(media_control_can_claim_or_route_audio(mc));
    media_control_on_ear_detection_changed(mc, true, true);
    g_assert_true(media_control_can_claim_or_route_audio(mc));

    media_control_free(mc);
    drain_main_context();
    g_test_dbus_down(test_bus);
    g_object_unref(test_bus);
}

static void test_explicit_start_is_not_repaused_off_head(void)
{
    GTestDBus *test_bus = g_test_dbus_new(G_TEST_DBUS_NONE);
    g_test_dbus_up(test_bus);

    MediaControl *mc = media_control_new();
    g_assert_nonnull(mc);
    media_control_set_airpods_link_active(mc, true);
    media_control_set_airpods_audio_active(mc, true);
    media_control_on_ear_detection_changed(mc, true, true);

    FakePlayerService service = {0};
    fake_player_service_start(&service,
                              test_bus,
                              TEST_PLAYER_ONE_NAME,
                              "Playing");
    g_assert_true(spin_until(player_was_queried,
                             &service.player,
                             2000));
    g_assert_true(spin_until(media_is_playing, mc, 2000));

    media_control_on_ear_detection_changed(mc, false, false);
    CountCondition pause = {&service.player, 1};
    g_assert_true(spin_until(pause_count_reached, &pause, 2000));

    /* A noisy duplicate Playing property is not an explicit restart and
     * must not cancel or compensate the still-pending Pause. */
    fake_player_emit_status(&service.player, "Playing");
    drain_main_context();
    g_assert_cmpuint(service.player.play_count, ==, 0);
    g_assert_nonnull(service.player.pending_pause);

    fake_player_emit_status(&service.player, "Paused");
    g_assert_true(spin_until(media_is_not_playing, mc, 2000));
    reply_to_pending_pause(&service.player);
    drain_main_context();

    guint playback_started_count = 0;
    media_control_set_playback_started_callback(mc,
                                                count_playback_started,
                                                &playback_started_count);
    fake_player_emit_status(&service.player, "Playing");
    g_assert_true(spin_until(media_is_playing, mc, 2000));
    drain_main_context();

    /* Removal paused once. An explicit later start remains on speakers even
     * though the connected AirPods still report off-head. */
    g_assert_cmpuint(playback_started_count, ==, 1);
    g_assert_cmpuint(service.player.pause_count, ==, 1);
    g_assert_cmpuint(service.player.play_count, ==, 0);

    media_control_free(mc);
    drain_main_context();
    fake_player_service_stop(&service);
    g_test_dbus_down(test_bus);
    g_object_unref(test_bus);
}

static void test_unconfirmed_source_initial_out_keeps_speakers_playing(void)
{
    GTestDBus *test_bus = g_test_dbus_new(G_TEST_DBUS_NONE);
    g_test_dbus_up(test_bus);

    MediaControl *mc = media_control_new();
    g_assert_nonnull(mc);
    media_control_set_airpods_link_active(mc, true);

    FakePlayerService service = {0};
    fake_player_service_start(&service,
                              test_bus,
                              TEST_PLAYER_ONE_NAME,
                              "Playing");
    g_assert_true(spin_until(player_was_queried,
                             &service.player,
                             2000));
    g_assert_true(spin_until(media_is_playing, mc, 2000));

    /* A live AAP control link and an initial off-head report do not prove
     * that Linux is sending audio to AirPods. Speaker playback is untouched. */
    media_control_on_ear_detection_changed(mc, false, false);
    drain_main_context();
    g_assert_cmpuint(service.player.pause_count, ==, 0);
    g_assert_null(service.player.pending_pause);
    g_assert_true(media_control_is_playing(mc));

    media_control_free(mc);
    drain_main_context();
    fake_player_service_stop(&service);
    g_test_dbus_down(test_bus);
    g_object_unref(test_bus);
}

static void test_second_player_start_is_forwarded_while_first_is_playing(void)
{
    GTestDBus *test_bus = g_test_dbus_new(G_TEST_DBUS_NONE);
    g_test_dbus_up(test_bus);

    MediaControl *mc = media_control_new();
    g_assert_nonnull(mc);

    FakePlayerService first = {0};
    FakePlayerService second = {0};
    fake_player_service_start(&first,
                              test_bus,
                              TEST_PLAYER_ONE_NAME,
                              "Playing");
    fake_player_service_start(&second,
                              test_bus,
                              TEST_PLAYER_TWO_NAME,
                              "Paused");
    PlayerPair pair = {&first.player, &second.player};
    g_assert_true(spin_until(both_players_were_queried, &pair, 2000));
    g_assert_true(spin_until(media_is_playing, mc, 2000));

    guint playback_started_count = 0;
    media_control_set_playback_started_callback(mc,
                                                count_playback_started,
                                                &playback_started_count);
    fake_player_emit_status(&second.player, "Playing");
    drain_main_context();

    /* The aggregate remains Playing, but this explicit edge must still reach
     * handoff policy so Linux can reclaim AirPods from another host. */
    g_assert_cmpuint(playback_started_count, ==, 1);

    media_control_free(mc);
    drain_main_context();
    fake_player_service_stop(&first);
    fake_player_service_stop(&second);
    g_test_dbus_down(test_bus);
    g_object_unref(test_bus);
}

static void test_disconnected_restart_reconciles_only_that_player(void)
{
    GTestDBus *test_bus = g_test_dbus_new(G_TEST_DBUS_NONE);
    g_test_dbus_up(test_bus);

    MediaControl *mc = media_control_new();
    g_assert_nonnull(mc);
    media_control_set_airpods_link_active(mc, true);
    media_control_set_airpods_audio_active(mc, true);
    media_control_on_ear_detection_changed(mc, true, true);

    FakePlayerService first = {0};
    FakePlayerService second = {0};
    fake_player_service_start(&first,
                              test_bus,
                              TEST_PLAYER_ONE_NAME,
                              "Playing");
    fake_player_service_start(&second,
                              test_bus,
                              TEST_PLAYER_TWO_NAME,
                              "Playing");
    PlayerPair pair = {&first.player, &second.player};
    g_assert_true(spin_until(both_players_were_queried, &pair, 2000));
    drain_main_context();

    media_control_on_ear_detection_changed(mc, false, false);
    g_assert_true(spin_until(both_players_were_paused, &pair, 2000));

    /* Both Pause calls are deliberately still awaiting ACK when the link
     * drops and only the first player is manually restarted on speakers. */
    fake_player_emit_status(&first.player, "Paused");
    fake_player_emit_status(&second.player, "Paused");
    g_assert_true(spin_until(media_is_not_playing, mc, 2000));
    media_control_set_airpods_link_active(mc, false);

    guint playback_started_count = 0;
    media_control_set_playback_started_callback(mc,
                                                count_playback_started,
                                                &playback_started_count);
    fake_player_emit_status(&first.player, "Playing");
    CountCondition first_play = {&first.player, 1};
    g_assert_true(spin_until(play_count_reached, &first_play, 2000));

    g_assert_cmpuint(playback_started_count, ==, 1);
    g_assert_cmpuint(first.player.pause_count, ==, 1);
    g_assert_cmpuint(second.player.pause_count, ==, 1);
    g_assert_cmpuint(second.player.play_count, ==, 0);

    reply_to_pending_pause(&first.player);
    reply_to_pending_pause(&second.player);
    drain_main_context();
    g_assert_cmpuint(first.player.pause_count, ==, 1);
    g_assert_cmpuint(second.player.pause_count, ==, 1);
    g_assert_cmpuint(second.player.play_count, ==, 0);

    media_control_free(mc);
    drain_main_context();
    fake_player_service_stop(&first);
    fake_player_service_stop(&second);
    g_test_dbus_down(test_bus);
    g_object_unref(test_bus);
}

static void test_pending_handoff_restart_reconciles_only_that_player(void)
{
    GTestDBus *test_bus = g_test_dbus_new(G_TEST_DBUS_NONE);
    g_test_dbus_up(test_bus);

    MediaControl *mc = media_control_new();
    g_assert_nonnull(mc);
    media_control_set_airpods_link_active(mc, true);
    media_control_set_airpods_audio_active(mc, true);
    media_control_on_ear_detection_changed(mc, true, true);

    FakePlayerService first = {0};
    FakePlayerService second = {0};
    fake_player_service_start(&first,
                              test_bus,
                              TEST_PLAYER_ONE_NAME,
                              "Playing");
    fake_player_service_start(&second,
                              test_bus,
                              TEST_PLAYER_TWO_NAME,
                              "Playing");
    PlayerPair pair = {&first.player, &second.player};
    g_assert_true(spin_until(both_players_were_queried, &pair, 2000));
    drain_main_context();

    media_control_pause_all_for_handoff(mc);
    g_assert_true(spin_until(both_players_were_paused, &pair, 2000));
    fake_player_emit_status(&first.player, "Paused");
    fake_player_emit_status(&second.player, "Paused");
    g_assert_true(spin_until(media_is_not_playing, mc, 2000));

    /* An explicit speaker start must compensate only this player's in-flight
     * handoff Pause. The other paused application remains untouched. */
    fake_player_emit_status(&first.player, "Playing");
    CountCondition first_play = {&first.player, 1};
    g_assert_true(spin_until(play_count_reached, &first_play, 2000));
    g_assert_cmpuint(second.player.play_count, ==, 0);

    reply_to_pending_pause(&first.player);
    reply_to_pending_pause(&second.player);
    drain_main_context();
    g_assert_cmpuint(first.player.play_count, ==, 1);
    g_assert_cmpuint(second.player.play_count, ==, 0);

    media_control_free(mc);
    drain_main_context();
    fake_player_service_stop(&first);
    fake_player_service_stop(&second);
    g_test_dbus_down(test_bus);
    g_object_unref(test_bus);
}

static void test_disconnected_link_never_pauses_speaker_playback(void)
{
    GTestDBus *test_bus = g_test_dbus_new(G_TEST_DBUS_NONE);
    g_test_dbus_up(test_bus);

    MediaControl *mc = media_control_new();
    g_assert_nonnull(mc);

    /* Reproduce the regression: the last live AirPods session said "out",
     * then its control link disappeared before a local-speaker player began. */
    media_control_set_airpods_link_active(mc, true);
    media_control_on_ear_detection_changed(mc, false, false);
    media_control_set_airpods_link_active(mc, false);

    guint playback_started_count = 0;
    media_control_set_playback_started_callback(mc,
                                                count_playback_started,
                                                &playback_started_count);

    GError *error = NULL;
    GDBusConnection *service_connection =
        g_dbus_connection_new_for_address_sync(
            g_test_dbus_get_bus_address(test_bus),
            G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
                G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION,
            NULL,
            NULL,
            &error);
    g_assert_no_error(error);
    g_assert_nonnull(service_connection);

    GDBusNodeInfo *node_info = g_dbus_node_info_new_for_xml(
        introspection_xml, &error);
    g_assert_no_error(error);
    g_assert_nonnull(node_info);

    FakePlayer player = {
        .call_order = g_string_new(NULL),
        .playback_status = "Playing",
    };
    guint registration_id = g_dbus_connection_register_object(
        service_connection,
        TEST_PLAYER_PATH,
        node_info->interfaces[0],
        &fake_player_vtable,
        &player,
        NULL,
        &error);
    g_assert_no_error(error);
    g_assert_cmpuint(registration_id, >, 0);

    GVariant *request_result = g_dbus_connection_call_sync(
        service_connection,
        "org.freedesktop.DBus",
        "/org/freedesktop/DBus",
        "org.freedesktop.DBus",
        "RequestName",
        g_variant_new("(su)", TEST_PLAYER_NAME, 0u),
        G_VARIANT_TYPE("(u)"),
        G_DBUS_CALL_FLAGS_NONE,
        2000,
        NULL,
        &error);
    g_assert_no_error(error);
    g_assert_nonnull(request_result);
    g_variant_unref(request_result);

    g_assert_true(spin_until(media_is_playing, mc, 2000));
    drain_main_context();
    g_assert_cmpuint(playback_started_count, ==, 1);
    g_assert_cmpuint(player.pause_count, ==, 0);
    g_assert_null(player.pending_pause);

    media_control_free(mc);
    drain_main_context();
    g_assert_true(g_dbus_connection_unregister_object(service_connection,
                                                        registration_id));
    g_string_free(player.call_order, TRUE);
    g_dbus_node_info_unref(node_info);
    g_dbus_connection_close_sync(service_connection, NULL, NULL);
    g_object_unref(service_connection);
    g_test_dbus_down(test_bus);
    g_object_unref(test_bus);
}

static void test_quick_rewear_orders_play_after_pending_pause(void)
{
    GTestDBus *test_bus = g_test_dbus_new(G_TEST_DBUS_NONE);
    g_test_dbus_up(test_bus);

    MediaControl *mc = media_control_new();
    g_assert_nonnull(mc);
    media_control_set_airpods_link_active(mc, true);
    media_control_set_airpods_audio_active(mc, true);
    media_control_on_ear_detection_changed(mc, true, true);

    GError *error = NULL;
    GDBusConnection *service_connection =
        g_dbus_connection_new_for_address_sync(
            g_test_dbus_get_bus_address(test_bus),
            G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
                G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION,
            NULL,
            NULL,
            &error);
    g_assert_no_error(error);
    g_assert_nonnull(service_connection);

    GDBusNodeInfo *node_info = g_dbus_node_info_new_for_xml(
        introspection_xml, &error);
    g_assert_no_error(error);
    g_assert_nonnull(node_info);

    FakePlayer player = {
        .call_order = g_string_new(NULL),
        .playback_status = "Playing",
    };
    guint registration_id = g_dbus_connection_register_object(
        service_connection,
        TEST_PLAYER_PATH,
        node_info->interfaces[0],
        &fake_player_vtable,
        &player,
        NULL,
        &error);
    g_assert_no_error(error);
    g_assert_cmpuint(registration_id, >, 0);

    GVariant *request_result = g_dbus_connection_call_sync(
        service_connection,
        "org.freedesktop.DBus",
        "/org/freedesktop/DBus",
        "org.freedesktop.DBus",
        "RequestName",
        g_variant_new("(su)", TEST_PLAYER_NAME, 0u),
        G_VARIANT_TYPE("(u)"),
        G_DBUS_CALL_FLAGS_NONE,
        2000,
        NULL,
        &error);
    g_assert_no_error(error);
    g_assert_nonnull(request_result);
    g_variant_unref(request_result);

    g_assert_true(spin_until(media_is_playing, mc, 2000));

    media_control_on_ear_detection_changed(mc, false, false);
    CountCondition first_pause = {&player, 1};
    g_assert_true(spin_until(pause_count_reached, &first_pause, 2000));

    /* Rewear before the fake player acknowledges Pause. */
    media_control_on_ear_detection_changed(mc, true, true);
    CountCondition first_play = {&player, 1};
    g_assert_true(spin_until(play_count_reached, &first_play, 2000));
    g_assert_cmpstr(player.call_order->str, ==, "Pause,Play,");
    reply_to_pending_pause(&player);
    drain_main_context();

    /* A pending MPRIS call must also be safe when the owner is destroyed. */
    media_control_on_ear_detection_changed(mc, false, false);
    CountCondition second_pause = {&player, 2};
    g_assert_true(spin_until(pause_count_reached, &second_pause, 2000));
    media_control_free(mc);
    reply_to_pending_pause(&player);
    drain_main_context();

    g_assert_true(g_dbus_connection_unregister_object(service_connection,
                                                        registration_id));
    g_string_free(player.call_order, TRUE);
    g_dbus_node_info_unref(node_info);
    g_dbus_connection_close_sync(service_connection, NULL, NULL);
    g_object_unref(service_connection);
    g_test_dbus_down(test_bus);
    g_object_unref(test_bus);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/media-control/claim-requires-fresh-wear",
                    test_claim_requires_fresh_allowed_wear_state);
    g_test_add_func("/media-control/explicit-start-not-repaused-off-head",
                    test_explicit_start_is_not_repaused_off_head);
    g_test_add_func("/media-control/unconfirmed-source-keeps-speakers",
                    test_unconfirmed_source_initial_out_keeps_speakers_playing);
    g_test_add_func("/media-control/second-player-start-forwarded",
                    test_second_player_start_is_forwarded_while_first_is_playing);
    g_test_add_func("/media-control/disconnected-restart-is-player-local",
                    test_disconnected_restart_reconciles_only_that_player);
    g_test_add_func("/media-control/handoff-restart-is-player-local",
                    test_pending_handoff_restart_reconciles_only_that_player);
    g_test_add_func("/media-control/quick-rewear-async-order",
                    test_quick_rewear_orders_play_after_pending_pause);
    g_test_add_func("/media-control/disconnected-link-does-not-pause",
                    test_disconnected_link_never_pauses_speaker_playback);
    return g_test_run();
}
