/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2024 EarPort Contributors
 *
 * EarPort Daemon - AirPods integration for Linux
 */

#include <glib.h>
#include <glib-unix.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>

#include "airpods_state.h"
#include "aap_protocol.h"
#include "bluetooth.h"
#include "bluez_monitor.h"
#include "ble_autoconnect.h"
#include "config.h"
#include "connection_policy.h"
#include "handoff_policy.h"
#include "dbus_service.h"
#include "media_control.h"

/* Global application state */
typedef struct {
    GMainLoop *main_loop;
    AirPodsState state;
    BluetoothConnection *bt_conn;
    BluezMonitor *bluez_monitor;
    DbusService *dbus_service;
    MediaControl *media_control;
    EarPortConfig config;

    /* Pending connect info */
    char *pending_address;
    char *pending_name;

    /* Reconnection */
    guint reconnect_timeout_id;
    int reconnect_attempts;
    bool bluez_connected;   /* Device still connected at BlueZ level */

    /* AAP initialization is ACK-driven. Newer AirPods can take about a
     * second to finish feature negotiation, so an eager notification request
     * is silently ignored. */
    AapInitState aap_init;
    guint handshake_timeout_id;
    guint notification_retry_timeout_id;
    guint saved_settings_timeout_id;
    guint connection_generation;
    guint ear_detection_poll_timeout_id;
    guint removal_disconnect_timeout_id;
    bool notifications_healthy;
    bool ear_state_valid;

    /* Keep the last valid AAP wear observation across an L2CAP reset. BlueZ
     * may report the link loss after the AAP socket has already cleared the
     * public device state. */
    bool last_wear_state_valid;
    bool last_left_in_ear;
    bool last_right_in_ear;
    RemovalLifecycle removal_lifecycle;
    bool reclaim_after_removal_reconnect;

    /* Seamless audio-source handoff state. AudioSource addresses use the
     * same byte order as bdaddr_t, not the printable Bluetooth address. */
    uint8_t local_audio_source_address[6];
    bool local_audio_source_address_valid;
    AapAudioSourceData current_audio_source;
    bool current_audio_source_valid;
    /* A remote Apple host explicitly owns, or has requested, the AirPods.
     * Transient NONE notifications must never clear this: only a fresh Linux
     * MPRIS Playing edge may claim the headphones again. */
    HandoffPolicy handoff_policy;
    gint64 last_audio_claim_time;
} AppContext;

/* L2CAP reconnection: AirPods frequently refuse the first L2CAP connect
 * right after the BlueZ link comes up, so retry with exponential backoff. */
#define RECONNECT_MAX_ATTEMPTS 5
#define RECONNECT_BASE_DELAY_SEC 2
#define AUDIO_CLAIM_DEBOUNCE_USEC (500 * 1000)
#define NOTIFICATION_RETRY_DELAY_MSEC 2000
#define EAR_DETECTION_POLL_INTERVAL_SEC 1
#define REMOVAL_DISCONNECT_DELAY_MSEC 1000

static AppContext app = {0};

/* Forward declarations */
static void connect_to_airpods(const char *address, const char *name);
static void disconnect_from_airpods(void);
static void apply_device_profile(const char *address);
static void schedule_saved_settings(void);
static void schedule_reconnect(void);
static void cancel_reconnect(void);
static void on_media_playback_started(void *user_data);
static void on_media_playback_stopped(void *user_data);
static void maybe_reclaim_after_removal_reconnect(void);

static bool removal_disconnect_still_required(const char *address,
                                              void *user_data)
{
    (void)user_data;
    if (!app.config.disconnect_on_removal || app.bluez_monitor == NULL ||
        !app.bluez_connected ||
        !airpods_address_equal(app.pending_address, address)) {
        return false;
    }

    g_mutex_lock(&app.state.lock);
    bool required = app.state.connected &&
        airpods_address_equal(app.state.device_address, address) &&
        ble_airpods_model_supports_wear_autoconnect(
            (uint16_t)app.state.model) &&
        wear_policy_fully_removed(app.state.ear_detection.left_in_ear,
                                  app.state.ear_detection.right_in_ear);
    g_mutex_unlock(&app.state.lock);
    return required;
}

static void on_removal_disconnect_finished(const char *address,
                                           bool completed,
                                           void *user_data)
{
    (void)user_data;
    if (completed || !removal_lifecycle_matches(&app.removal_lifecycle,
                                                address)) {
        return;
    }

    /* Every queued Disconnect attempt failed or rewear cancelled its delayed
     * retry. No BlueZ Connected=false event will finish this lifecycle. */
    removal_lifecycle_clear(&app.removal_lifecycle);
}

static void cancel_removal_disconnect(void)
{
    if (app.removal_disconnect_timeout_id > 0) {
        g_source_remove(app.removal_disconnect_timeout_id);
        app.removal_disconnect_timeout_id = 0;
    }
}

static gboolean removal_disconnect_timeout_cb(gpointer user_data)
{
    (void)user_data;
    app.removal_disconnect_timeout_id = 0;

    if (!app.config.disconnect_on_removal || app.bluez_monitor == NULL ||
        !app.bluez_connected) {
        return G_SOURCE_REMOVE;
    }

    g_mutex_lock(&app.state.lock);
    bool still_connected = app.state.connected;
    bool fully_removed = wear_policy_fully_removed(
        app.state.ear_detection.left_in_ear,
        app.state.ear_detection.right_in_ear);
    char *address = g_strdup(app.state.device_address);
    g_mutex_unlock(&app.state.lock);

    if (still_connected && fully_removed && address != NULL) {
        if (bluez_monitor_disconnect_device(
                app.bluez_monitor,
                address,
                removal_disconnect_still_required,
                on_removal_disconnect_finished,
                NULL)) {
            removal_lifecycle_mark(&app.removal_lifecycle, address);
        } else {
            g_warning("Could not queue BlueZ disconnect for removed AirPods");
        }
    }

    g_free(address);
    return G_SOURCE_REMOVE;
}

static void update_removal_disconnect(bool left_in_ear, bool right_in_ear)
{
    g_mutex_lock(&app.state.lock);
    AirPodsModel model = app.state.model;
    g_mutex_unlock(&app.state.lock);

    if (!app.config.disconnect_on_removal || !app.bluez_connected ||
        !ble_airpods_model_supports_wear_autoconnect((uint16_t)model) ||
        !wear_policy_fully_removed(left_in_ear, right_in_ear)) {
        cancel_removal_disconnect();
        return;
    }

    if (app.removal_disconnect_timeout_id == 0) {
        g_message("Both AirPods wear slots are out; disconnecting in one second unless reworn");
        app.removal_disconnect_timeout_id = g_timeout_add(
            REMOVAL_DISCONNECT_DELAY_MSEC,
            removal_disconnect_timeout_cb,
            NULL);
    }
}

static void cancel_ear_detection_poll(void)
{
    if (app.ear_detection_poll_timeout_id > 0) {
        g_source_remove(app.ear_detection_poll_timeout_id);
        app.ear_detection_poll_timeout_id = 0;
    }
}

static gboolean send_handshake_timeout_cb(gpointer user_data)
{
    (void)user_data;
    app.handshake_timeout_id = 0;

    if (app.bt_conn != NULL && bt_connection_is_connected(app.bt_conn) &&
        !bt_connection_send_handshake(app.bt_conn)) {
        g_warning("Failed to send AirPods handshake");
    }
    return G_SOURCE_REMOVE;
}

static void cancel_handshake_timeout(void)
{
    if (app.handshake_timeout_id > 0) {
        g_source_remove(app.handshake_timeout_id);
        app.handshake_timeout_id = 0;
    }
}

static gboolean ear_detection_poll_timeout_cb(gpointer user_data)
{
    (void)user_data;

    if (app.bt_conn == NULL || !bt_connection_is_connected(app.bt_conn)) {
        app.ear_detection_poll_timeout_id = 0;
        return G_SOURCE_REMOVE;
    }

    /* Do not send application-level requests until feature negotiation and
     * notification registration have completed for this AAP session. */
    if (!app.aap_init.notifications_requested) {
        return G_SOURCE_CONTINUE;
    }

    /* This six-byte AAP query is deliberately unconditional once per second.
     * Synchronously enumerating MPRIS players before every query could block
     * the main loop long enough to delay removal-disconnect. Unchanged poll
     * replies are deduplicated before logging or D-Bus publication. */

    if (bt_connection_send(app.bt_conn,
                           AAP_PKT_REQUEST_EAR_DETECTION,
                           AAP_EAR_DETECTION_REQUEST_SIZE) !=
        (ssize_t)AAP_EAR_DETECTION_REQUEST_SIZE) {
        g_warning("Failed to request current AirPods wear state");
    }

    return G_SOURCE_CONTINUE;
}

static void start_ear_detection_poll(void)
{
    if (app.ear_detection_poll_timeout_id > 0)
        return;

    app.ear_detection_poll_timeout_id = g_timeout_add_seconds(
        EAR_DETECTION_POLL_INTERVAL_SEC,
        ear_detection_poll_timeout_cb,
        NULL);
}

static void cancel_notification_retry(void)
{
    if (app.notification_retry_timeout_id > 0) {
        g_source_remove(app.notification_retry_timeout_id);
        app.notification_retry_timeout_id = 0;
    }
}

static void cancel_saved_settings(void)
{
    if (app.saved_settings_timeout_id > 0) {
        g_source_remove(app.saved_settings_timeout_id);
        app.saved_settings_timeout_id = 0;
    }
}

static void reset_aap_initialization(void)
{
    cancel_removal_disconnect();
    cancel_handshake_timeout();
    cancel_ear_detection_poll();
    cancel_notification_retry();
    cancel_saved_settings();
    aap_init_state_reset(&app.aap_init);
    app.notifications_healthy = false;
    app.ear_state_valid = false;
}

static gboolean retry_notifications_timeout_cb(gpointer user_data)
{
    (void)user_data;
    app.notification_retry_timeout_id = 0;

    if (app.notifications_healthy || app.bt_conn == NULL ||
        !bt_connection_is_connected(app.bt_conn)) {
        return G_SOURCE_REMOVE;
    }

    g_message("No battery or wear notification after AAP setup; retrying notification request once");
    if (!bt_connection_send_request_notifications(app.bt_conn))
        g_warning("Failed to retry AirPods notification request");

    return G_SOURCE_REMOVE;
}

static void mark_notifications_healthy(void)
{
    if (app.notifications_healthy)
        return;

    app.notifications_healthy = true;
    cancel_notification_retry();
    g_message("AirPods battery/wear notification stream is active");
}

static bool handle_aap_initialization_packet(const uint8_t *data, size_t len)
{
    AapInitAction action = aap_init_next_action(&app.aap_init, data, len);

    switch (action) {
    case AAP_INIT_ACTION_SEND_FEATURES:
        g_message("AirPods handshake acknowledged; sending feature configuration");
        if (bt_connection_send_set_features(app.bt_conn)) {
            aap_init_mark_action_sent(&app.aap_init, action);
        } else {
            g_warning("Failed to send AirPods feature configuration");
        }
        return true;

    case AAP_INIT_ACTION_REQUEST_NOTIFICATIONS:
        g_message("AirPods feature negotiation acknowledged; requesting notifications");
        if (bt_connection_send_request_notifications(app.bt_conn)) {
            aap_init_mark_action_sent(&app.aap_init, action);
            start_ear_detection_poll();
            schedule_saved_settings();
            maybe_reclaim_after_removal_reconnect();
            cancel_notification_retry();
            app.notification_retry_timeout_id =
                g_timeout_add(NOTIFICATION_RETRY_DELAY_MSEC,
                              retry_notifications_timeout_cb,
                              NULL);
        } else {
            g_warning("Failed to request AirPods notifications");
        }
        return true;

    case AAP_INIT_ACTION_NONE:
    default:
        return false;
    }
}

static void reset_audio_handoff_state(void)
{
    memset(&app.current_audio_source, 0, sizeof(app.current_audio_source));
    app.current_audio_source_valid = false;
    handoff_policy_reset(&app.handoff_policy);
    app.last_audio_claim_time = 0;
}

static bool audio_source_is_local(const AapAudioSourceData *source)
{
    return source != NULL && app.local_audio_source_address_valid &&
           memcmp(source->device_address,
                  app.local_audio_source_address,
                  sizeof(source->device_address)) == 0;
}

static bool audio_source_is_remote(const AapAudioSourceData *source)
{
    return source != NULL && app.local_audio_source_address_valid &&
           !audio_source_is_local(source);
}

static bool claim_audio_for_linux(bool restart_sink)
{
    if (!app.config.handoff_enabled)
        return false;

    if (app.media_control != NULL &&
        media_control_wear_state_blocks_playback(app.media_control)) {
        g_debug("Cannot claim AirPods audio ownership while they are not worn");
        return false;
    }

    if (app.bt_conn == NULL || !bt_connection_is_connected(app.bt_conn)) {
        g_debug("Cannot claim AirPods audio ownership: AAP is disconnected");
        return false;
    }

    uint8_t command[AAP_CONTROL_CMD_SIZE];
    aap_build_owns_connection_cmd(true, command);
    if (bt_connection_send(app.bt_conn, command, sizeof(command)) !=
        (ssize_t)sizeof(command)) {
        g_warning("Failed to send AirPods audio ownership claim");
        return false;
    }

    g_message("Claimed AirPods audio ownership for Linux");
    handoff_policy_note_linux_claim(&app.handoff_policy);
    app.last_audio_claim_time = g_get_monotonic_time();
    if (restart_sink && app.media_control != NULL) {
        const char *address = app.state.device_address != NULL
                                ? app.state.device_address
                                : app.pending_address;
        media_control_reclaim_audio(app.media_control, address);
    }
    return true;
}

static void maybe_reclaim_after_removal_reconnect(void)
{
    if (!app.reclaim_after_removal_reconnect ||
        !app.last_wear_state_valid ||
        (!app.last_left_in_ear && !app.last_right_in_ear) ||
        app.media_control == NULL ||
        !media_control_is_playing(app.media_control)) {
        return;
    }

    /* Rewear may resume MPRIS before the uncancellable BlueZ Disconnect has
     * completed. The player then remains continuously Playing across the new
     * AAP session, so no fresh MPRIS edge exists to reclaim AudioSource. */
    g_message("Reclaiming AirPods audio after rapid removal/reconnect");
    if (claim_audio_for_linux(true))
        app.reclaim_after_removal_reconnect = false;
}

static bool release_audio_from_linux(void)
{
    if (!app.config.handoff_enabled)
        return false;

    if (app.bt_conn == NULL || !bt_connection_is_connected(app.bt_conn)) {
        g_debug("Cannot release AirPods audio ownership: AAP is disconnected");
        return false;
    }

    uint8_t command[AAP_CONTROL_CMD_SIZE];
    aap_build_owns_connection_cmd(false, command);
    if (bt_connection_send(app.bt_conn, command, sizeof(command)) !=
        (ssize_t)sizeof(command)) {
        g_warning("Failed to release AirPods audio ownership");
        return false;
    }

    g_message("Released AirPods audio ownership from Linux");
    return true;
}

static void suppress_wear_reconnect_until_removal(void)
{
    /* An Apple-host handoff is not a wear-driven disconnect cycle. Never let
     * its pause ownership survive into a later Linux connection. */
    removal_lifecycle_clear(&app.removal_lifecycle);

    if (app.bluez_monitor == NULL)
        return;

    g_mutex_lock(&app.state.lock);
    char *address = g_strdup(app.state.device_address != NULL
                                 ? app.state.device_address
                                 : app.pending_address);
    g_mutex_unlock(&app.state.lock);

    if (address != NULL) {
        bluez_monitor_suppress_auto_connect_until_unworn(
            app.bluez_monitor, address);
    }
    g_free(address);
}

/* ============================================================================
 * Bluetooth data handling
 * ========================================================================== */

static void on_bt_data_received(const uint8_t *data, size_t len, void *user_data)
{
    (void)user_data;

    if (handle_aap_initialization_packet(data, len))
        return;

    AapParsedPacket packet;
    AapParseResult result = aap_parse_packet(data, len, &packet);

    if (result != AAP_PARSE_OK) {
        if (result != AAP_PARSE_UNKNOWN_OPCODE) {
            g_debug("Failed to parse packet: %d", result);
        }
        return;
    }

    switch (packet.type) {
    case AAP_PKT_TYPE_BATTERY:
        mark_notifications_healthy();
        g_message("Battery: L=%d%% (status=%d) R=%d%% (status=%d) Case=%d%% (status=%d)",
                  packet.data.battery.left_level,
                  packet.data.battery.left_status,
                  packet.data.battery.right_level,
                  packet.data.battery.right_status,
                  packet.data.battery.case_level,
                  packet.data.battery.case_status);

        airpods_state_set_battery(&app.state,
                                   packet.data.battery.left_level,
                                   packet.data.battery.left_status,
                                   packet.data.battery.right_level,
                                   packet.data.battery.right_status,
                                   packet.data.battery.case_level,
                                   packet.data.battery.case_status);

        dbus_service_emit_battery_changed(app.dbus_service,
                                           packet.data.battery.left_level,
                                           packet.data.battery.right_level,
                                           packet.data.battery.case_level);
        static const char *const battery_properties[] = {
            "BatteryLeft", "BatteryRight", "BatteryCase",
            "ChargingLeft", "ChargingRight", "ChargingCase",
        };
        dbus_service_emit_properties_changed_many(
            app.dbus_service, battery_properties,
            G_N_ELEMENTS(battery_properties));
        break;

    case AAP_PKT_TYPE_EAR_DETECTION: {
        mark_notifications_healthy();
        bool primary_in_ear = packet.data.ear_detection.primary_in_ear;
        bool secondary_in_ear = packet.data.ear_detection.secondary_in_ear;

        /* AirPods Max 1 reports only the primary AAP wear slot; its secondary
         * slot always reads "out", which would jam one-out auto-pause. Max 2
         * has two physical sensors, so preserve both raw AAP slots there. */
        g_mutex_lock(&app.state.lock);
        bool single_wear_sensor =
            airpods_model_uses_single_aap_wear_sensor(app.state.model);
        g_mutex_unlock(&app.state.lock);
        if (single_wear_sensor)
            secondary_in_ear = primary_in_ear;

        /* Poll replies are intentionally frequent while the AAP link is active.
         * Only publish actual transitions so steady-state polling performs
         * no journal or D-Bus work and cannot restart a removal timer after
         * its disconnect request has already been queued. */
        g_mutex_lock(&app.state.lock);
        bool state_changed = !app.ear_state_valid ||
            app.state.ear_detection.left_in_ear != primary_in_ear ||
            app.state.ear_detection.right_in_ear != secondary_in_ear;
        g_mutex_unlock(&app.state.lock);
        if (!state_changed)
            break;

        app.ear_state_valid = true;
        app.last_wear_state_valid = true;
        app.last_left_in_ear = primary_in_ear;
        app.last_right_in_ear = secondary_in_ear;
        g_message("Ear detection: primary=%s secondary=%s",
                  packet.data.ear_detection.primary_in_ear ? "in" : "out",
                  packet.data.ear_detection.secondary_in_ear ? "in" : "out");

        airpods_state_set_ear_detection(&app.state,
                                         primary_in_ear,
                                         secondary_in_ear);

        dbus_service_emit_ear_detection_changed(app.dbus_service,
                                                 app.state.ear_detection.left_in_ear,
                                                 app.state.ear_detection.right_in_ear);
        static const char *const wear_properties[] = {
            "LeftInEar", "RightInEar",
        };
        dbus_service_emit_properties_changed_many(
            app.dbus_service, wear_properties,
            G_N_ELEMENTS(wear_properties));

        /* Trigger media pause/resume based on ear detection */
        if (app.media_control) {
            media_control_on_ear_detection_changed(app.media_control,
                                                    app.state.ear_detection.left_in_ear,
                                                    app.state.ear_detection.right_in_ear);
        }
        bool completed_removal_cycle = removal_lifecycle_note_wear(
                &app.removal_lifecycle,
                app.pending_address,
                app.state.ear_detection.left_in_ear,
                app.state.ear_detection.right_in_ear);
        if (completed_removal_cycle) {
            g_debug("Completed same-device removal/reconnect lifecycle");
            maybe_reclaim_after_removal_reconnect();
        }
        update_removal_disconnect(app.state.ear_detection.left_in_ear,
                                  app.state.ear_detection.right_in_ear);
        break;
    }

    case AAP_PKT_TYPE_AUDIO_SOURCE: {
        const AapAudioSourceData *source = &packet.data.audio_source;
        bool local_source = audio_source_is_local(source);
        bool remote_source = audio_source_is_remote(source);
        bool was_yielded_to_remote =
            handoff_policy_is_yielded(&app.handoff_policy);
        bool previous_source_was_local =
            app.current_audio_source_valid &&
            app.current_audio_source.type != AAP_AUDIO_SOURCE_NONE &&
            audio_source_is_local(&app.current_audio_source);
        bool previous_source_was_remote =
            app.current_audio_source_valid &&
            app.current_audio_source.type != AAP_AUDIO_SOURCE_NONE &&
            audio_source_is_remote(&app.current_audio_source);
        bool entered_remote_source =
            app.local_audio_source_address_valid &&
            source->type != AAP_AUDIO_SOURCE_NONE &&
            !local_source &&
            !previous_source_was_remote;

        const char *source_name = source->type == AAP_AUDIO_SOURCE_NONE ? "none" :
                                  source->type == AAP_AUDIO_SOURCE_CALL ? "call" :
                                                                         "media";
        g_message("AirPods audio source changed: %s%s",
                  source_name,
                  local_source ? " (Linux)" :
                  (!app.local_audio_source_address_valid &&
                   source->type != AAP_AUDIO_SOURCE_NONE) ?
                      " (unclassified)" : "");

        /* Publish the new source before pausing players. A synchronous Pause
         * may cause an aggregate-stopped callback; it must observe that the
         * remote device, not Linux, is now the active source. */
        app.current_audio_source = *source;
        app.current_audio_source_valid = true;

        if (!app.config.handoff_enabled) {
            handoff_policy_reset(&app.handoff_policy);
        } else if (source->type != AAP_AUDIO_SOURCE_NONE && remote_source) {
            handoff_policy_note_source(&app.handoff_policy,
                                       HANDOFF_SOURCE_REMOTE);

            /* AirPods 4 needs an explicit acknowledgement that Linux yields
             * ownership when another device takes media or a call. Do this
             * once on the local/none -> remote transition, before pausing. */
            if (entered_remote_source && !was_yielded_to_remote)
                release_audio_from_linux();
            if (entered_remote_source)
                suppress_wear_reconnect_until_removal();

            bool linux_has_playing_media = app.media_control != NULL &&
                                           media_control_is_playing(app.media_control);
            if (previous_source_was_local || linux_has_playing_media) {
                g_message("Another device took AirPods audio; pausing Linux media");
                if (app.media_control != NULL)
                    media_control_pause_all_for_handoff(app.media_control);
            }
        } else if (local_source) {
            handoff_policy_note_source(&app.handoff_policy,
                                       HANDOFF_SOURCE_LOCAL);
        } else if (source->type == AAP_AUDIO_SOURCE_NONE) {
            handoff_policy_note_source(&app.handoff_policy,
                                       HANDOFF_SOURCE_NONE);
        } else {
            /* Without the local controller address, a non-NONE AudioSource
             * cannot safely be classified. Treat it as informational: calling
             * it remote would make Linux pause its own newly started player. */
            g_debug("Ignoring unclassified AirPods audio source until the local adapter identity is known");
        }

        /* NONE after a remote source is deliberately a no-op. AirPods 4
         * emits short MEDIA/CALL -> NONE gaps while smart routing is still in
         * progress; reclaiming here fights the iPhone and can strand the bud
         * between both hosts. */

        break;
    }

    case AAP_PKT_TYPE_OWNERSHIP_RELEASE_REQUEST: {
        if (!app.config.handoff_enabled)
            break;

        bool newly_yielded =
            !handoff_policy_is_yielded(&app.handoff_policy);
        handoff_policy_note_remote_request(&app.handoff_policy);
        app.last_audio_claim_time = 0;
        g_message("Another Apple device requested AirPods ownership; yielding Linux audio");
        if (newly_yielded)
            release_audio_from_linux();
        suppress_wear_reconnect_until_removal();
        if (app.media_control != NULL &&
            media_control_is_playing(app.media_control)) {
            media_control_pause_all_for_handoff(app.media_control);
        }
        break;
    }

    case AAP_PKT_TYPE_NOISE_CONTROL:
        g_message("Noise control mode: %s",
                  noise_control_mode_to_string(packet.data.noise_control));

        airpods_state_set_noise_control(&app.state, packet.data.noise_control);

        dbus_service_emit_noise_control_changed(app.dbus_service,
                                                 packet.data.noise_control);
        dbus_service_emit_properties_changed(app.dbus_service, "NoiseControlMode");
        break;

    case AAP_PKT_TYPE_CONV_AWARENESS:
        g_message("Conversational awareness: %s",
                  packet.data.conversational_awareness ? "enabled" : "disabled");

        airpods_state_set_conversational_awareness(&app.state,
                                                    packet.data.conversational_awareness);

        dbus_service_emit_properties_changed(app.dbus_service, "ConversationalAwareness");
        break;

    case AAP_PKT_TYPE_CA_DETECTION:
        g_debug("CA detection event: volume_level=%d", packet.data.ca_volume_level);
        break;

    case AAP_PKT_TYPE_LISTENING_MODES:
        g_message("Listening modes: off=%s transparency=%s anc=%s adaptive=%s (raw=0x%02X)",
                  packet.data.listening_modes.off_enabled ? "on" : "off",
                  packet.data.listening_modes.transparency_enabled ? "on" : "off",
                  packet.data.listening_modes.anc_enabled ? "on" : "off",
                  packet.data.listening_modes.adaptive_enabled ? "on" : "off",
                  packet.data.listening_modes.raw_value);

        airpods_state_set_listening_modes(&app.state,
                                           packet.data.listening_modes.off_enabled,
                                           packet.data.listening_modes.transparency_enabled,
                                           packet.data.listening_modes.anc_enabled,
                                           packet.data.listening_modes.adaptive_enabled);

        static const char *const listening_mode_properties[] = {
            "ListeningModeOff", "ListeningModeTransparency",
            "ListeningModeANC", "ListeningModeAdaptive",
        };
        dbus_service_emit_properties_changed_many(
            app.dbus_service, listening_mode_properties,
            G_N_ELEMENTS(listening_mode_properties));
        break;

    case AAP_PKT_TYPE_METADATA:
        g_message("Metadata received: device='%s' model='%s' manufacturer='%s'",
                  packet.data.metadata.device_name,
                  packet.data.metadata.model_number,
                  packet.data.metadata.manufacturer);

        /* Update model from model number */
        {
            AirPodsModel detected_model = airpods_model_from_number(packet.data.metadata.model_number);
            if (detected_model != AIRPODS_MODEL_UNKNOWN) {
                g_mutex_lock(&app.state.lock);
                app.state.model = detected_model;
                g_mutex_unlock(&app.state.lock);

                g_message("Detected AirPods model: %s", airpods_model_to_string(detected_model));
                static const char *const model_properties[] = {
                    "DeviceModel", "IsHeadphones", "SupportsANC",
                    "SupportsAdaptive", "DisplayName",
                };
                dbus_service_emit_properties_changed_many(
                    app.dbus_service, model_properties,
                    G_N_ELEMENTS(model_properties));

                /* A wear notification can precede metadata. Re-evaluate the
                 * removal timer now that it is safe to know whether this model
                 * has a matching disconnected-BLE reconnect implementation. */
                if (app.last_wear_state_valid) {
                    update_removal_disconnect(app.last_left_in_ear,
                                              app.last_right_in_ear);
                }
            }
        }
        break;

    default:
        break;
    }
}

/* ============================================================================
 * L2CAP reconnection with exponential backoff
 * ========================================================================== */

static gboolean reconnect_timeout_cb(gpointer user_data)
{
    (void)user_data;
    app.reconnect_timeout_id = 0;

    if (!app.bluez_connected || app.pending_address == NULL)
        return G_SOURCE_REMOVE;

    if (app.bt_conn && bt_connection_is_connected(app.bt_conn))
        return G_SOURCE_REMOVE;

    g_message("L2CAP reconnect attempt %d/%d to %s",
              app.reconnect_attempts, RECONNECT_MAX_ATTEMPTS, app.pending_address);

    /* On failure, the BT_STATE_ERROR callback schedules the next attempt */
    connect_to_airpods(app.pending_address, app.pending_name);

    return G_SOURCE_REMOVE;
}

static void schedule_reconnect(void)
{
    if (app.reconnect_timeout_id > 0)
        return;

    if (!app.bluez_connected || app.pending_address == NULL)
        return;

    if (app.reconnect_attempts >= RECONNECT_MAX_ATTEMPTS) {
        g_warning("Giving up L2CAP reconnection after %d attempts", app.reconnect_attempts);
        return;
    }

    guint delay = RECONNECT_BASE_DELAY_SEC << app.reconnect_attempts;  /* 2,4,8,16,32s */
    app.reconnect_attempts++;

    g_message("Scheduling L2CAP reconnect attempt %d/%d in %us",
              app.reconnect_attempts, RECONNECT_MAX_ATTEMPTS, delay);

    app.reconnect_timeout_id = g_timeout_add_seconds(delay, reconnect_timeout_cb, NULL);
}

static void cancel_reconnect(void)
{
    if (app.reconnect_timeout_id > 0) {
        g_source_remove(app.reconnect_timeout_id);
        app.reconnect_timeout_id = 0;
    }
    app.reconnect_attempts = 0;
}

static void on_bt_state_changed(BluetoothState state, const char *error, void *user_data)
{
    (void)user_data;

    switch (state) {
    case BT_STATE_CONNECTED:
        g_message("Bluetooth connected, sending handshake...");
        cancel_reconnect();
        reset_aap_initialization();
        app.connection_generation++;

        app.local_audio_source_address_valid =
            bt_connection_get_local_audio_source_address(
                app.bt_conn, app.local_audio_source_address);

        /* Attach to main loop for data reception */
        bt_connection_attach_to_mainloop(app.bt_conn, NULL);

        /* Start initialization. Feature configuration and notification
         * subscription are sent from on_bt_data_received only after their
         * corresponding ACKs. */
        /* Keep the main loop responsive while the control channel settles. */
        app.handshake_timeout_id = g_timeout_add(100,
                                                  send_handshake_timeout_cb,
                                                  NULL);

        /* Update state */
        airpods_state_set_device(&app.state,
                                  app.pending_name,
                                  app.pending_address,
                                  AIRPODS_MODEL_UNKNOWN);  /* Model detected later via metadata */

        /* Load and apply saved device profile */
        apply_device_profile(app.pending_address);

        /* Emit property changes BEFORE the DeviceConnected signal so the
         * extension's proxy cache is up-to-date when its handler reads
         * DisplayName for the connection notification. */
        dbus_service_emit_all_properties_changed(app.dbus_service);

        dbus_service_emit_device_connected(app.dbus_service,
                                            app.pending_address,
                                            app.pending_name);

        break;

    case BT_STATE_DISCONNECTED:
        g_message("Bluetooth disconnected");
        reset_aap_initialization();

        if (app.state.connected) {
            dbus_service_emit_device_disconnected(app.dbus_service,
                                                   app.state.device_address,
                                                   app.state.device_name);
        }

        airpods_state_reset(&app.state);
        dbus_service_emit_all_properties_changed(app.dbus_service);

        /* L2CAP dropped but the device is still connected at BlueZ level
         * (e.g. AirPods went idle): try to re-establish the link. */
        schedule_reconnect();
        break;

    case BT_STATE_ERROR:
        g_warning("Bluetooth error: %s", error ? error : "unknown");
        reset_aap_initialization();
        schedule_reconnect();
        break;

    default:
        break;
    }
}

/* ============================================================================
 * Device profile management
 * ========================================================================== */

static void apply_device_profile(const char *address)
{
    if (address == NULL || address[0] == '\0') {
        return;
    }

    DeviceProfile profile;
    bool has_profile = config_load_device_profile(address, &profile);

    if (!has_profile || !profile.has_saved_settings) {
        g_message("No saved profile for device %s, using defaults", address);
        return;
    }

    g_message("Applying saved profile for device %s", address);

    /* Apply display name */
    airpods_state_set_display_name(&app.state, profile.display_name);

    /* Apply listening modes */
    airpods_state_set_listening_modes(&app.state,
                                       profile.listening_modes.off_enabled,
                                       profile.listening_modes.transparency_enabled,
                                       profile.listening_modes.anc_enabled,
                                       profile.listening_modes.adaptive_enabled);

    /* Apply conversational awareness (will be sent after connection stabilizes) */
    g_mutex_lock(&app.state.lock);
    app.state.conversational_awareness = profile.conversational_awareness;
    app.state.adaptive_noise_level = profile.adaptive_noise_level;
    g_mutex_unlock(&app.state.lock);
}

typedef struct {
    char *address;
    guint connection_generation;
    guint stage;
    DeviceProfile profile;
} SavedSettingsTask;

static void saved_settings_task_free(gpointer data)
{
    SavedSettingsTask *task = data;
    if (task == NULL)
        return;

    g_free(task->address);
    g_free(task);
}

static gboolean apply_saved_settings_step(gpointer user_data)
{
    SavedSettingsTask *task = user_data;

    if (app.bt_conn == NULL || !bt_connection_is_connected(app.bt_conn) ||
        !app.aap_init.notifications_requested ||
        app.connection_generation != task->connection_generation ||
        !airpods_address_equal(app.pending_address, task->address) ||
        !airpods_address_equal(app.state.device_address, task->address)) {
        g_debug("Skipping stale saved-settings task for %s", task->address);
        app.saved_settings_timeout_id = 0;
        return G_SOURCE_REMOVE;
    }

    uint8_t packet[AAP_CONTROL_CMD_SIZE];
    switch (task->stage) {
    case 0: {
        uint8_t modes = 0;
        if (task->profile.listening_modes.off_enabled)
            modes |= AAP_LISTENING_MODE_OFF;
        if (task->profile.listening_modes.transparency_enabled)
            modes |= AAP_LISTENING_MODE_TRANSPARENCY;
        if (task->profile.listening_modes.anc_enabled)
            modes |= AAP_LISTENING_MODE_ANC;
        if (task->profile.listening_modes.adaptive_enabled)
            modes |= AAP_LISTENING_MODE_ADAPTIVE;
        aap_build_listening_modes_cmd(modes, packet);
        break;
    }
    case 1:
        aap_build_conv_awareness_cmd(
            task->profile.conversational_awareness, packet);
        break;
    case 2:
        aap_build_adaptive_level_cmd(
            task->profile.adaptive_noise_level, packet);
        break;
    default:
        app.saved_settings_timeout_id = 0;
        return G_SOURCE_REMOVE;
    }

    if (bt_connection_send(app.bt_conn, packet, sizeof(packet)) !=
        (ssize_t)sizeof(packet)) {
        g_warning("Failed to apply saved AirPods setting at stage %u",
                  task->stage);
        app.saved_settings_timeout_id = 0;
        return G_SOURCE_REMOVE;
    }

    task->stage++;
    if (task->stage >= 3) {
        app.saved_settings_timeout_id = 0;
        g_message("Applied saved settings to AirPods");
        return G_SOURCE_REMOVE;
    }

    return G_SOURCE_CONTINUE;
}

static void schedule_saved_settings(void)
{
    cancel_saved_settings();

    if (app.pending_address == NULL ||
        !app.aap_init.notifications_requested)
        return;

    SavedSettingsTask *task = g_new0(SavedSettingsTask, 1);
    task->address = g_strdup(app.pending_address);
    task->connection_generation = app.connection_generation;
    if (!config_load_device_profile(task->address, &task->profile) ||
        !task->profile.has_saved_settings) {
        saved_settings_task_free(task);
        return;
    }

    /* The feature handshake and notification request are complete. Apply the
     * three control values without sleeping in the GLib main thread. */
    app.saved_settings_timeout_id = g_timeout_add_full(
        G_PRIORITY_DEFAULT,
        100,
        apply_saved_settings_step,
        task,
        saved_settings_task_free);
}

/* ============================================================================
 * Connection management
 * ========================================================================== */

static void connect_to_airpods(const char *address, const char *name)
{
    if (address == NULL || address[0] == '\0') {
        g_warning("Cannot connect to AirPods without an address");
        return;
    }

    if (app.bt_conn &&
        bt_connection_get_state(app.bt_conn) != BT_STATE_DISCONNECTED) {
        g_message("AAP connection already active for %s; ignoring duplicate request for %s",
                  app.pending_address ? app.pending_address : "unknown", address);
        return;
    }

    /* Store pending info (dup first: on reconnect, address/name may alias
     * app.pending_address/app.pending_name) */
    char *addr_copy = g_strdup(address);
    char *name_copy = g_strdup(name);
    g_free(app.pending_address);
    g_free(app.pending_name);
    app.pending_address = addr_copy;
    app.pending_name = name_copy;

    /* Create new connection if needed */
    if (app.bt_conn == NULL) {
        app.bt_conn = bt_connection_new();
        bt_connection_set_data_callback(app.bt_conn, on_bt_data_received, NULL);
        bt_connection_set_state_callback(app.bt_conn, on_bt_state_changed, NULL);
    }

    g_message("Connecting to AirPods: %s (%s)", name, address);

    if (!bt_connection_connect(app.bt_conn, address)) {
        g_warning("Failed to initiate connection");
    }
}

static void disconnect_from_airpods(void)
{
    cancel_removal_disconnect();
    cancel_ear_detection_poll();
    if (app.bt_conn) {
        bt_connection_disconnect(app.bt_conn);
    }
}

static void on_media_playback_started(void *user_data)
{
    (void)user_data;

    if (!app.config.handoff_enabled)
        return;

    if (app.bt_conn == NULL || !bt_connection_is_connected(app.bt_conn)) {
        g_debug("Media playback started while AirPods AAP is disconnected");
        return;
    }

    if (app.current_audio_source_valid &&
        app.current_audio_source.type != AAP_AUDIO_SOURCE_NONE &&
        audio_source_is_local(&app.current_audio_source) &&
        !handoff_policy_is_yielded(&app.handoff_policy)) {
        g_debug("Linux already owns the AirPods audio source");
        return;
    }

    if (app.current_audio_source_valid &&
        app.current_audio_source.type == AAP_AUDIO_SOURCE_CALL &&
        audio_source_is_remote(&app.current_audio_source)) {
        g_message("Linux playback started during a call on another device; pausing until the call releases AirPods");
        if (app.media_control != NULL)
            media_control_pause_all_for_handoff(app.media_control);
        handoff_policy_note_remote_request(&app.handoff_policy);
        return;
    }

    /* A Playing transition is the only event allowed to take ownership back
     * after an Apple-device handoff. The user has already started playback,
     * so merely forget our old pause reason without sending another Play. */
    if (app.media_control != NULL)
        media_control_clear_handoff_pause(app.media_control);

    gint64 now = g_get_monotonic_time();
    if (app.last_audio_claim_time > 0 &&
        now - app.last_audio_claim_time < AUDIO_CLAIM_DEBOUNCE_USEC &&
        !handoff_policy_is_yielded(&app.handoff_policy)) {
        g_debug("Skipping duplicate AirPods ownership claim from MPRIS");
        return;
    }

    g_message("Linux media playback started; requesting AirPods handoff");
    if (claim_audio_for_linux(true))
        app.reclaim_after_removal_reconnect = false;
}

static void on_media_playback_stopped(void *user_data)
{
    (void)user_data;

    if (!app.config.handoff_enabled ||
        app.bt_conn == NULL || !bt_connection_is_connected(app.bt_conn) ||
        handoff_policy_is_yielded(&app.handoff_policy) ||
        (app.current_audio_source_valid &&
         app.current_audio_source.type != AAP_AUDIO_SOURCE_NONE &&
         audio_source_is_remote(&app.current_audio_source))) {
        return;
    }

    app.reclaim_after_removal_reconnect = false;
    g_message("All Linux media stopped; releasing AirPods audio ownership");
    if (release_audio_from_linux()) {
        /* Treat the successful command as an optimistic release so playback
         * restarted before the AudioSource notification can claim again. */
        memset(&app.current_audio_source, 0,
               sizeof(app.current_audio_source));
        app.current_audio_source_valid = true;
        handoff_policy_reset(&app.handoff_policy);
        app.last_audio_claim_time = 0;
    }
}

/* ============================================================================
 * BlueZ callbacks
 * ========================================================================== */

static void on_bluez_device_connected(const BluezDeviceInfo *device, void *user_data)
{
    (void)user_data;
    g_message("BlueZ: AirPods connected - %s (%s)", device->name, device->address);

    if (device->address == NULL || device->address[0] == '\0')
        return;

    ConnectionDecision decision = connection_policy_device_connected(
        app.pending_address, device->address);
    if (decision == CONNECTION_DECISION_IGNORE)
        return;

    bool resumes_removal_cycle = removal_lifecycle_note_connected(
        &app.removal_lifecycle, device->address);

    if (decision == CONNECTION_DECISION_CURRENT) {
        /* Duplicate Connected events are common during BlueZ discovery. They
         * must update link availability without resetting a healthy AAP
         * channel or its reconnect backoff. */
        app.bluez_connected = true;
        if (app.bt_conn == NULL ||
            bt_connection_get_state(app.bt_conn) == BT_STATE_DISCONNECTED) {
            connect_to_airpods(device->address, device->name);
        }
        return;
    }

    if (decision == CONNECTION_DECISION_SWITCH) {
        g_message("Switching EarPort from %s to newly connected AirPods %s",
                  app.pending_address, device->address);

        /* Suppress reconnect scheduling from the intentional old-socket
         * close. connect_to_airpods installs the new target afterwards. */
        app.bluez_connected = false;
        cancel_removal_disconnect();
        cancel_reconnect();
        disconnect_from_airpods();
    }

    /* This is a real BlueZ device selection, not a transient reconnect of
     * the same AAP socket. Do not clear handoff pause ownership on the latter:
     * the Apple source may still need to release before Linux can resume. */
    reset_audio_handoff_state();
    if (app.media_control != NULL && !resumes_removal_cycle)
        media_control_reset_device_state(app.media_control);
    if (!resumes_removal_cycle) {
        app.reclaim_after_removal_reconnect = false;
        app.last_wear_state_valid = false;
        app.last_left_in_ear = false;
        app.last_right_in_ear = false;
    } else {
        g_debug("Preserving ear-pause ownership for same-device rewear");
        if (app.media_control != NULL) {
            media_control_cancel_audio_route(app.media_control);
            media_control_clear_handoff_pause(app.media_control);
        }
    }

    app.bluez_connected = true;
    app.reconnect_attempts = 0;
    connect_to_airpods(device->address, device->name);
    if (app.media_control != NULL)
        media_control_route_audio_to_device(app.media_control, device->address);
}

static void on_bluez_device_disconnected(const BluezDeviceInfo *device, void *user_data)
{
    (void)user_data;
    g_message("BlueZ: AirPods disconnected - %s (%s)", device->name, device->address);

    ConnectionDecision decision = connection_policy_device_disconnected(
        app.pending_address, device->address);
    if (decision != CONNECTION_DECISION_DISCONNECT_CURRENT) {
        g_message("Ignoring disconnect for non-current AirPods %s",
                  device->address ? device->address : "unknown");
        return;
    }

    char *disconnected_address = g_strdup(app.pending_address);
    bool fully_removed = app.last_wear_state_valid &&
        wear_policy_fully_removed(app.last_left_in_ear,
                                  app.last_right_in_ear);
    bool preserve_removal_cycle = removal_lifecycle_matches(
        &app.removal_lifecycle, disconnected_address);
    if (fully_removed) {
        if (!preserve_removal_cycle) {
            removal_lifecycle_mark(&app.removal_lifecycle,
                                   disconnected_address);
            preserve_removal_cycle = true;
        }
        /* The BlueZ signal can beat the asynchronous Disconnect reply, and a
         * spontaneous radio drop has no reply at all. Rearm immediately from
         * the richer paired-device cache. */
        bluez_monitor_rearm_auto_connect_after_removal(
            app.bluez_monitor, disconnected_address);
    } else if (!preserve_removal_cycle) {
        removal_lifecycle_clear(&app.removal_lifecycle);
    }
    bool reconnect_after_pending_removal = preserve_removal_cycle &&
        removal_lifecycle_note_disconnected(&app.removal_lifecycle,
                                            disconnected_address);

    app.bluez_connected = false;
    cancel_removal_disconnect();
    cancel_reconnect();
    reset_aap_initialization();
    disconnect_from_airpods();

    reset_audio_handoff_state();
    if (app.media_control != NULL) {
        if (preserve_removal_cycle) {
            media_control_cancel_audio_route(app.media_control);
            media_control_clear_handoff_pause(app.media_control);
        } else {
            media_control_reset_device_state(app.media_control);
        }
    }

    g_clear_pointer(&app.pending_address, g_free);
    g_clear_pointer(&app.pending_name, g_free);

    /* If another paired AirPods remains connected, move the sole AAP channel
     * to it. This also makes startup deterministic when BlueZ reports more
     * than one already-connected device. */
    bool reconnect_queued = reconnect_after_pending_removal &&
        bluez_monitor_connect_device(app.bluez_monitor,
                                     disconnected_address);
    app.reclaim_after_removal_reconnect = reconnect_queued;
    if (reconnect_after_pending_removal && !reconnect_queued) {
        g_warning("Rapidly reworn AirPods could not be reconnected immediately");
    }

    if (!reconnect_queued) {
        BluezDeviceInfo *fallback = bluez_monitor_find_connected_device(
            app.bluez_monitor, disconnected_address);
        if (fallback != NULL) {
            g_message("Falling back to connected AirPods %s", fallback->address);
            on_bluez_device_connected(fallback, NULL);
            bluez_device_info_free(fallback);
        }
    }

    if (!preserve_removal_cycle) {
        app.reclaim_after_removal_reconnect = false;
        app.last_wear_state_valid = false;
        app.last_left_in_ear = false;
        app.last_right_in_ear = false;
    }

    g_free(disconnected_address);
}

/* ============================================================================
 * D-Bus method callbacks
 * ========================================================================== */

static void on_set_noise_control(NoiseControlMode mode, void *user_data)
{
    (void)user_data;

    if (!app.bt_conn || !bt_connection_is_connected(app.bt_conn)) {
        g_warning("Cannot set noise control: not connected");
        return;
    }

    uint8_t packet[AAP_CONTROL_CMD_SIZE];
    aap_build_noise_control_cmd(mode, packet);
    if (bt_connection_send(app.bt_conn, packet, AAP_CONTROL_CMD_SIZE) !=
        (ssize_t)AAP_CONTROL_CMD_SIZE) {
        g_warning("Failed to set AirPods noise-control mode");
    }
}

static void on_set_conv_awareness(bool enabled, void *user_data)
{
    (void)user_data;

    if (!app.bt_conn || !bt_connection_is_connected(app.bt_conn)) {
        g_warning("Cannot set conversational awareness: not connected");
        return;
    }

    uint8_t packet[AAP_CONTROL_CMD_SIZE];
    aap_build_conv_awareness_cmd(enabled, packet);
    if (bt_connection_send(app.bt_conn, packet, AAP_CONTROL_CMD_SIZE) !=
        (ssize_t)AAP_CONTROL_CMD_SIZE) {
        g_warning("Failed to set AirPods conversational awareness");
        return;
    }

    airpods_state_set_conversational_awareness(&app.state, enabled);
    dbus_service_emit_properties_changed(app.dbus_service,
                                         "ConversationalAwareness");

    /* Save to device profile */
    if (app.state.device_address && app.state.device_address[0] != '\0') {
        DeviceProfile profile;
        config_load_device_profile(app.state.device_address, &profile);
        profile.conversational_awareness = enabled;
        config_save_device_profile(app.state.device_address, &profile);
    }
}

static void on_set_adaptive_level(int level, void *user_data)
{
    (void)user_data;

    if (!app.bt_conn || !bt_connection_is_connected(app.bt_conn)) {
        g_warning("Cannot set adaptive level: not connected");
        return;
    }

    uint8_t packet[AAP_CONTROL_CMD_SIZE];
    aap_build_adaptive_level_cmd(level, packet);
    if (bt_connection_send(app.bt_conn, packet, AAP_CONTROL_CMD_SIZE) !=
        (ssize_t)AAP_CONTROL_CMD_SIZE) {
        g_warning("Failed to set AirPods adaptive noise level");
        return;
    }

    airpods_state_set_adaptive_noise_level(&app.state, level);
    dbus_service_emit_properties_changed(app.dbus_service,
                                         "AdaptiveNoiseLevel");

    /* Save to device profile */
    if (app.state.device_address && app.state.device_address[0] != '\0') {
        DeviceProfile profile;
        config_load_device_profile(app.state.device_address, &profile);
        profile.adaptive_noise_level = level;
        config_save_device_profile(app.state.device_address, &profile);
    }
}

static void on_set_ear_pause_mode(int mode, void *user_data)
{
    (void)user_data;

    g_message("Setting ear pause mode to %d", mode);

    /* Update state */
    g_mutex_lock(&app.state.lock);
    app.state.ear_pause_mode = mode;
    g_mutex_unlock(&app.state.lock);

    /* Update media control */
    if (app.media_control) {
        media_control_set_ear_pause_mode(app.media_control, (EarPauseMode)mode);
    }

    /* Save to config file */
    app.config.ear_pause_mode = mode;
    config_save(&app.config);

    /* Notify property change */
    dbus_service_emit_properties_changed(app.dbus_service, "EarPauseMode");
}

static void on_set_listening_modes(bool off, bool transparency, bool anc, bool adaptive, void *user_data)
{
    (void)user_data;

    if (!app.bt_conn || !bt_connection_is_connected(app.bt_conn)) {
        g_warning("Cannot set listening modes: not connected");
        return;
    }

    /* Build the bitmask */
    uint8_t modes = 0;
    if (off) modes |= AAP_LISTENING_MODE_OFF;
    if (transparency) modes |= AAP_LISTENING_MODE_TRANSPARENCY;
    if (anc) modes |= AAP_LISTENING_MODE_ANC;
    if (adaptive) modes |= AAP_LISTENING_MODE_ADAPTIVE;

    /* Ensure at least 2 modes are enabled */
    int count = (off ? 1 : 0) + (transparency ? 1 : 0) + (anc ? 1 : 0) + (adaptive ? 1 : 0);
    if (count < 2) {
        g_warning("At least 2 listening modes must be enabled");
        return;
    }

    g_message("Setting listening modes: 0x%02X", modes);

    uint8_t packet[AAP_CONTROL_CMD_SIZE];
    aap_build_listening_modes_cmd(modes, packet);
    if (bt_connection_send(app.bt_conn, packet, AAP_CONTROL_CMD_SIZE) !=
        (ssize_t)AAP_CONTROL_CMD_SIZE) {
        g_warning("Failed to set AirPods listening modes");
        return;
    }

    /* Update local state immediately */
    airpods_state_set_listening_modes(&app.state, off, transparency, anc, adaptive);

    /* Save to device profile */
    if (app.state.device_address && app.state.device_address[0] != '\0') {
        DeviceProfile profile;
        config_load_device_profile(app.state.device_address, &profile);
        profile.listening_modes.off_enabled = off;
        profile.listening_modes.transparency_enabled = transparency;
        profile.listening_modes.anc_enabled = anc;
        profile.listening_modes.adaptive_enabled = adaptive;
        config_save_device_profile(app.state.device_address, &profile);
    }

    static const char *const listening_mode_properties[] = {
        "ListeningModeOff", "ListeningModeTransparency",
        "ListeningModeANC", "ListeningModeAdaptive",
    };
    dbus_service_emit_properties_changed_many(
        app.dbus_service, listening_mode_properties,
        G_N_ELEMENTS(listening_mode_properties));
}

static void on_set_display_name(const char *name, void *user_data)
{
    (void)user_data;

    char display_name[DEVICE_PROFILE_DISPLAY_NAME_SIZE];
    config_copy_display_name(display_name, name);
    g_message("Setting display name to '%s'", display_name);

    /* Update state */
    airpods_state_set_display_name(&app.state, display_name);

    /* Save to device profile */
    if (app.state.device_address && app.state.device_address[0] != '\0') {
        DeviceProfile profile;
        config_load_device_profile(app.state.device_address, &profile);
        config_copy_display_name(profile.display_name, display_name);
        config_save_device_profile(app.state.device_address, &profile);
    }

    /* Notify property change */
    dbus_service_emit_properties_changed(app.dbus_service, "DisplayName");
}

/* ============================================================================
 * Signal handlers
 * ========================================================================== */

static gboolean on_sigint(gpointer user_data)
{
    (void)user_data;
    g_message("Received SIGINT, shutting down...");
    g_main_loop_quit(app.main_loop);
    return G_SOURCE_REMOVE;
}

static gboolean on_sigterm(gpointer user_data)
{
    (void)user_data;
    g_message("Received SIGTERM, shutting down...");
    g_main_loop_quit(app.main_loop);
    return G_SOURCE_REMOVE;
}

/* ============================================================================
 * Main
 * ========================================================================== */

static void cleanup(void)
{
    g_message("Cleaning up...");

    app.bluez_connected = false;
    cancel_reconnect();
    reset_aap_initialization();

    if (app.bt_conn) {
        bt_connection_free(app.bt_conn);
        app.bt_conn = NULL;
    }

    if (app.bluez_monitor) {
        bluez_monitor_free(app.bluez_monitor);
        app.bluez_monitor = NULL;
    }

    if (app.dbus_service) {
        dbus_service_free(app.dbus_service);
        app.dbus_service = NULL;
    }

    if (app.media_control) {
        media_control_free(app.media_control);
        app.media_control = NULL;
    }

    g_free(app.pending_address);
    g_free(app.pending_name);
    removal_lifecycle_clear(&app.removal_lifecycle);
    app.reclaim_after_removal_reconnect = false;

    airpods_state_cleanup(&app.state);

    if (app.main_loop) {
        g_main_loop_unref(app.main_loop);
        app.main_loop = NULL;
    }
}

int main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    g_message("EarPort Daemon %s starting...", EARPORT_VERSION);

    /* Load configuration */
    config_load(&app.config);

    /* Initialize state */
    airpods_state_init(&app.state);

    /* Create main loop */
    app.main_loop = g_main_loop_new(NULL, FALSE);

    /* Set up signal handlers */
    g_unix_signal_add(SIGINT, on_sigint, NULL);
    g_unix_signal_add(SIGTERM, on_sigterm, NULL);

    /* Create D-Bus service */
    app.dbus_service = dbus_service_new(&app.state);
    if (app.dbus_service == NULL) {
        g_critical("Failed to create D-Bus service");
        cleanup();
        return 1;
    }

    dbus_service_set_noise_control_callback(app.dbus_service, on_set_noise_control, NULL);
    dbus_service_set_conv_awareness_callback(app.dbus_service, on_set_conv_awareness, NULL);
    dbus_service_set_adaptive_level_callback(app.dbus_service, on_set_adaptive_level, NULL);
    dbus_service_set_ear_pause_mode_callback(app.dbus_service, on_set_ear_pause_mode, NULL);
    dbus_service_set_listening_modes_callback(app.dbus_service, on_set_listening_modes, NULL);
    dbus_service_set_display_name_callback(app.dbus_service, on_set_display_name, NULL);

    if (!dbus_service_start(app.dbus_service)) {
        g_critical("Failed to start D-Bus service");
        cleanup();
        return 1;
    }

    /* Create media control for MPRIS integration */
    app.media_control = media_control_new();
    if (app.media_control == NULL) {
        g_warning("Failed to create media control (MPRIS pause/resume disabled)");
    } else {
        /* Load ear pause mode from config */
        app.state.ear_pause_mode = app.config.ear_pause_mode;
        media_control_set_ear_pause_mode(app.media_control, (EarPauseMode)app.config.ear_pause_mode);
        media_control_set_playback_started_callback(app.media_control,
                                                    on_media_playback_started,
                                                    NULL);
        media_control_set_playback_stopped_callback(app.media_control,
                                                    on_media_playback_stopped,
                                                    NULL);
        g_message("Media control enabled (ear_pause_mode=%d)", app.config.ear_pause_mode);
    }

    /* Create BlueZ monitor */
    app.bluez_monitor = bluez_monitor_new();
    if (app.bluez_monitor == NULL) {
        g_critical("Failed to create BlueZ monitor");
        cleanup();
        return 1;
    }

    bluez_monitor_set_connected_callback(app.bluez_monitor, on_bluez_device_connected, NULL);
    bluez_monitor_set_disconnected_callback(app.bluez_monitor, on_bluez_device_disconnected, NULL);
    bluez_monitor_set_auto_connect_on_wear(app.bluez_monitor,
                                           app.config.auto_connect_on_wear);

    if (!bluez_monitor_start(app.bluez_monitor)) {
        g_critical("Failed to start BlueZ monitor");
        cleanup();
        return 1;
    }

    /* Check for already connected devices */
    bluez_monitor_check_existing_devices(app.bluez_monitor);

    g_message("EarPort Daemon running. Press Ctrl+C to quit.");

    /* Run main loop */
    g_main_loop_run(app.main_loop);

    /* Cleanup */
    cleanup();

    g_message("EarPort Daemon stopped.");
    return 0;
}
