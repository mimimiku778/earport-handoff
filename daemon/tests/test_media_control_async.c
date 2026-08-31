/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 EarPort Contributors
 */

#include <gio/gio.h>
#include <glib.h>

#include "media_control.h"

#define TEST_PLAYER_NAME "org.mpris.MediaPlayer2.EarPortAsyncTest"
#define TEST_PLAYER_PATH "/org/mpris/MediaPlayer2"

typedef struct {
    guint pause_count;
    guint play_count;
    GDBusMethodInvocation *pending_pause;
    GString *call_order;
} FakePlayer;

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
    gpointer user_data G_GNUC_UNUSED)
{
    if (g_strcmp0(property_name, "PlaybackStatus") == 0)
        return g_variant_new_string("Playing");
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

static void test_quick_rewear_orders_play_after_pending_pause(void)
{
    GTestDBus *test_bus = g_test_dbus_new(G_TEST_DBUS_NONE);
    g_test_dbus_up(test_bus);

    MediaControl *mc = media_control_new();
    g_assert_nonnull(mc);
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
    g_test_add_func("/media-control/quick-rewear-async-order",
                    test_quick_rewear_orders_play_after_pending_pause);
    return g_test_run();
}
