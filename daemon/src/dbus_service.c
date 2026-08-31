/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2024 EarPort Contributors
 */

#include "dbus_service.h"
#include <string.h>

/* D-Bus introspection XML */
static const gchar introspection_xml[] =
    "<node>"
    "  <interface name='" DBUS_INTERFACE_NAME "'>"
    "    <property name='Connected' type='b' access='read'/>"
    "    <property name='DeviceName' type='s' access='read'/>"
    "    <property name='DeviceAddress' type='s' access='read'/>"
    "    <property name='DeviceModel' type='s' access='read'/>"
    "    <property name='DisplayName' type='s' access='read'/>"
    "    <property name='IsHeadphones' type='b' access='read'/>"
    "    <property name='SupportsANC' type='b' access='read'/>"
    "    <property name='SupportsAdaptive' type='b' access='read'/>"
    "    <property name='BatteryLeft' type='i' access='read'/>"
    "    <property name='BatteryRight' type='i' access='read'/>"
    "    <property name='BatteryCase' type='i' access='read'/>"
    "    <property name='ChargingLeft' type='b' access='read'/>"
    "    <property name='ChargingRight' type='b' access='read'/>"
    "    <property name='ChargingCase' type='b' access='read'/>"
    "    <property name='NoiseControlMode' type='s' access='read'/>"
    "    <property name='ConversationalAwareness' type='b' access='read'/>"
    "    <property name='LeftInEar' type='b' access='read'/>"
    "    <property name='RightInEar' type='b' access='read'/>"
    "    <property name='AdaptiveNoiseLevel' type='i' access='read'/>"
    "    <property name='EarPauseMode' type='i' access='read'/>"
    "    <property name='ListeningModeOff' type='b' access='read'/>"
    "    <property name='ListeningModeTransparency' type='b' access='read'/>"
    "    <property name='ListeningModeANC' type='b' access='read'/>"
    "    <property name='ListeningModeAdaptive' type='b' access='read'/>"
    "    <method name='SetNoiseControlMode'>"
    "      <arg type='s' name='mode' direction='in'/>"
    "    </method>"
    "    <method name='SetConversationalAwareness'>"
    "      <arg type='b' name='enabled' direction='in'/>"
    "    </method>"
    "    <method name='SetAdaptiveNoiseLevel'>"
    "      <arg type='i' name='level' direction='in'/>"
    "    </method>"
    "    <method name='SetEarPauseMode'>"
    "      <arg type='i' name='mode' direction='in'/>"
    "    </method>"
    "    <method name='SetListeningModes'>"
    "      <arg type='b' name='off' direction='in'/>"
    "      <arg type='b' name='transparency' direction='in'/>"
    "      <arg type='b' name='anc' direction='in'/>"
    "      <arg type='b' name='adaptive' direction='in'/>"
    "    </method>"
    "    <method name='SetDisplayName'>"
    "      <arg type='s' name='name' direction='in'/>"
    "    </method>"
    "    <signal name='DeviceConnected'>"
    "      <arg type='s' name='address'/>"
    "      <arg type='s' name='name'/>"
    "    </signal>"
    "    <signal name='DeviceDisconnected'>"
    "      <arg type='s' name='address'/>"
    "      <arg type='s' name='name'/>"
    "    </signal>"
    "    <signal name='BatteryChanged'>"
    "      <arg type='i' name='left'/>"
    "      <arg type='i' name='right'/>"
    "      <arg type='i' name='case_battery'/>"
    "    </signal>"
    "    <signal name='NoiseControlModeChanged'>"
    "      <arg type='s' name='mode'/>"
    "    </signal>"
    "    <signal name='EarDetectionChanged'>"
    "      <arg type='b' name='leftInEar'/>"
    "      <arg type='b' name='rightInEar'/>"
    "    </signal>"
    "  </interface>"
    "</node>";

struct DbusService {
    GDBusConnection *connection;
    GDBusNodeInfo *introspection_data;
    guint registration_id;
    bool owns_name;

    AirPodsState *state;

    DbusNoiseControlCallback noise_control_callback;
    void *noise_control_user_data;

    DbusConvAwarenessCallback conv_awareness_callback;
    void *conv_awareness_user_data;

    DbusAdaptiveLevelCallback adaptive_level_callback;
    void *adaptive_level_user_data;

    DbusEarPauseModeCallback ear_pause_mode_callback;
    void *ear_pause_mode_user_data;

    DbusListeningModesCallback listening_modes_callback;
    void *listening_modes_user_data;

    DbusDisplayNameCallback display_name_callback;
    void *display_name_user_data;
};

static GVariant *get_property(GDBusConnection *connection G_GNUC_UNUSED,
                               const gchar *sender G_GNUC_UNUSED,
                               const gchar *object_path G_GNUC_UNUSED,
                               const gchar *interface_name G_GNUC_UNUSED,
                               const gchar *property_name,
                               GError **error G_GNUC_UNUSED,
                               gpointer user_data)
{
    DbusService *service = user_data;
    AirPodsState *state = service->state;

    g_mutex_lock(&state->lock);

    GVariant *result = NULL;

    if (g_strcmp0(property_name, "Connected") == 0) {
        result = g_variant_new_boolean(state->connected);
    } else if (g_strcmp0(property_name, "DeviceName") == 0) {
        result = g_variant_new_string(state->device_name ? state->device_name : "");
    } else if (g_strcmp0(property_name, "DeviceAddress") == 0) {
        result = g_variant_new_string(state->device_address ? state->device_address : "");
    } else if (g_strcmp0(property_name, "DeviceModel") == 0) {
        result = g_variant_new_string(airpods_model_to_string(state->model));
    } else if (g_strcmp0(property_name, "DisplayName") == 0) {
        result = g_variant_new_string(airpods_state_get_display_name(state));
    } else if (g_strcmp0(property_name, "IsHeadphones") == 0) {
        result = g_variant_new_boolean(airpods_model_is_headphones(state->model));
    } else if (g_strcmp0(property_name, "SupportsANC") == 0) {
        result = g_variant_new_boolean(airpods_model_supports_anc(state->model));
    } else if (g_strcmp0(property_name, "SupportsAdaptive") == 0) {
        result = g_variant_new_boolean(airpods_model_supports_adaptive(state->model));
    } else if (g_strcmp0(property_name, "BatteryLeft") == 0) {
        result = g_variant_new_int32(state->battery.left.level);
    } else if (g_strcmp0(property_name, "BatteryRight") == 0) {
        result = g_variant_new_int32(state->battery.right.level);
    } else if (g_strcmp0(property_name, "BatteryCase") == 0) {
        result = g_variant_new_int32(state->battery.case_battery.level);
    } else if (g_strcmp0(property_name, "ChargingLeft") == 0) {
        result = g_variant_new_boolean(state->battery.left.status == BATTERY_STATUS_CHARGING);
    } else if (g_strcmp0(property_name, "ChargingRight") == 0) {
        result = g_variant_new_boolean(state->battery.right.status == BATTERY_STATUS_CHARGING);
    } else if (g_strcmp0(property_name, "ChargingCase") == 0) {
        result = g_variant_new_boolean(state->battery.case_battery.status == BATTERY_STATUS_CHARGING);
    } else if (g_strcmp0(property_name, "NoiseControlMode") == 0) {
        result = g_variant_new_string(noise_control_mode_to_string(state->noise_control_mode));
    } else if (g_strcmp0(property_name, "ConversationalAwareness") == 0) {
        result = g_variant_new_boolean(state->conversational_awareness);
    } else if (g_strcmp0(property_name, "LeftInEar") == 0) {
        result = g_variant_new_boolean(state->ear_detection.left_in_ear);
    } else if (g_strcmp0(property_name, "RightInEar") == 0) {
        result = g_variant_new_boolean(state->ear_detection.right_in_ear);
    } else if (g_strcmp0(property_name, "AdaptiveNoiseLevel") == 0) {
        result = g_variant_new_int32(state->adaptive_noise_level);
    } else if (g_strcmp0(property_name, "EarPauseMode") == 0) {
        result = g_variant_new_int32(state->ear_pause_mode);
    } else if (g_strcmp0(property_name, "ListeningModeOff") == 0) {
        result = g_variant_new_boolean(state->listening_modes.off_enabled);
    } else if (g_strcmp0(property_name, "ListeningModeTransparency") == 0) {
        result = g_variant_new_boolean(state->listening_modes.transparency_enabled);
    } else if (g_strcmp0(property_name, "ListeningModeANC") == 0) {
        result = g_variant_new_boolean(state->listening_modes.anc_enabled);
    } else if (g_strcmp0(property_name, "ListeningModeAdaptive") == 0) {
        result = g_variant_new_boolean(state->listening_modes.adaptive_enabled);
    }

    g_mutex_unlock(&state->lock);

    return result;
}

static void handle_method_call(GDBusConnection *connection G_GNUC_UNUSED,
                                const gchar *sender G_GNUC_UNUSED,
                                const gchar *object_path G_GNUC_UNUSED,
                                const gchar *interface_name G_GNUC_UNUSED,
                                const gchar *method_name,
                                GVariant *parameters,
                                GDBusMethodInvocation *invocation,
                                gpointer user_data)
{
    DbusService *service = user_data;

    if (g_strcmp0(method_name, "SetNoiseControlMode") == 0) {
        const gchar *mode_str = NULL;
        g_variant_get(parameters, "(&s)", &mode_str);

        NoiseControlMode mode = noise_control_mode_from_string(mode_str);
        g_message("D-Bus: SetNoiseControlMode(%s) -> %d", mode_str, mode);

        if (service->noise_control_callback) {
            service->noise_control_callback(mode, service->noise_control_user_data);
        }

        g_dbus_method_invocation_return_value(invocation, NULL);

    } else if (g_strcmp0(method_name, "SetConversationalAwareness") == 0) {
        gboolean enabled = FALSE;
        g_variant_get(parameters, "(b)", &enabled);

        g_message("D-Bus: SetConversationalAwareness(%s)", enabled ? "true" : "false");

        if (service->conv_awareness_callback) {
            service->conv_awareness_callback(enabled, service->conv_awareness_user_data);
        }

        g_dbus_method_invocation_return_value(invocation, NULL);

    } else if (g_strcmp0(method_name, "SetAdaptiveNoiseLevel") == 0) {
        gint32 level = 0;
        g_variant_get(parameters, "(i)", &level);

        if (level < 0 || level > 100) {
            g_dbus_method_invocation_return_error(
                invocation,
                G_DBUS_ERROR,
                G_DBUS_ERROR_INVALID_ARGS,
                "Adaptive noise level must be between 0 and 100");
            return;
        }

        g_message("D-Bus: SetAdaptiveNoiseLevel(%d)", level);

        if (service->adaptive_level_callback) {
            service->adaptive_level_callback(level, service->adaptive_level_user_data);
        }

        g_dbus_method_invocation_return_value(invocation, NULL);

    } else if (g_strcmp0(method_name, "SetEarPauseMode") == 0) {
        gint32 mode = 0;
        g_variant_get(parameters, "(i)", &mode);

        if (mode < 0 || mode > 2) {
            g_dbus_method_invocation_return_error(
                invocation,
                G_DBUS_ERROR,
                G_DBUS_ERROR_INVALID_ARGS,
                "Ear pause mode must be between 0 and 2");
            return;
        }

        g_message("D-Bus: SetEarPauseMode(%d)", mode);

        if (service->ear_pause_mode_callback) {
            service->ear_pause_mode_callback(mode, service->ear_pause_mode_user_data);
        }

        g_dbus_method_invocation_return_value(invocation, NULL);

    } else if (g_strcmp0(method_name, "SetListeningModes") == 0) {
        gboolean off = FALSE, transparency = FALSE, anc = FALSE, adaptive = FALSE;
        g_variant_get(parameters, "(bbbb)", &off, &transparency, &anc, &adaptive);

        g_message("D-Bus: SetListeningModes(off=%s, transparency=%s, anc=%s, adaptive=%s)",
                  off ? "true" : "false",
                  transparency ? "true" : "false",
                  anc ? "true" : "false",
                  adaptive ? "true" : "false");

        if (service->listening_modes_callback) {
            service->listening_modes_callback(off, transparency, anc, adaptive,
                                               service->listening_modes_user_data);
        }

        g_dbus_method_invocation_return_value(invocation, NULL);

    } else if (g_strcmp0(method_name, "SetDisplayName") == 0) {
        const gchar *name = NULL;
        g_variant_get(parameters, "(&s)", &name);

        g_message("D-Bus: SetDisplayName('%s')", name ? name : "");

        if (service->display_name_callback) {
            service->display_name_callback(name, service->display_name_user_data);
        }

        g_dbus_method_invocation_return_value(invocation, NULL);

    } else {
        g_dbus_method_invocation_return_error(invocation,
                                               G_DBUS_ERROR,
                                               G_DBUS_ERROR_UNKNOWN_METHOD,
                                               "Unknown method: %s",
                                               method_name);
    }
}

static const GDBusInterfaceVTable interface_vtable = {
    .method_call = handle_method_call,
    .get_property = get_property,
    .set_property = NULL,  /* No writable properties */
};

static void release_bus_name(DbusService *service)
{
    if (!service->owns_name || service->connection == NULL)
        return;

    GError *error = NULL;
    GVariant *reply = g_dbus_connection_call_sync(
        service->connection,
        "org.freedesktop.DBus",
        "/org/freedesktop/DBus",
        "org.freedesktop.DBus",
        "ReleaseName",
        g_variant_new("(s)", DBUS_SERVICE_NAME),
        G_VARIANT_TYPE("(u)"),
        G_DBUS_CALL_FLAGS_NONE,
        2000,
        NULL,
        &error);

    if (reply != NULL)
        g_variant_unref(reply);
    if (error != NULL) {
        g_warning("Failed to release D-Bus name: %s", error->message);
        g_error_free(error);
    }
    service->owns_name = false;
}

DbusService *dbus_service_new(AirPodsState *state)
{
    DbusService *service = g_new0(DbusService, 1);
    service->state = state;

    GError *error = NULL;
    service->introspection_data = g_dbus_node_info_new_for_xml(introspection_xml, &error);
    if (error) {
        g_warning("Failed to parse introspection XML: %s", error->message);
        g_error_free(error);
        g_free(service);
        return NULL;
    }

    return service;
}

void dbus_service_free(DbusService *service)
{
    if (service == NULL)
        return;

    dbus_service_stop(service);

    if (service->introspection_data)
        g_dbus_node_info_unref(service->introspection_data);

    g_free(service);
}

bool dbus_service_start(DbusService *service)
{
    enum {
        DBUS_NAME_FLAG_DO_NOT_QUEUE = 1u << 2,
        DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER = 1,
        DBUS_REQUEST_NAME_REPLY_ALREADY_OWNER = 4,
    };

    if (service == NULL || service->connection != NULL)
        return false;

    GError *error = NULL;
    service->connection = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &error);
    if (service->connection == NULL) {
        g_warning("Failed to connect to the session bus: %s",
                  error ? error->message : "unknown error");
        g_clear_error(&error);
        return false;
    }

    /* Acquire synchronously with DO_NOT_QUEUE. g_bus_own_name() reports only
     * whether callbacks were scheduled, allowing duplicate daemons to keep
     * running until its later name-lost callback. This bounded startup call
     * makes singleton ownership a prerequisite for any Bluetooth work. */
    GVariant *reply = g_dbus_connection_call_sync(
        service->connection,
        "org.freedesktop.DBus",
        "/org/freedesktop/DBus",
        "org.freedesktop.DBus",
        "RequestName",
        g_variant_new("(su)", DBUS_SERVICE_NAME,
                      DBUS_NAME_FLAG_DO_NOT_QUEUE),
        G_VARIANT_TYPE("(u)"),
        G_DBUS_CALL_FLAGS_NONE,
        5000,
        NULL,
        &error);

    guint32 request_result = 0;
    if (reply != NULL) {
        g_variant_get(reply, "(u)", &request_result);
        g_variant_unref(reply);
    }

    if (error != NULL ||
        (request_result != DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER &&
         request_result != DBUS_REQUEST_NAME_REPLY_ALREADY_OWNER)) {
        if (error != NULL) {
            g_warning("Failed to acquire D-Bus name: %s", error->message);
        } else {
            g_warning("Another EarPort daemon already owns %s",
                      DBUS_SERVICE_NAME);
        }
        g_clear_error(&error);
        g_clear_object(&service->connection);
        return false;
    }
    service->owns_name = true;

    service->registration_id = g_dbus_connection_register_object(
        service->connection,
        DBUS_OBJECT_PATH,
        service->introspection_data->interfaces[0],
        &interface_vtable,
        service,
        NULL,
        &error);
    if (service->registration_id == 0) {
        g_warning("Failed to register D-Bus object: %s",
                  error ? error->message : "unknown error");
        g_clear_error(&error);
        release_bus_name(service);
        g_clear_object(&service->connection);
        return false;
    }

    /* A restarted session bus invalidates both the name and object. Exiting
     * lets the user service manager restart the daemon into a clean state. */
    g_dbus_connection_set_exit_on_close(service->connection, TRUE);
    g_message("D-Bus name acquired: %s", DBUS_SERVICE_NAME);
    g_message("D-Bus object registered at %s", DBUS_OBJECT_PATH);
    return true;
}

void dbus_service_stop(DbusService *service)
{
    if (service->registration_id > 0 && service->connection) {
        g_dbus_connection_unregister_object(service->connection, service->registration_id);
        service->registration_id = 0;
    }

    release_bus_name(service);

    if (service->connection) {
        g_object_unref(service->connection);
        service->connection = NULL;
    }
}

void dbus_service_set_noise_control_callback(DbusService *service,
                                              DbusNoiseControlCallback callback,
                                              void *user_data)
{
    service->noise_control_callback = callback;
    service->noise_control_user_data = user_data;
}

void dbus_service_set_conv_awareness_callback(DbusService *service,
                                               DbusConvAwarenessCallback callback,
                                               void *user_data)
{
    service->conv_awareness_callback = callback;
    service->conv_awareness_user_data = user_data;
}

void dbus_service_set_adaptive_level_callback(DbusService *service,
                                               DbusAdaptiveLevelCallback callback,
                                               void *user_data)
{
    service->adaptive_level_callback = callback;
    service->adaptive_level_user_data = user_data;
}

void dbus_service_set_ear_pause_mode_callback(DbusService *service,
                                               DbusEarPauseModeCallback callback,
                                               void *user_data)
{
    service->ear_pause_mode_callback = callback;
    service->ear_pause_mode_user_data = user_data;
}

void dbus_service_set_listening_modes_callback(DbusService *service,
                                                DbusListeningModesCallback callback,
                                                void *user_data)
{
    service->listening_modes_callback = callback;
    service->listening_modes_user_data = user_data;
}

void dbus_service_set_display_name_callback(DbusService *service,
                                             DbusDisplayNameCallback callback,
                                             void *user_data)
{
    service->display_name_callback = callback;
    service->display_name_user_data = user_data;
}

static void emit_signal(DbusService *service,
                         const char *signal_name,
                         GVariant *parameters)
{
    if (service->connection == NULL)
        return;

    GError *error = NULL;
    g_dbus_connection_emit_signal(
        service->connection,
        NULL,  /* Broadcast to all listeners */
        DBUS_OBJECT_PATH,
        DBUS_INTERFACE_NAME,
        signal_name,
        parameters,
        &error
    );

    if (error) {
        g_warning("Failed to emit signal %s: %s", signal_name, error->message);
        g_error_free(error);
    }
}

void dbus_service_emit_device_connected(DbusService *service,
                                         const char *address,
                                         const char *name)
{
    emit_signal(service, "DeviceConnected",
                g_variant_new("(ss)", address ? address : "", name ? name : ""));
}

void dbus_service_emit_device_disconnected(DbusService *service,
                                            const char *address,
                                            const char *name)
{
    emit_signal(service, "DeviceDisconnected",
                g_variant_new("(ss)", address ? address : "", name ? name : ""));
}

void dbus_service_emit_battery_changed(DbusService *service,
                                        int left, int right, int case_battery)
{
    emit_signal(service, "BatteryChanged",
                g_variant_new("(iii)", left, right, case_battery));
}

void dbus_service_emit_noise_control_changed(DbusService *service,
                                              NoiseControlMode mode)
{
    emit_signal(service, "NoiseControlModeChanged",
                g_variant_new("(s)", noise_control_mode_to_string(mode)));
}

void dbus_service_emit_ear_detection_changed(DbusService *service,
                                              bool left_in_ear,
                                              bool right_in_ear)
{
    emit_signal(service, "EarDetectionChanged",
                g_variant_new("(bb)", left_in_ear, right_in_ear));
}

void dbus_service_emit_properties_changed_many(
    DbusService *service,
    const char *const *property_names,
    gsize property_count)
{
    if (service->connection == NULL)
        return;

    GVariantBuilder builder;
    g_variant_builder_init(&builder, G_VARIANT_TYPE("a{sv}"));

    for (gsize i = 0; i < property_count; i++) {
        GVariant *prop_value = get_property(service->connection, NULL,
                                             DBUS_OBJECT_PATH,
                                             DBUS_INTERFACE_NAME,
                                             property_names[i], NULL, service);
        if (prop_value != NULL) {
            g_variant_builder_add(&builder, "{sv}", property_names[i],
                                  prop_value);
        }
    }

    GError *error = NULL;
    g_dbus_connection_emit_signal(
        service->connection,
        NULL,
        DBUS_OBJECT_PATH,
        "org.freedesktop.DBus.Properties",
        "PropertiesChanged",
        g_variant_new("(s@a{sv}@as)",
                      DBUS_INTERFACE_NAME,
                      g_variant_builder_end(&builder),
                      g_variant_new_strv(NULL, 0)),
        &error
    );

    if (error) {
        g_warning("Failed to emit PropertiesChanged: %s", error->message);
        g_error_free(error);
    }
}

void dbus_service_emit_properties_changed(DbusService *service,
                                           const char *property_name)
{
    dbus_service_emit_properties_changed_many(service, &property_name, 1);
}

void dbus_service_emit_all_properties_changed(DbusService *service)
{
    static const char *const property_names[] = {
        "Connected",
        "DeviceName",
        "DeviceAddress",
        "DeviceModel",
        "DisplayName",
        "IsHeadphones",
        "SupportsANC",
        "SupportsAdaptive",
        "BatteryLeft",
        "BatteryRight",
        "BatteryCase",
        "ChargingLeft",
        "ChargingRight",
        "ChargingCase",
        "NoiseControlMode",
        "ConversationalAwareness",
        "LeftInEar",
        "RightInEar",
        "AdaptiveNoiseLevel",
        "EarPauseMode",
        "ListeningModeOff",
        "ListeningModeTransparency",
        "ListeningModeANC",
        "ListeningModeAdaptive",
    };

    dbus_service_emit_properties_changed_many(
        service, property_names, G_N_ELEMENTS(property_names));
}
