/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2024 EarPort Contributors
 */

#include "bluez_monitor.h"
#include "bluetooth.h"
#include "ble_autoconnect.h"

#include <stdio.h>
#include <string.h>

#define APPLE_COMPANY_ID 0x004c
#define AUTO_CONNECT_MIN_RSSI_DBM (-70)
#define AUTO_CONNECT_CONFIRM_WINDOW_USEC (2 * G_USEC_PER_SEC)
#define AUTO_CONNECT_COOLDOWN_USEC (30 * G_USEC_PER_SEC)
#define AUTO_CONNECT_RSSI_MAX_AGE_USEC (5 * G_USEC_PER_SEC)
#define BLUEZ_CALL_TIMEOUT_MSEC 5000

typedef struct {
    gint value;
    gint64 observed_usec;
} AdvertisementRssi;

struct BluezMonitor {
    GDBusConnection *connection;
    guint properties_signal_id;
    guint interfaces_added_id;
    guint interfaces_removed_id;

    BluezDeviceCallback connected_callback;
    void *connected_user_data;

    BluezDeviceCallback disconnected_callback;
    void *disconnected_user_data;

    /* Track known devices */
    GHashTable *known_devices;  /* path -> BluezDeviceInfo */
    GHashTable *paired_devices; /* path -> BluezDeviceInfo */

    bool auto_connect_on_wear;
    GHashTable *auto_connect_states; /* advertising path -> state */
    GHashTable *auto_connect_model_attempts; /* BLE model -> gint64 */
    GHashTable *connect_in_flight;   /* paired Device1 path set */
    GHashTable *advertisement_rssi; /* advertising path -> AdvertisementRssi */
    GPtrArray *discovery_adapters;  /* adapter paths whose session we own */
    guint discovery_retry_id;
};

static void start_le_discovery(BluezMonitor *monitor);

static void forget_discovery_adapter(BluezMonitor *monitor,
                                     const char *object_path)
{
    for (guint i = 0; i < monitor->discovery_adapters->len; i++) {
        if (g_strcmp0(g_ptr_array_index(monitor->discovery_adapters, i),
                      object_path) == 0) {
            g_ptr_array_remove_index(monitor->discovery_adapters, i);
            return;
        }
    }
}

void bluez_device_info_free(BluezDeviceInfo *info)
{
    if (info == NULL)
        return;
    g_free(info->address);
    g_free(info->name);
    g_free(info->object_path);
    g_free(info);
}

BluezDeviceInfo *bluez_device_info_copy(const BluezDeviceInfo *info)
{
    if (info == NULL)
        return NULL;

    BluezDeviceInfo *copy = g_new0(BluezDeviceInfo, 1);
    copy->address = g_strdup(info->address);
    copy->name = g_strdup(info->name);
    copy->object_path = g_strdup(info->object_path);
    copy->connected = info->connected;
    copy->paired = info->paired;
    copy->vendor_id = info->vendor_id;
    copy->product_id = info->product_id;
    return copy;
}

static bool parse_bluez_modalias(const char *modalias,
                                 uint16_t *vendor_id,
                                 uint16_t *product_id)
{
    unsigned int vendor = 0;
    unsigned int product = 0;

    if (modalias == NULL ||
        sscanf(modalias, "bluetooth:v%4xp%4x", &vendor, &product) != 2 ||
        vendor > UINT16_MAX || product > UINT16_MAX) {
        return false;
    }

    if (vendor_id != NULL)
        *vendor_id = (uint16_t)vendor;
    if (product_id != NULL)
        *product_id = (uint16_t)product;
    return true;
}

static bool device_is_airpods(GDBusConnection *connection, const char *object_path)
{
    GError *error = NULL;

    GVariant *result = g_dbus_connection_call_sync(
        connection,
        BLUEZ_SERVICE,
        object_path,
        DBUS_PROPERTIES_INTERFACE,
        "Get",
        g_variant_new("(ss)", BLUEZ_DEVICE_INTERFACE, "UUIDs"),
        G_VARIANT_TYPE("(v)"),
        G_DBUS_CALL_FLAGS_NONE,
        BLUEZ_CALL_TIMEOUT_MSEC,
        NULL,
        &error
    );

    if (error) {
        g_error_free(error);
        return false;
    }

    GVariant *variant = NULL;
    g_variant_get(result, "(v)", &variant);

    bool is_airpods = false;

    if (g_variant_is_of_type(variant, G_VARIANT_TYPE_STRING_ARRAY)) {
        gsize n_uuids = 0;
        const gchar **uuids = g_variant_get_strv(variant, &n_uuids);

        for (gsize i = 0; i < n_uuids; i++) {
            if (g_ascii_strcasecmp(uuids[i], AIRPODS_UUID) == 0) {
                is_airpods = true;
                break;
            }
        }
        g_free(uuids);
    }

    g_variant_unref(variant);
    g_variant_unref(result);

    return is_airpods;
}

static BluezDeviceInfo *get_device_info(GDBusConnection *connection, const char *object_path)
{
    GError *error = NULL;

    GVariant *result = g_dbus_connection_call_sync(
        connection,
        BLUEZ_SERVICE,
        object_path,
        DBUS_PROPERTIES_INTERFACE,
        "GetAll",
        g_variant_new("(s)", BLUEZ_DEVICE_INTERFACE),
        G_VARIANT_TYPE("(a{sv})"),
        G_DBUS_CALL_FLAGS_NONE,
        BLUEZ_CALL_TIMEOUT_MSEC,
        NULL,
        &error
    );

    if (error) {
        g_warning("Failed to get device properties: %s", error->message);
        g_error_free(error);
        return NULL;
    }

    BluezDeviceInfo *info = g_new0(BluezDeviceInfo, 1);
    info->object_path = g_strdup(object_path);

    GVariant *props = NULL;
    g_variant_get(result, "(@a{sv})", &props);

    GVariant *value;

    value = g_variant_lookup_value(props, "Address", G_VARIANT_TYPE_STRING);
    if (value) {
        info->address = g_variant_dup_string(value, NULL);
        g_variant_unref(value);
    }

    value = g_variant_lookup_value(props, "Name", G_VARIANT_TYPE_STRING);
    if (value) {
        info->name = g_variant_dup_string(value, NULL);
        g_variant_unref(value);
    }

    value = g_variant_lookup_value(props, "Connected", G_VARIANT_TYPE_BOOLEAN);
    if (value) {
        info->connected = g_variant_get_boolean(value);
        g_variant_unref(value);
    }

    value = g_variant_lookup_value(props, "Paired", G_VARIANT_TYPE_BOOLEAN);
    if (value) {
        info->paired = g_variant_get_boolean(value);
        g_variant_unref(value);
    }

    value = g_variant_lookup_value(props, "Modalias", G_VARIANT_TYPE_STRING);
    if (value) {
        const char *modalias = g_variant_get_string(value, NULL);
        parse_bluez_modalias(modalias, &info->vendor_id, &info->product_id);
        g_variant_unref(value);
    }

    g_variant_unref(props);
    g_variant_unref(result);

    return info;
}

static void cache_rssi(BluezMonitor *monitor,
                       const char *object_path,
                       GVariant *device_properties)
{
    GVariant *value = g_variant_lookup_value(device_properties, "RSSI",
                                             G_VARIANT_TYPE_INT16);
    if (value == NULL)
        return;

    AdvertisementRssi *rssi = g_new(AdvertisementRssi, 1);
    rssi->value = g_variant_get_int16(value);
    rssi->observed_usec = g_get_monotonic_time();
    g_hash_table_replace(monitor->advertisement_rssi,
                         g_strdup(object_path), rssi);
    g_variant_unref(value);
}

static void cache_paired_device(BluezMonitor *monitor,
                                const char *object_path,
                                const BluezDeviceInfo *info)
{
    if (info != NULL && info->paired && info->vendor_id == APPLE_COMPANY_ID &&
        device_is_airpods(monitor->connection, object_path)) {
        g_hash_table_replace(monitor->paired_devices,
                             g_strdup(object_path),
                             bluez_device_info_copy(info));
    } else {
        g_hash_table_remove(monitor->paired_devices, object_path);
    }
}

typedef struct {
    GHashTable *in_flight;
    GHashTable *auto_connect_states;
    char *object_path;
    char *advertisement_path;
    char *device_name;
} AutoConnectRequest;

static void auto_connect_request_free(AutoConnectRequest *request)
{
    g_hash_table_unref(request->in_flight);
    g_hash_table_unref(request->auto_connect_states);
    g_free(request->object_path);
    g_free(request->advertisement_path);
    g_free(request->device_name);
    g_free(request);
}

static void on_auto_connect_finished(GObject *source_object,
                                     GAsyncResult *result,
                                     gpointer user_data)
{
    AutoConnectRequest *request = user_data;
    GError *error = NULL;
    GVariant *reply = g_dbus_connection_call_finish(
        G_DBUS_CONNECTION(source_object), result, &error);

    g_hash_table_remove(request->in_flight, request->object_path);

    if (error != NULL) {
        BleAutoConnectState *state = g_hash_table_lookup(
            request->auto_connect_states, request->advertisement_path);
        if (state != NULL) {
            /* The confirmed worn sequence was consumed when Connect was
             * issued. Rearm it so a still-worn device can form a fresh
             * two-advertisement sequence after the existing cooldown. Keep
             * has_attempted/last_attempt_usec intact to enforce that delay. */
            state->worn_sequence_active = false;
            state->worn_observations = 0;
            state->first_worn_observation_usec = 0;
            state->sequence_consumed = false;
        }
        g_warning("Auto-connect failed for %s: %s",
                  request->device_name ? request->device_name : "AirPods",
                  error->message);
        g_error_free(error);
    } else {
        g_message("Auto-connect request accepted for %s",
                  request->device_name ? request->device_name : "AirPods");
    }

    if (reply != NULL)
        g_variant_unref(reply);
    auto_connect_request_free(request);
}

static void request_device_connect(BluezMonitor *monitor,
                                   const BluezDeviceInfo *device,
                                   const char *advertisement_path)
{
    if (g_hash_table_contains(monitor->connect_in_flight,
                              device->object_path)) {
        return;
    }

    g_hash_table_add(monitor->connect_in_flight,
                     g_strdup(device->object_path));

    AutoConnectRequest *request = g_new0(AutoConnectRequest, 1);
    request->in_flight = g_hash_table_ref(monitor->connect_in_flight);
    request->auto_connect_states = g_hash_table_ref(
        monitor->auto_connect_states);
    request->object_path = g_strdup(device->object_path);
    request->advertisement_path = g_strdup(advertisement_path);
    request->device_name = g_strdup(device->name);

    g_message("Confirmed nearby wear state; connecting paired %s",
              device->name ? device->name : "AirPods");

    g_dbus_connection_call(
        monitor->connection,
        BLUEZ_SERVICE,
        device->object_path,
        BLUEZ_DEVICE_INTERFACE,
        "Connect",
        NULL,
        NULL,
        G_DBUS_CALL_FLAGS_NONE,
        30000,
        NULL,
        on_auto_connect_finished,
        request
    );
}

static BluezDeviceInfo *find_unique_paired_target(BluezMonitor *monitor,
                                                   uint16_t ble_model,
                                                   unsigned int *match_count)
{
    uint16_t expected_product = ble_airpods_bluez_product_id(ble_model);
    BluezDeviceInfo *target = NULL;
    unsigned int count = 0;
    GHashTableIter iter;
    gpointer value = NULL;

    g_hash_table_iter_init(&iter, monitor->paired_devices);
    while (g_hash_table_iter_next(&iter, NULL, &value)) {
        const BluezDeviceInfo *info = value;
        if (!info->paired || info->connected ||
            info->vendor_id != APPLE_COMPANY_ID ||
            info->product_id != expected_product ||
            g_hash_table_contains(monitor->connect_in_flight,
                                  info->object_path)) {
            continue;
        }

        count++;
        if (target == NULL)
            target = bluez_device_info_copy(info);
    }

    if (match_count != NULL)
        *match_count = count;

    if (count != 1) {
        bluez_device_info_free(target);
        return NULL;
    }
    return target;
}

static void process_apple_manufacturer_data(BluezMonitor *monitor,
                                            const char *object_path,
                                            GVariant *manufacturer_data)
{
    if (!monitor->auto_connect_on_wear || manufacturer_data == NULL)
        return;

    GVariantIter iter;
    guint16 company_id = 0;
    GVariant *boxed = NULL;
    g_variant_iter_init(&iter, manufacturer_data);

    while (g_variant_iter_next(&iter, "{q@v}", &company_id, &boxed)) {
        GVariant *bytes_variant = g_variant_get_variant(boxed);
        g_variant_unref(boxed);
        boxed = NULL;

        if (company_id != APPLE_COMPANY_ID ||
            !g_variant_is_of_type(bytes_variant, G_VARIANT_TYPE_BYTESTRING)) {
            g_variant_unref(bytes_variant);
            continue;
        }

        gsize len = 0;
        const guint8 *bytes = g_variant_get_fixed_array(bytes_variant, &len,
                                                        sizeof(guint8));
        BleAirPodsAdvertisement advertisement;
        bool parsed = ble_airpods_parse_manufacturer_data(
            bytes, len, &advertisement);
        g_variant_unref(bytes_variant);
        if (!parsed)
            continue;

        /* Only a strong, nearby advertisement may confirm a positive wear
         * transition. Negative observations are always useful for rearming. */
        gint64 now_usec = g_get_monotonic_time();
        AdvertisementRssi *rssi = g_hash_table_lookup(
            monitor->advertisement_rssi, object_path);
        if (advertisement.worn &&
            (rssi == NULL || rssi->value < AUTO_CONNECT_MIN_RSSI_DBM ||
             now_usec < rssi->observed_usec ||
             now_usec - rssi->observed_usec >
                 AUTO_CONNECT_RSSI_MAX_AGE_USEC)) {
            g_debug("Ignoring worn AirPods advertisement without strong RSSI");
            return;
        }

        gpointer model_key = GUINT_TO_POINTER((guint)advertisement.model);
        gint64 *last_model_attempt = g_hash_table_lookup(
            monitor->auto_connect_model_attempts, model_key);
        if (advertisement.worn && last_model_attempt != NULL &&
            now_usec >= *last_model_attempt &&
            now_usec - *last_model_attempt < AUTO_CONNECT_COOLDOWN_USEC) {
            return;
        }

        BleAutoConnectState *state = g_hash_table_lookup(
            monitor->auto_connect_states, object_path);
        if (state == NULL) {
            state = g_new0(BleAutoConnectState, 1);
            g_hash_table_insert(monitor->auto_connect_states,
                                g_strdup(object_path), state);
        }

        BleAutoConnectState state_before_observation = *state;
        if (!ble_autoconnect_observe(state,
                                     advertisement.worn,
                                     now_usec,
                                     AUTO_CONNECT_CONFIRM_WINDOW_USEC,
                                     AUTO_CONNECT_COOLDOWN_USEC)) {
            return;
        }

        unsigned int matches = 0;
        BluezDeviceInfo *target = find_unique_paired_target(
            monitor, advertisement.model, &matches);
        if (target == NULL) {
            /* Confirmation must not be consumed before a stable target is
             * available. The next advertisement may arrive after BlueZ has
             * finished populating Paired/Modalias properties. */
            *state = state_before_observation;
            if (matches > 1) {
                g_warning("Refusing wear auto-connect: %u paired AirPods share BLE model 0x%04x",
                          matches, advertisement.model);
            } else {
                g_debug("No disconnected paired AirPods uniquely match BLE model 0x%04x",
                        advertisement.model);
            }
            return;
        }

        gint64 *attempt_time = g_new(gint64, 1);
        *attempt_time = now_usec;
        g_hash_table_replace(monitor->auto_connect_model_attempts,
                             model_key, attempt_time);
        request_device_connect(monitor, target, object_path);
        bluez_device_info_free(target);
        return;
    }
}

static void on_properties_changed(GDBusConnection *connection,
                                   const gchar *sender_name G_GNUC_UNUSED,
                                   const gchar *object_path,
                                   const gchar *interface_name G_GNUC_UNUSED,
                                   const gchar *signal_name G_GNUC_UNUSED,
                                   GVariant *parameters,
                                   gpointer user_data)
{
    BluezMonitor *monitor = user_data;

    const gchar *iface = NULL;
    GVariant *changed_props = NULL;

    g_variant_get(parameters, "(&s@a{sv}as)", &iface, &changed_props, NULL);

    if (g_strcmp0(iface, BLUEZ_ADAPTER_INTERFACE) == 0) {
        GVariant *powered = g_variant_lookup_value(
            changed_props, "Powered", G_VARIANT_TYPE_BOOLEAN);
        if (powered != NULL && g_variant_get_boolean(powered)) {
            /* A prior discovery session is lost across adapter power cycles. */
            forget_discovery_adapter(monitor, object_path);
            start_le_discovery(monitor);
        }
        if (powered != NULL)
            g_variant_unref(powered);
        g_variant_unref(changed_props);
        return;
    }

    /* Only care about Device1 interface from here. */
    if (g_strcmp0(iface, BLUEZ_DEVICE_INTERFACE) != 0) {
        g_variant_unref(changed_props);
        return;
    }

    cache_rssi(monitor, object_path, changed_props);

    GVariant *manufacturer_data = g_variant_lookup_value(
        changed_props, "ManufacturerData", G_VARIANT_TYPE("a{qv}"));
    if (manufacturer_data != NULL) {
        process_apple_manufacturer_data(monitor, object_path,
                                        manufacturer_data);
        g_variant_unref(manufacturer_data);
    }

    GVariant *connected_var = g_variant_lookup_value(
        changed_props, "Connected", G_VARIANT_TYPE_BOOLEAN);
    GVariant *paired_var = g_variant_lookup_value(changed_props, "Paired", NULL);
    GVariant *modalias_var = g_variant_lookup_value(changed_props, "Modalias", NULL);
    bool identity_changed = connected_var != NULL || paired_var != NULL ||
                            modalias_var != NULL;
    if (paired_var != NULL)
        g_variant_unref(paired_var);
    if (modalias_var != NULL)
        g_variant_unref(modalias_var);
    g_variant_unref(changed_props);
    if (!identity_changed) {
        if (connected_var != NULL)
            g_variant_unref(connected_var);
        return;
    }

    BluezDeviceInfo *info = get_device_info(connection, object_path);
    if (info == NULL) {
        if (connected_var != NULL)
            g_variant_unref(connected_var);
        return;
    }

    bool is_airpods = g_hash_table_contains(monitor->paired_devices,
                                             object_path) ||
                      device_is_airpods(connection, object_path);
    if (!is_airpods) {
        if (connected_var != NULL)
            g_variant_unref(connected_var);
        bluez_device_info_free(info);
        return;
    }

    cache_paired_device(monitor, object_path, info);
    if (connected_var == NULL) {
        bluez_device_info_free(info);
        return;
    }

    bool connected = g_variant_get_boolean(connected_var);
    g_variant_unref(connected_var);

    g_message("AirPods %s: %s (%s)",
              connected ? "connected" : "disconnected",
              info->name ? info->name : "Unknown",
              info->address ? info->address : "Unknown");

    if (connected) {
        /* Store in known devices */
        g_hash_table_insert(monitor->known_devices,
                            g_strdup(object_path),
                            bluez_device_info_copy(info));

        if (monitor->connected_callback) {
            monitor->connected_callback(info, monitor->connected_user_data);
        }
    } else {
        /* Remove from known devices */
        g_hash_table_remove(monitor->known_devices, object_path);

        if (monitor->disconnected_callback) {
            monitor->disconnected_callback(info, monitor->disconnected_user_data);
        }
    }

    bluez_device_info_free(info);
}

static void on_interfaces_added(GDBusConnection *connection,
                                 const gchar *sender_name G_GNUC_UNUSED,
                                 const gchar *object_path G_GNUC_UNUSED,
                                 const gchar *interface_name G_GNUC_UNUSED,
                                 const gchar *signal_name G_GNUC_UNUSED,
                                 GVariant *parameters,
                                 gpointer user_data)
{
    /* New device appeared - check if it's connected AirPods */
    BluezMonitor *monitor = user_data;

    const gchar *obj_path = NULL;
    GVariant *interfaces = NULL;

    g_variant_get(parameters, "(&o@a{sa{sv}})", &obj_path, &interfaces);

    GVariant *adapter_properties = g_variant_lookup_value(
        interfaces, BLUEZ_ADAPTER_INTERFACE, G_VARIANT_TYPE("a{sv}"));
    if (adapter_properties != NULL) {
        GVariant *powered = g_variant_lookup_value(
            adapter_properties, "Powered", G_VARIANT_TYPE_BOOLEAN);
        if (powered == NULL || g_variant_get_boolean(powered)) {
            forget_discovery_adapter(monitor, obj_path);
            start_le_discovery(monitor);
        }
        if (powered != NULL)
            g_variant_unref(powered);
        g_variant_unref(adapter_properties);
        g_variant_unref(interfaces);
        return;
    }

    GVariant *device_properties = g_variant_lookup_value(
        interfaces, BLUEZ_DEVICE_INTERFACE, G_VARIANT_TYPE("a{sv}"));
    if (device_properties == NULL) {
        g_variant_unref(interfaces);
        return;
    }

    cache_rssi(monitor, obj_path, device_properties);

    /* InterfacesAdded carries the first advertisement properties, so handle
     * ManufacturerData here as well as in subsequent PropertiesChanged. */
    GVariant *manufacturer_data = g_variant_lookup_value(
        device_properties, "ManufacturerData", G_VARIANT_TYPE("a{qv}"));
    if (manufacturer_data != NULL) {
        process_apple_manufacturer_data(monitor, obj_path, manufacturer_data);
        g_variant_unref(manufacturer_data);
    }

    /* Transient BLE advertisers are neither paired nor connected. Avoid a
     * synchronous GetAll/UUID round trip for every nearby Device1 object. */
    GVariant *paired_value = g_variant_lookup_value(
        device_properties, "Paired", G_VARIANT_TYPE_BOOLEAN);
    GVariant *connected_value = g_variant_lookup_value(
        device_properties, "Connected", G_VARIANT_TYPE_BOOLEAN);
    bool paired = paired_value != NULL && g_variant_get_boolean(paired_value);
    bool connected = connected_value != NULL &&
                     g_variant_get_boolean(connected_value);
    if (paired_value != NULL)
        g_variant_unref(paired_value);
    if (connected_value != NULL)
        g_variant_unref(connected_value);

    BluezDeviceInfo *info = NULL;
    if (paired || connected) {
        info = get_device_info(connection, obj_path);
        if (info != NULL && device_is_airpods(connection, obj_path)) {
            cache_paired_device(monitor, obj_path, info);
            if (info->connected) {
                g_message("New connected AirPods discovered: %s", info->name);

                g_hash_table_insert(monitor->known_devices,
                                    g_strdup(obj_path),
                                    bluez_device_info_copy(info));

                if (monitor->connected_callback) {
                    monitor->connected_callback(info,
                                                monitor->connected_user_data);
                }
            }
        }
    }

    bluez_device_info_free(info);
    g_variant_unref(device_properties);
    g_variant_unref(interfaces);
}

static void on_interfaces_removed(GDBusConnection *connection G_GNUC_UNUSED,
                                   const gchar *sender_name G_GNUC_UNUSED,
                                   const gchar *object_path G_GNUC_UNUSED,
                                   const gchar *interface_name G_GNUC_UNUSED,
                                   const gchar *signal_name G_GNUC_UNUSED,
                                   GVariant *parameters,
                                   gpointer user_data)
{
    BluezMonitor *monitor = user_data;

    const gchar *obj_path = NULL;
    g_variant_get(parameters, "(&oas)", &obj_path, NULL);

    /* Check if we were tracking this device */
    BluezDeviceInfo *info = g_hash_table_lookup(monitor->known_devices, obj_path);
    if (info) {
        g_message("AirPods device removed: %s", info->name);

        if (monitor->disconnected_callback) {
            monitor->disconnected_callback(info, monitor->disconnected_user_data);
        }

        g_hash_table_remove(monitor->known_devices, obj_path);
    }

    g_hash_table_remove(monitor->paired_devices, obj_path);
    g_hash_table_remove(monitor->connect_in_flight, obj_path);
    g_hash_table_remove(monitor->advertisement_rssi, obj_path);
    g_hash_table_remove(monitor->auto_connect_states, obj_path);
    forget_discovery_adapter(monitor, obj_path);
}

static bool discovery_adapter_is_owned(BluezMonitor *monitor,
                                       const char *object_path)
{
    for (guint i = 0; i < monitor->discovery_adapters->len; i++) {
        if (g_strcmp0(g_ptr_array_index(monitor->discovery_adapters, i),
                      object_path) == 0) {
            return true;
        }
    }
    return false;
}

static gboolean retry_le_discovery(gpointer user_data)
{
    BluezMonitor *monitor = user_data;
    monitor->discovery_retry_id = 0;
    start_le_discovery(monitor);
    return G_SOURCE_REMOVE;
}

static void schedule_discovery_retry(BluezMonitor *monitor)
{
    if (monitor->auto_connect_on_wear &&
        monitor->discovery_adapters->len == 0 &&
        monitor->discovery_retry_id == 0) {
        monitor->discovery_retry_id = g_timeout_add_seconds(
            5, retry_le_discovery, monitor);
    }
}

static void cancel_discovery_retry(BluezMonitor *monitor)
{
    if (monitor->discovery_retry_id != 0) {
        g_source_remove(monitor->discovery_retry_id);
        monitor->discovery_retry_id = 0;
    }
}

static void start_le_discovery(BluezMonitor *monitor)
{
    if (!monitor->auto_connect_on_wear)
        return;

    GError *error = NULL;
    GVariant *result = g_dbus_connection_call_sync(
        monitor->connection,
        BLUEZ_SERVICE,
        "/",
        DBUS_OBJECT_MANAGER_INTERFACE,
        "GetManagedObjects",
        NULL,
        G_VARIANT_TYPE("(a{oa{sa{sv}}})"),
        G_DBUS_CALL_FLAGS_NONE,
        5000,
        NULL,
        &error
    );
    if (error != NULL) {
        g_warning("Cannot enumerate Bluetooth adapters for wear auto-connect: %s",
                  error->message);
        g_error_free(error);
        schedule_discovery_retry(monitor);
        return;
    }

    GVariant *objects = NULL;
    g_variant_get(result, "(@a{oa{sa{sv}}})", &objects);
    GVariantIter iter;
    const gchar *object_path = NULL;
    GVariant *interfaces = NULL;
    bool found_adapter = false;
    bool found_powered_adapter = false;
    g_variant_iter_init(&iter, objects);

    while (g_variant_iter_next(&iter, "{&o@a{sa{sv}}}",
                               &object_path, &interfaces)) {
        GVariant *adapter_properties = g_variant_lookup_value(
            interfaces, BLUEZ_ADAPTER_INTERFACE, G_VARIANT_TYPE("a{sv}"));
        if (adapter_properties == NULL) {
            g_variant_unref(interfaces);
            continue;
        }

        found_adapter = true;
        GVariant *powered_value = g_variant_lookup_value(
            adapter_properties, "Powered", G_VARIANT_TYPE_BOOLEAN);
        bool powered = powered_value == NULL ||
                       g_variant_get_boolean(powered_value);
        if (powered_value != NULL)
            g_variant_unref(powered_value);
        g_variant_unref(adapter_properties);

        if (!powered) {
            /* PropertiesChanged will restart discovery when this adapter is
             * powered on. Do not call StartDiscovery (or warn) every five
             * seconds while Bluetooth is deliberately disabled. */
            g_variant_unref(interfaces);
            continue;
        }

        found_powered_adapter = true;
        if (discovery_adapter_is_owned(monitor, object_path)) {
            g_variant_unref(interfaces);
            continue;
        }

        GVariantBuilder filter;
        g_variant_builder_init(&filter, G_VARIANT_TYPE("a{sv}"));
        g_variant_builder_add(&filter, "{sv}", "Transport",
                              g_variant_new_string("le"));
        g_variant_builder_add(&filter, "{sv}", "DuplicateData",
                              g_variant_new_boolean(TRUE));
        g_variant_builder_add(&filter, "{sv}", "RSSI",
                              g_variant_new_int16(AUTO_CONNECT_MIN_RSSI_DBM));

        GError *filter_error = NULL;
        GVariant *filter_reply = g_dbus_connection_call_sync(
            monitor->connection,
            BLUEZ_SERVICE,
            object_path,
            BLUEZ_ADAPTER_INTERFACE,
            "SetDiscoveryFilter",
            g_variant_new("(a{sv})", &filter),
            G_VARIANT_TYPE("()"),
            G_DBUS_CALL_FLAGS_NONE,
            5000,
            NULL,
            &filter_error
        );
        if (filter_error != NULL) {
            g_warning("Could not set LE discovery filter on %s: %s",
                      object_path, filter_error->message);
            g_error_free(filter_error);
            g_variant_unref(interfaces);
            continue;
        }
        g_variant_unref(filter_reply);

        GError *start_error = NULL;
        GVariant *start_reply = g_dbus_connection_call_sync(
            monitor->connection,
            BLUEZ_SERVICE,
            object_path,
            BLUEZ_ADAPTER_INTERFACE,
            "StartDiscovery",
            NULL,
            G_VARIANT_TYPE("()"),
            G_DBUS_CALL_FLAGS_NONE,
            5000,
            NULL,
            &start_error
        );
        if (start_error != NULL) {
            gchar *remote_error = g_dbus_error_get_remote_error(start_error);
            if (g_strcmp0(remote_error, "org.bluez.Error.InProgress") == 0) {
                g_ptr_array_add(monitor->discovery_adapters,
                                g_strdup(object_path));
                g_debug("LE discovery session is already active");
            } else {
                g_warning("Could not start LE discovery on %s: %s",
                          object_path, start_error->message);
            }
            g_free(remote_error);
            g_error_free(start_error);
        } else {
            g_variant_unref(start_reply);
            g_ptr_array_add(monitor->discovery_adapters,
                            g_strdup(object_path));
            g_message("Continuous LE discovery started for wear auto-connect");
        }

        g_variant_unref(interfaces);
    }

    g_variant_unref(objects);
    g_variant_unref(result);

    if (monitor->discovery_adapters->len > 0 ||
        (found_adapter && !found_powered_adapter))
        cancel_discovery_retry(monitor);
    else
        schedule_discovery_retry(monitor);
}

static void stop_le_discovery(BluezMonitor *monitor)
{
    for (guint i = 0; i < monitor->discovery_adapters->len; i++) {
        const char *object_path = g_ptr_array_index(
            monitor->discovery_adapters, i);
        GError *error = NULL;
        GVariant *reply = g_dbus_connection_call_sync(
            monitor->connection,
            BLUEZ_SERVICE,
            object_path,
            BLUEZ_ADAPTER_INTERFACE,
            "StopDiscovery",
            NULL,
            G_VARIANT_TYPE("()"),
            G_DBUS_CALL_FLAGS_NONE,
            5000,
            NULL,
            &error
        );
        if (error != NULL) {
            g_debug("Could not stop owned LE discovery on %s: %s",
                    object_path, error->message);
            g_error_free(error);
        } else {
            g_variant_unref(reply);
        }
    }
    g_ptr_array_set_size(monitor->discovery_adapters, 0);
}

BluezMonitor *bluez_monitor_new(void)
{
    GError *error = NULL;

    GDBusConnection *connection = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, &error);
    if (error) {
        g_warning("Failed to connect to system bus: %s", error->message);
        g_error_free(error);
        return NULL;
    }

    BluezMonitor *monitor = g_new0(BluezMonitor, 1);
    monitor->connection = connection;
    monitor->known_devices = g_hash_table_new_full(
        g_str_hash, g_str_equal,
        g_free, (GDestroyNotify)bluez_device_info_free
    );
    monitor->paired_devices = g_hash_table_new_full(
        g_str_hash, g_str_equal,
        g_free, (GDestroyNotify)bluez_device_info_free
    );
    monitor->auto_connect_states = g_hash_table_new_full(
        g_str_hash, g_str_equal, g_free, g_free
    );
    monitor->auto_connect_model_attempts = g_hash_table_new_full(
        g_direct_hash, g_direct_equal, NULL, g_free
    );
    monitor->connect_in_flight = g_hash_table_new_full(
        g_str_hash, g_str_equal, g_free, NULL
    );
    monitor->advertisement_rssi = g_hash_table_new_full(
        g_str_hash, g_str_equal, g_free, g_free
    );
    monitor->discovery_adapters = g_ptr_array_new_with_free_func(g_free);

    return monitor;
}

void bluez_monitor_free(BluezMonitor *monitor)
{
    if (monitor == NULL)
        return;

    bluez_monitor_stop(monitor);
    g_hash_table_destroy(monitor->known_devices);
    g_hash_table_destroy(monitor->paired_devices);
    g_hash_table_destroy(monitor->auto_connect_states);
    g_hash_table_destroy(monitor->auto_connect_model_attempts);
    g_hash_table_unref(monitor->connect_in_flight);
    g_hash_table_destroy(monitor->advertisement_rssi);
    g_ptr_array_free(monitor->discovery_adapters, TRUE);
    g_object_unref(monitor->connection);
    g_free(monitor);
}

bool bluez_monitor_start(BluezMonitor *monitor)
{
    /* Subscribe to PropertiesChanged signal */
    monitor->properties_signal_id = g_dbus_connection_signal_subscribe(
        monitor->connection,
        BLUEZ_SERVICE,
        DBUS_PROPERTIES_INTERFACE,
        "PropertiesChanged",
        NULL,  /* Match all object paths */
        NULL,
        G_DBUS_SIGNAL_FLAGS_NONE,
        on_properties_changed,
        monitor,
        NULL
    );

    /* Subscribe to InterfacesAdded signal */
    monitor->interfaces_added_id = g_dbus_connection_signal_subscribe(
        monitor->connection,
        BLUEZ_SERVICE,
        DBUS_OBJECT_MANAGER_INTERFACE,
        "InterfacesAdded",
        "/",
        NULL,
        G_DBUS_SIGNAL_FLAGS_NONE,
        on_interfaces_added,
        monitor,
        NULL
    );

    /* Subscribe to InterfacesRemoved signal */
    monitor->interfaces_removed_id = g_dbus_connection_signal_subscribe(
        monitor->connection,
        BLUEZ_SERVICE,
        DBUS_OBJECT_MANAGER_INTERFACE,
        "InterfacesRemoved",
        "/",
        NULL,
        G_DBUS_SIGNAL_FLAGS_NONE,
        on_interfaces_removed,
        monitor,
        NULL
    );

    g_message("BlueZ monitor started");
    return true;
}

void bluez_monitor_stop(BluezMonitor *monitor)
{
    cancel_discovery_retry(monitor);
    stop_le_discovery(monitor);

    if (monitor->properties_signal_id > 0) {
        g_dbus_connection_signal_unsubscribe(monitor->connection, monitor->properties_signal_id);
        monitor->properties_signal_id = 0;
    }

    if (monitor->interfaces_added_id > 0) {
        g_dbus_connection_signal_unsubscribe(monitor->connection, monitor->interfaces_added_id);
        monitor->interfaces_added_id = 0;
    }

    if (monitor->interfaces_removed_id > 0) {
        g_dbus_connection_signal_unsubscribe(monitor->connection, monitor->interfaces_removed_id);
        monitor->interfaces_removed_id = 0;
    }
}

void bluez_monitor_set_connected_callback(BluezMonitor *monitor,
                                           BluezDeviceCallback callback,
                                           void *user_data)
{
    monitor->connected_callback = callback;
    monitor->connected_user_data = user_data;
}

void bluez_monitor_set_disconnected_callback(BluezMonitor *monitor,
                                              BluezDeviceCallback callback,
                                              void *user_data)
{
    monitor->disconnected_callback = callback;
    monitor->disconnected_user_data = user_data;
}

void bluez_monitor_set_auto_connect_on_wear(BluezMonitor *monitor, bool enabled)
{
    if (monitor == NULL || monitor->auto_connect_on_wear == enabled)
        return;

    monitor->auto_connect_on_wear = enabled;
    if (!enabled) {
        cancel_discovery_retry(monitor);
        stop_le_discovery(monitor);
    } else if (monitor->properties_signal_id > 0) {
        start_le_discovery(monitor);
    }
}

void bluez_monitor_check_existing_devices(BluezMonitor *monitor)
{
    GError *error = NULL;

    /* Call GetManagedObjects to enumerate all devices */
    GVariant *result = g_dbus_connection_call_sync(
        monitor->connection,
        BLUEZ_SERVICE,
        "/",
        DBUS_OBJECT_MANAGER_INTERFACE,
        "GetManagedObjects",
        NULL,
        G_VARIANT_TYPE("(a{oa{sa{sv}}})"),
        G_DBUS_CALL_FLAGS_NONE,
        BLUEZ_CALL_TIMEOUT_MSEC,
        NULL,
        &error
    );

    if (error) {
        g_warning("Failed to get managed objects: %s", error->message);
        g_error_free(error);
        schedule_discovery_retry(monitor);
        return;
    }

    GVariant *objects = NULL;
    g_variant_get(result, "(@a{oa{sa{sv}}})", &objects);

    GVariantIter iter;
    const gchar *object_path;
    GVariant *interfaces;

    g_variant_iter_init(&iter, objects);
    while (g_variant_iter_next(&iter, "{&o@a{sa{sv}}}", &object_path, &interfaces)) {
        GVariant *device_properties = g_variant_lookup_value(
            interfaces, BLUEZ_DEVICE_INTERFACE, G_VARIANT_TYPE("a{sv}"));
        if (device_properties != NULL) {
            BluezDeviceInfo *info = get_device_info(monitor->connection,
                                                     object_path);
            /* Random advertisement objects are neither paired nor connected
             * and normally have no UUID cache. Avoid a synchronous UUID
             * lookup for every nearby BLE broadcaster. */
            if (info != NULL && (info->paired || info->connected) &&
                device_is_airpods(monitor->connection, object_path)) {
                cache_paired_device(monitor, object_path, info);
                if (info && info->connected) {
                    g_message("Found already connected AirPods: %s (%s)",
                              info->name ? info->name : "Unknown",
                              info->address ? info->address : "Unknown");

                    g_hash_table_insert(monitor->known_devices,
                                        g_strdup(object_path),
                                        bluez_device_info_copy(info));

                    if (monitor->connected_callback) {
                        monitor->connected_callback(info, monitor->connected_user_data);
                    }
                }
            }
            bluez_device_info_free(info);
            g_variant_unref(device_properties);
        }
        g_variant_unref(interfaces);
    }

    g_variant_unref(objects);
    g_variant_unref(result);

    start_le_discovery(monitor);
}

BluezDeviceInfo *bluez_monitor_find_connected_device(BluezMonitor *monitor,
                                                      const char *exclude_address)
{
    if (monitor == NULL)
        return NULL;

    GHashTableIter iter;
    gpointer value = NULL;
    g_hash_table_iter_init(&iter, monitor->known_devices);

    while (g_hash_table_iter_next(&iter, NULL, &value)) {
        const BluezDeviceInfo *info = value;
        if (info->connected && info->address != NULL &&
            (exclude_address == NULL ||
             g_ascii_strcasecmp(info->address, exclude_address) != 0)) {
            return bluez_device_info_copy(info);
        }
    }

    return NULL;
}
