/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2024 EarPort Contributors
 */

#include "bluez_monitor.h"
#include "bluetooth.h"
#include "ble_autoconnect.h"
#include "connection_policy.h"

#include <stdio.h>
#include <string.h>

#define APPLE_COMPANY_ID 0x004c
#define AUTO_CONNECT_MIN_RSSI_DBM (-70)
#define AUTO_CONNECT_FAST_RSSI_DBM (-60)
#define AUTO_CONNECT_CONFIRM_WINDOW_USEC (2 * G_USEC_PER_SEC)
#define AUTO_CONNECT_COOLDOWN_USEC (30 * G_USEC_PER_SEC)
#define AUTO_CONNECT_RSSI_MAX_AGE_USEC (5 * G_USEC_PER_SEC)
#define HANDOFF_UNWORN_REARM_USEC (750 * 1000)
#define BLUEZ_CALL_TIMEOUT_MSEC 5000
#define RAPID_REWEAR_CONNECT_RETRY_BASE_MSEC 250
#define RAPID_REWEAR_CONNECT_MAX_ATTEMPTS 4
#define STOP_DISCOVERY_RETRY_MSEC 2000
#define STOP_DISCOVERY_DISPOSE_ATTEMPTS 3
#define STOP_DISCOVERY_MAX_ATTEMPTS 5

typedef struct {
    gint value;
    gint64 observed_usec;
} AdvertisementRssi;

struct BluezMonitor {
    gint ref_count;
    bool disposed;
    bool monitoring_started;
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
    GHashTable *auto_connect_state_models; /* advertising path -> BLE model */
    GHashTable *auto_connect_model_attempts; /* BLE model -> gint64 */
    GHashTable *handoff_suppressions; /* BLE model -> suppression state */
    GHashTable *connect_in_flight;   /* paired Device1 path set */
    GHashTable *disconnect_in_flight; /* paired Device1 path set */
    GHashTable *device_resolve_in_flight; /* Device1 path set */
    GHashTable *discovery_start_in_flight; /* adapter path set */
    GHashTable *stop_in_flight; /* adapter path set */
    GHashTable *adapter_power_states; /* adapter path -> encoded bool */
    GHashTable *advertisement_rssi; /* advertising path -> AdvertisementRssi */
    GPtrArray *discovery_adapters;  /* adapter paths whose session we own */
    bool managed_objects_in_flight;
    guint discovery_retry_id;
};

static void start_le_discovery(BluezMonitor *monitor);
static void stop_le_discovery(BluezMonitor *monitor);
static void request_stop_discovery(BluezMonitor *monitor,
                                   const char *object_path);
static void request_managed_objects(BluezMonitor *monitor);
static void cancel_discovery_retry(BluezMonitor *monitor);
static BluezMonitor *bluez_monitor_ref(BluezMonitor *monitor);
static void bluez_monitor_unref(BluezMonitor *monitor);

static BluezMonitor *bluez_monitor_ref(BluezMonitor *monitor)
{
    g_atomic_int_inc(&monitor->ref_count);
    return monitor;
}

static bool forget_discovery_adapter(BluezMonitor *monitor,
                                     const char *object_path)
{
    for (guint i = 0; i < monitor->discovery_adapters->len; i++) {
        if (g_strcmp0(g_ptr_array_index(monitor->discovery_adapters, i),
                      object_path) == 0) {
            g_ptr_array_remove_index(monitor->discovery_adapters, i);
            return true;
        }
    }
    return false;
}

static void remember_adapter_power(BluezMonitor *monitor,
                                   const char *object_path,
                                   bool powered)
{
    g_hash_table_replace(monitor->adapter_power_states,
                         g_strdup(object_path),
                         GINT_TO_POINTER(powered ? 2 : 1));
}

static bool adapter_is_powered(BluezMonitor *monitor,
                               const char *object_path)
{
    return GPOINTER_TO_INT(g_hash_table_lookup(
               monitor->adapter_power_states, object_path)) == 2;
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

static bool properties_identify_airpods(GVariant *props,
                                        bool *uuids_were_supplied);

static void device_info_apply_properties(BluezDeviceInfo *info,
                                         GVariant *props)
{
    GVariant *value = NULL;

    value = g_variant_lookup_value(props, "Address", G_VARIANT_TYPE_STRING);
    if (value != NULL) {
        g_free(info->address);
        info->address = g_variant_dup_string(value, NULL);
        g_variant_unref(value);
    }

    value = g_variant_lookup_value(props, "Name", G_VARIANT_TYPE_STRING);
    if (value != NULL) {
        g_free(info->name);
        info->name = g_variant_dup_string(value, NULL);
        g_variant_unref(value);
    }

    value = g_variant_lookup_value(props, "Connected",
                                   G_VARIANT_TYPE_BOOLEAN);
    if (value != NULL) {
        info->connected = g_variant_get_boolean(value);
        g_variant_unref(value);
    }

    value = g_variant_lookup_value(props, "Paired", G_VARIANT_TYPE_BOOLEAN);
    if (value != NULL) {
        info->paired = g_variant_get_boolean(value);
        g_variant_unref(value);
    }

    value = g_variant_lookup_value(props, "Modalias", G_VARIANT_TYPE_STRING);
    if (value != NULL) {
        const char *modalias = g_variant_get_string(value, NULL);
        info->vendor_id = 0;
        info->product_id = 0;
        parse_bluez_modalias(modalias, &info->vendor_id, &info->product_id);
        g_variant_unref(value);
    }
}

static BluezDeviceInfo *device_info_from_properties(const char *object_path,
                                                    GVariant *props)
{
    BluezDeviceInfo *info = g_new0(BluezDeviceInfo, 1);
    info->object_path = g_strdup(object_path);
    device_info_apply_properties(info, props);
    return info;
}

static bool properties_identify_airpods(GVariant *props,
                                        bool *uuids_were_supplied)
{
    GVariant *value = g_variant_lookup_value(
        props, "UUIDs", G_VARIANT_TYPE_STRING_ARRAY);
    if (uuids_were_supplied != NULL)
        *uuids_were_supplied = value != NULL;
    if (value == NULL)
        return false;

    bool is_airpods = false;
    gsize n_uuids = 0;
    const gchar **uuids = g_variant_get_strv(value, &n_uuids);
    for (gsize i = 0; i < n_uuids; i++) {
        if (g_ascii_strcasecmp(uuids[i], AIRPODS_UUID) == 0) {
            is_airpods = true;
            break;
        }
    }

    g_free(uuids);
    g_variant_unref(value);
    return is_airpods;
}

static void fill_missing_device_identity(BluezDeviceInfo *destination,
                                         const BluezDeviceInfo *source)
{
    if (destination == NULL || source == NULL)
        return;

    if (destination->address == NULL)
        destination->address = g_strdup(source->address);
    if (destination->name == NULL)
        destination->name = g_strdup(source->name);
    if (destination->object_path == NULL)
        destination->object_path = g_strdup(source->object_path);
    if (destination->vendor_id == 0 && source->vendor_id != 0)
        destination->vendor_id = source->vendor_id;
    if (destination->product_id == 0 && source->product_id != 0)
        destination->product_id = source->product_id;
    destination->paired = destination->paired || source->paired;
}

/* The connected cache is authoritative for link state, while the paired
 * cache may carry a Modalias that arrived later. Merge both so a partial
 * PropertiesChanged signal cannot downgrade a known product to zero. */
static BluezDeviceInfo *copy_richest_cached_device(BluezMonitor *monitor,
                                                   const char *object_path)
{
    const BluezDeviceInfo *known = g_hash_table_lookup(
        monitor->known_devices, object_path);
    const BluezDeviceInfo *paired = g_hash_table_lookup(
        monitor->paired_devices, object_path);
    const BluezDeviceInfo *base = known != NULL ? known : paired;
    if (base == NULL)
        return NULL;

    BluezDeviceInfo *info = bluez_device_info_copy(base);
    fill_missing_device_identity(info, known);
    fill_missing_device_identity(info, paired);
    if (known != NULL)
        info->connected = known->connected;
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
                                const BluezDeviceInfo *info,
                                bool verified_airpods)
{
    if (info != NULL && info->paired && verified_airpods) {
        g_hash_table_replace(monitor->paired_devices,
                             g_strdup(object_path),
                             bluez_device_info_copy(info));
    } else {
        g_hash_table_remove(monitor->paired_devices, object_path);
    }
}

typedef struct {
    BluezMonitor *monitor;
    GHashTable *in_flight;
    GHashTable *auto_connect_states;
    char *object_path;
    char *advertisement_path;
    char *device_name;
    bool direct_rewear;
    guint attempt;
} AutoConnectRequest;

static void auto_connect_request_free(AutoConnectRequest *request)
{
    g_hash_table_unref(request->in_flight);
    g_hash_table_unref(request->auto_connect_states);
    bluez_monitor_unref(request->monitor);
    g_free(request->object_path);
    g_free(request->advertisement_path);
    g_free(request->device_name);
    g_free(request);
}

static void request_device_connect_attempt(BluezMonitor *monitor,
                                           const BluezDeviceInfo *device,
                                           const char *advertisement_path,
                                           bool direct_rewear,
                                           guint attempt);

static bool rapid_rewear_connect_error_is_retryable(const GError *error)
{
    if (error == NULL)
        return false;

    gchar *remote_error = g_dbus_error_get_remote_error(error);
    bool terminal =
        g_strcmp0(remote_error, "org.bluez.Error.NotPaired") == 0 ||
        g_strcmp0(remote_error, "org.bluez.Error.NotSupported") == 0 ||
        g_strcmp0(remote_error, "org.bluez.Error.InvalidArguments") == 0 ||
        g_strcmp0(remote_error,
                  "org.freedesktop.DBus.Error.UnknownObject") == 0 ||
        g_strcmp0(remote_error,
                  "org.freedesktop.DBus.Error.ServiceUnknown") == 0;
    g_free(remote_error);
    return !terminal;
}

static gboolean retry_rapid_rewear_connect(gpointer user_data)
{
    AutoConnectRequest *request = user_data;
    BluezMonitor *monitor = request->monitor;

    if (monitor->disposed ||
        g_hash_table_contains(monitor->known_devices,
                              request->object_path) ||
        g_hash_table_contains(monitor->connect_in_flight,
                              request->object_path)) {
        return G_SOURCE_REMOVE;
    }

    BluezDeviceInfo device = {
        .name = request->device_name,
        .object_path = request->object_path,
        .paired = true,
    };
    request_device_connect_attempt(monitor, &device,
                                   request->advertisement_path,
                                   true, request->attempt + 1);
    return G_SOURCE_REMOVE;
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

    bool retry_scheduled = false;
    if (error != NULL) {
        if (request->direct_rewear &&
            request->attempt < RAPID_REWEAR_CONNECT_MAX_ATTEMPTS &&
            !request->monitor->disposed &&
            rapid_rewear_connect_error_is_retryable(error)) {
            guint delay = RAPID_REWEAR_CONNECT_RETRY_BASE_MSEC <<
                          (request->attempt - 1);
            g_warning("Rapid-rewear connect attempt %u failed for %s: %s; retrying in %u ms",
                      request->attempt,
                      request->device_name ? request->device_name : "AirPods",
                      error->message, delay);
            g_timeout_add_full(G_PRIORITY_DEFAULT, delay,
                               retry_rapid_rewear_connect, request,
                               (GDestroyNotify)auto_connect_request_free);
            retry_scheduled = true;
        } else {
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
        }
        g_error_free(error);
    } else {
        g_message("Auto-connect request accepted for %s",
                  request->device_name ? request->device_name : "AirPods");
    }

    if (reply != NULL)
        g_variant_unref(reply);
    if (!retry_scheduled)
        auto_connect_request_free(request);
}

static void request_device_connect_attempt(BluezMonitor *monitor,
                                           const BluezDeviceInfo *device,
                                           const char *advertisement_path,
                                           bool direct_rewear,
                                           guint attempt)
{
    if (g_hash_table_contains(monitor->connect_in_flight,
                              device->object_path)) {
        return;
    }

    g_hash_table_add(monitor->connect_in_flight,
                     g_strdup(device->object_path));

    AutoConnectRequest *request = g_new0(AutoConnectRequest, 1);
    request->monitor = bluez_monitor_ref(monitor);
    request->in_flight = g_hash_table_ref(monitor->connect_in_flight);
    request->auto_connect_states = g_hash_table_ref(
        monitor->auto_connect_states);
    request->object_path = g_strdup(device->object_path);
    request->advertisement_path = g_strdup(advertisement_path);
    request->device_name = g_strdup(device->name);
    request->direct_rewear = direct_rewear;
    request->attempt = attempt;

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

static void request_device_connect(BluezMonitor *monitor,
                                   const BluezDeviceInfo *device,
                                   const char *advertisement_path)
{
    request_device_connect_attempt(monitor, device, advertisement_path,
                                   false, 1);
}

typedef struct {
    gint ref_count;
    BluezMonitor *monitor;
    char *object_path;
    char *device_address;
    char *device_name;
    uint16_t product_id;
    unsigned int attempt;
    BluezDisconnectRetryCheck retry_check;
    BluezDisconnectFinishedCallback callback;
    void *user_data;
} DisconnectRequest;

static DisconnectRequest *disconnect_request_ref(DisconnectRequest *request)
{
    g_atomic_int_inc(&request->ref_count);
    return request;
}

static void disconnect_request_unref(DisconnectRequest *request)
{
    if (!g_atomic_int_dec_and_test(&request->ref_count))
        return;

    g_hash_table_remove(request->monitor->disconnect_in_flight,
                        request->object_path);
    bluez_monitor_unref(request->monitor);
    g_free(request->object_path);
    g_free(request->device_address);
    g_free(request->device_name);
    g_free(request);
}

static bool rearm_auto_connect_model(
    GHashTable *auto_connect_states,
    GHashTable *auto_connect_state_models,
    GHashTable *auto_connect_model_attempts,
    GHashTable *handoff_suppressions,
    uint16_t product_id)
{
    if (product_id == 0)
        return false;

    uint16_t ble_model = ble_airpods_bluez_product_id(product_id);
    gpointer model_key = GUINT_TO_POINTER((guint)ble_model);
    g_hash_table_remove(auto_connect_model_attempts, model_key);
    g_hash_table_remove(handoff_suppressions, model_key);

    GHashTableIter iter;
    gpointer path = NULL;
    gpointer value = NULL;
    g_hash_table_iter_init(&iter, auto_connect_state_models);
    while (g_hash_table_iter_next(&iter, &path, &value)) {
        if (value != model_key)
            continue;

        g_hash_table_remove(auto_connect_states, path);
        g_hash_table_iter_remove(&iter);
    }
    return true;
}

static void rearm_auto_connect_after_removal(DisconnectRequest *request)
{
    BluezMonitor *monitor = request->monitor;
    rearm_auto_connect_model(monitor->auto_connect_states,
                             monitor->auto_connect_state_models,
                             monitor->auto_connect_model_attempts,
                             monitor->handoff_suppressions,
                             request->product_id);
}

static bool error_is_bluez_not_connected(const GError *error)
{
    if (error == NULL)
        return false;

    gchar *remote_error = g_dbus_error_get_remote_error(error);
    bool not_connected = g_strcmp0(remote_error,
                                   "org.bluez.Error.NotConnected") == 0;
    g_free(remote_error);
    return not_connected;
}

static bool disconnect_error_is_retryable(const GError *error)
{
    if (error == NULL || g_error_matches(error, G_IO_ERROR,
                                         G_IO_ERROR_CANCELLED)) {
        return false;
    }

    gchar *remote_error = g_dbus_error_get_remote_error(error);
    bool terminal =
        g_strcmp0(remote_error, "org.bluez.Error.NotPaired") == 0 ||
        g_strcmp0(remote_error, "org.bluez.Error.NotSupported") == 0 ||
        g_strcmp0(remote_error, "org.bluez.Error.InvalidArguments") == 0 ||
        g_strcmp0(remote_error,
                  "org.freedesktop.DBus.Error.AccessDenied") == 0 ||
        g_strcmp0(remote_error,
                  "org.freedesktop.DBus.Error.UnknownMethod") == 0 ||
        g_strcmp0(remote_error,
                  "org.freedesktop.DBus.Error.UnknownObject") == 0 ||
        g_strcmp0(remote_error,
                  "org.freedesktop.DBus.Error.ServiceUnknown") == 0;
    g_free(remote_error);
    return !terminal;
}

static bool disconnect_is_still_required(DisconnectRequest *request)
{
    return !request->monitor->disposed && request->retry_check != NULL &&
           request->retry_check(request->device_address, request->user_data);
}

static void notify_disconnect_finished(DisconnectRequest *request,
                                       bool completed)
{
    if (!request->monitor->disposed && request->callback != NULL) {
        request->callback(request->device_address, completed,
                          request->user_data);
    }
}

static void send_disconnect_request(DisconnectRequest *request);

static gboolean retry_disconnect_timeout_cb(gpointer user_data)
{
    DisconnectRequest *request = user_data;
    if (!disconnect_is_still_required(request)) {
        notify_disconnect_finished(request, false);
        return G_SOURCE_REMOVE;
    }

    request->attempt++;
    g_message("Retrying removal disconnect for %s (attempt %u/%u)",
              request->device_name ? request->device_name : "AirPods",
              request->attempt, REMOVAL_DISCONNECT_MAX_ATTEMPTS);
    send_disconnect_request(request);
    return G_SOURCE_REMOVE;
}

static void on_disconnect_finished(GObject *source_object,
                                   GAsyncResult *result,
                                   gpointer user_data)
{
    DisconnectRequest *request = user_data;
    GError *error = NULL;
    GVariant *reply = g_dbus_connection_call_finish(
        G_DBUS_CONNECTION(source_object), result, &error);

    bool already_disconnected = error_is_bluez_not_connected(error);
    if (error != NULL && !already_disconnected) {
        bool retryable = disconnect_error_is_retryable(error);
        bool still_required = disconnect_is_still_required(request);
        if (removal_disconnect_retry_should_schedule(request->attempt,
                                                     retryable,
                                                     still_required)) {
            unsigned int delay = removal_disconnect_retry_delay_msec(
                request->attempt);
            g_warning("Removal disconnect attempt %u failed for %s: %s; retrying in %u ms",
                      request->attempt,
                      request->device_name ? request->device_name : "AirPods",
                      error->message, delay);
            g_timeout_add_full(
                G_PRIORITY_DEFAULT,
                delay,
                retry_disconnect_timeout_cb,
                disconnect_request_ref(request),
                (GDestroyNotify)disconnect_request_unref);
        } else {
            g_warning("Removal disconnect failed for %s after %u attempt(s): %s",
                      request->device_name ? request->device_name : "AirPods",
                      request->attempt, error->message);
            notify_disconnect_finished(request, false);
        }
    } else {
        /* The deliberate removal cycle is complete. Drop only this model's
         * cooldown and advertisement states, so the restarted scan must see
         * a fresh unworn -> worn edge but can reconnect without waiting for
         * the normal 30-second retry cooldown. */
        rearm_auto_connect_after_removal(request);
        g_message("Removal disconnect accepted for %s",
                  request->device_name ? request->device_name : "AirPods");
        notify_disconnect_finished(request, true);
    }

    if (error != NULL)
        g_error_free(error);

    if (reply != NULL)
        g_variant_unref(reply);
    disconnect_request_unref(request);
}

static void send_disconnect_request(DisconnectRequest *request)
{
    g_dbus_connection_call(
        request->monitor->connection,
        BLUEZ_SERVICE,
        request->object_path,
        BLUEZ_DEVICE_INTERFACE,
        "Disconnect",
        NULL,
        G_VARIANT_TYPE("()"),
        G_DBUS_CALL_FLAGS_NONE,
        BLUEZ_CALL_TIMEOUT_MSEC,
        NULL,
        on_disconnect_finished,
        disconnect_request_ref(request));
}

static bool selector_matches_device(const BluezDeviceInfo *info,
                                    const char *selector)
{
    return info != NULL && selector != NULL &&
           ((info->address != NULL &&
             g_ascii_strcasecmp(info->address, selector) == 0) ||
            (info->object_path != NULL &&
             g_strcmp0(info->object_path, selector) == 0));
}

static BluezDeviceInfo *find_cached_target(BluezMonitor *monitor,
                                           const char *selector,
                                           bool require_connected)
{
    GHashTable *tables[] = {
        monitor->known_devices,
        monitor->paired_devices,
    };
    for (guint table_index = 0; table_index < G_N_ELEMENTS(tables);
         table_index++) {
        GHashTableIter iter;
        gpointer object_path = NULL;
        gpointer value = NULL;
        g_hash_table_iter_init(&iter, tables[table_index]);
        while (g_hash_table_iter_next(&iter, &object_path, &value)) {
            const BluezDeviceInfo *info = value;
            if ((!require_connected || info->connected) &&
                selector_matches_device(info, selector)) {
                return copy_richest_cached_device(monitor, object_path);
            }
        }
    }

    return NULL;
}

static BluezDeviceInfo *find_disconnect_target(BluezMonitor *monitor,
                                                const char *selector)
{
    return find_cached_target(monitor, selector, true);
}

bool bluez_monitor_connect_device(BluezMonitor *monitor,
                                  const char *selector)
{
    if (monitor == NULL || monitor->disposed || selector == NULL ||
        selector[0] == '\0') {
        return false;
    }

    BluezDeviceInfo *target = find_cached_target(monitor, selector, false);
    if (target == NULL || !target->paired || target->connected ||
        target->object_path == NULL) {
        bluez_device_info_free(target);
        return false;
    }

    if (!g_hash_table_contains(monitor->connect_in_flight,
                               target->object_path)) {
        g_message("Reconnecting %s after rapid rewear during removal disconnect",
                  target->name ? target->name : "AirPods");
        /* The Device1 path is a stable non-NULL key if Connect fails. There may
         * be no live BLE advertisement path during this narrow race. */
        request_device_connect_attempt(monitor, target, target->object_path,
                                       true, 1);
    }

    bluez_device_info_free(target);
    return true;
}

bool bluez_monitor_disconnect_device(BluezMonitor *monitor,
                                     const char *selector,
                                     BluezDisconnectRetryCheck retry_check,
                                     BluezDisconnectFinishedCallback callback,
                                     void *user_data)
{
    if (monitor == NULL || monitor->disposed || selector == NULL ||
        selector[0] == '\0') {
        return false;
    }

    BluezDeviceInfo *target = find_disconnect_target(monitor, selector);
    if (target == NULL || target->object_path == NULL) {
        g_debug("Cannot disconnect AirPods: connected cached target not found");
        bluez_device_info_free(target);
        return false;
    }

    if (g_hash_table_contains(monitor->disconnect_in_flight,
                              target->object_path)) {
        bluez_device_info_free(target);
        return true;
    }

    g_hash_table_add(monitor->disconnect_in_flight,
                     g_strdup(target->object_path));

    DisconnectRequest *request = g_new0(DisconnectRequest, 1);
    request->ref_count = 1;
    request->monitor = bluez_monitor_ref(monitor);
    request->object_path = g_strdup(target->object_path);
    request->device_address = g_strdup(target->address);
    request->device_name = g_strdup(target->name);
    request->product_id = target->product_id;
    request->attempt = 1;
    request->retry_check = retry_check;
    request->callback = callback;
    request->user_data = user_data;

    g_message("AirPods stayed fully removed; disconnecting %s",
              target->name ? target->name : "AirPods");
    send_disconnect_request(request);

    bluez_device_info_free(target);
    disconnect_request_unref(request);
    return true;
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
                                  info->object_path) ||
            g_hash_table_contains(monitor->disconnect_in_flight,
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
    if (!monitor->auto_connect_on_wear || manufacturer_data == NULL ||
        g_hash_table_size(monitor->known_devices) > 0)
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
        BleHandoffSuppressionState *suppression = g_hash_table_lookup(
            monitor->handoff_suppressions, model_key);
        if (suppression != NULL) {
            if (ble_handoff_suppression_observe(
                    suppression, advertisement.worn, now_usec,
                    HANDOFF_UNWORN_REARM_USEC)) {
                return;
            }

            g_hash_table_remove(monitor->handoff_suppressions, model_key);
            g_hash_table_remove(monitor->auto_connect_model_attempts,
                                model_key);
            g_debug("Rearmed wear auto-connect after a stable removal");
        }

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
        g_hash_table_replace(monitor->auto_connect_state_models,
                             g_strdup(object_path), model_key);

        BleAutoConnectState state_before_observation = *state;
        if (!ble_autoconnect_observe(state,
                                     advertisement.worn,
                                     advertisement.worn &&
                                         rssi->value >= AUTO_CONNECT_FAST_RSSI_DBM,
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

static bool verify_airpods_device(BluezMonitor *monitor,
                                  const char *object_path,
                                  GVariant *properties)
{
    if (g_hash_table_contains(monitor->known_devices, object_path) ||
        g_hash_table_contains(monitor->paired_devices, object_path)) {
        return true;
    }

    bool uuids_supplied = false;
    bool supplied_match = properties_identify_airpods(properties,
                                                       &uuids_supplied);
    if (uuids_supplied)
        return supplied_match;

    return false;
}

typedef struct {
    BluezMonitor *monitor;
    char *object_path;
} DeviceResolveRequest;

static void device_resolve_request_free(DeviceResolveRequest *request)
{
    bluez_monitor_unref(request->monitor);
    g_free(request->object_path);
    g_free(request);
}

static void cache_resolved_device(BluezMonitor *monitor,
                                  const BluezDeviceInfo *info)
{
    cache_paired_device(monitor, info->object_path, info, true);
    if (!info->connected)
        return;

    bool already_known = g_hash_table_contains(monitor->known_devices,
                                                info->object_path);
    g_hash_table_replace(monitor->known_devices,
                         g_strdup(info->object_path),
                         bluez_device_info_copy(info));
    cancel_discovery_retry(monitor);
    stop_le_discovery(monitor);

    if (!already_known && monitor->connected_callback != NULL) {
        monitor->connected_callback(info, monitor->connected_user_data);
    }
}

static void on_device_resolved(GObject *source_object,
                               GAsyncResult *result,
                               gpointer user_data)
{
    DeviceResolveRequest *request = user_data;
    BluezMonitor *monitor = request->monitor;
    g_hash_table_remove(monitor->device_resolve_in_flight,
                        request->object_path);

    GError *error = NULL;
    GVariant *reply = g_dbus_connection_call_finish(
        G_DBUS_CONNECTION(source_object), result, &error);
    if (error != NULL) {
        if (!monitor->disposed) {
            g_debug("Could not resolve BlueZ device %s: %s",
                    request->object_path, error->message);
        }
        g_error_free(error);
        device_resolve_request_free(request);
        return;
    }

    if (!monitor->disposed && monitor->monitoring_started) {
        GVariant *properties = NULL;
        g_variant_get(reply, "(@a{sv})", &properties);
        BluezDeviceInfo *info = device_info_from_properties(
            request->object_path, properties);
        bool verified = properties_identify_airpods(properties, NULL);
        if (verified && (info->paired || info->connected))
            cache_resolved_device(monitor, info);
        bluez_device_info_free(info);
        g_variant_unref(properties);
    }

    g_variant_unref(reply);
    device_resolve_request_free(request);
}

static void resolve_device_async(BluezMonitor *monitor,
                                 const char *object_path)
{
    if (g_hash_table_contains(monitor->device_resolve_in_flight,
                              object_path)) {
        return;
    }

    g_hash_table_add(monitor->device_resolve_in_flight,
                     g_strdup(object_path));
    DeviceResolveRequest *request = g_new0(DeviceResolveRequest, 1);
    request->monitor = bluez_monitor_ref(monitor);
    request->object_path = g_strdup(object_path);
    g_dbus_connection_call(
        monitor->connection,
        BLUEZ_SERVICE,
        object_path,
        DBUS_PROPERTIES_INTERFACE,
        "GetAll",
        g_variant_new("(s)", BLUEZ_DEVICE_INTERFACE),
        G_VARIANT_TYPE("(a{sv})"),
        G_DBUS_CALL_FLAGS_NONE,
        BLUEZ_CALL_TIMEOUT_MSEC,
        NULL,
        on_device_resolved,
        request);
}

static void on_properties_changed(GDBusConnection *connection G_GNUC_UNUSED,
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
        if (powered != NULL) {
            bool is_powered = g_variant_get_boolean(powered);
            remember_adapter_power(monitor, object_path, is_powered);
            /* A prior discovery session is lost across adapter power cycles. */
            forget_discovery_adapter(monitor, object_path);
            if (is_powered)
                start_le_discovery(monitor);
            else
                cancel_discovery_retry(monitor);
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
    GVariant *paired_var = g_variant_lookup_value(
        changed_props, "Paired", G_VARIANT_TYPE_BOOLEAN);
    GVariant *modalias_var = g_variant_lookup_value(
        changed_props, "Modalias", G_VARIANT_TYPE_STRING);
    GVariant *uuids_var = g_variant_lookup_value(
        changed_props, "UUIDs", G_VARIANT_TYPE_STRING_ARRAY);
    bool connected_changed = connected_var != NULL;
    bool connected = connected_changed && g_variant_get_boolean(connected_var);
    bool paired_changed = paired_var != NULL;
    bool paired = paired_changed && g_variant_get_boolean(paired_var);
    bool identity_changed = connected_var != NULL || paired_var != NULL ||
                            modalias_var != NULL || uuids_var != NULL;
    if (connected_var != NULL)
        g_variant_unref(connected_var);
    if (paired_var != NULL)
        g_variant_unref(paired_var);
    if (modalias_var != NULL)
        g_variant_unref(modalias_var);
    if (uuids_var != NULL)
        g_variant_unref(uuids_var);
    if (!identity_changed) {
        g_variant_unref(changed_props);
        return;
    }

    bool was_known_connected = g_hash_table_contains(monitor->known_devices,
                                                       object_path);
    bool was_verified = was_known_connected ||
        g_hash_table_contains(monitor->paired_devices, object_path);
    BluezDeviceInfo *info = copy_richest_cached_device(monitor, object_path);
    if (info != NULL) {
        device_info_apply_properties(info, changed_props);
    } else {
        info = device_info_from_properties(object_path, changed_props);
        if (info->paired || info->connected) {
            bool uuids_supplied = false;
            bool supplied_airpods = properties_identify_airpods(
                changed_props, &uuids_supplied);
            if (!uuids_supplied) {
                /* Resolve this one newly paired/connected candidate without
                 * ever blocking the main loop. The completion rechecks its
                 * current Connected state before publishing it. */
                resolve_device_async(monitor, object_path);
                g_variant_unref(changed_props);
                bluez_device_info_free(info);
                return;
            }
            if (!supplied_airpods) {
                g_variant_unref(changed_props);
                bluez_device_info_free(info);
                return;
            }
            was_verified = true;
        }
    }

    if (info == NULL) {
        g_variant_unref(changed_props);
        if (connected_changed && !connected &&
            g_hash_table_size(monitor->known_devices) == 0) {
            start_le_discovery(monitor);
        }
        return;
    }

    bool is_airpods = was_verified || verify_airpods_device(
        monitor, object_path, changed_props);
    g_variant_unref(changed_props);
    if (!is_airpods) {
        bluez_device_info_free(info);
        return;
    }

    if (paired_changed)
        info->paired = paired;
    if (connected_changed)
        info->connected = connected;

    cache_paired_device(monitor, object_path, info, true);
    if (!connected_changed) {
        /* Paired and Modalias commonly arrive in separate signals. Refresh a
         * connected entry too, otherwise its product_id can remain zero and
         * overwrite the richer paired identity during removal. */
        if (was_known_connected) {
            info->connected = true;
            g_hash_table_replace(monitor->known_devices,
                                 g_strdup(object_path),
                                 bluez_device_info_copy(info));
        }
        bluez_device_info_free(info);
        return;
    }

    g_message("AirPods %s: %s (%s)",
              connected ? "connected" : "disconnected",
              info->name ? info->name : "Unknown",
              info->address ? info->address : "Unknown");

    if (connected) {
        /* Store in known devices */
        g_hash_table_insert(monitor->known_devices,
                            g_strdup(object_path),
                            bluez_device_info_copy(info));

        /* Wear auto-connect is only needed while no AirPods are connected.
         * Release our discovery session once BlueZ establishes the link. */
        cancel_discovery_retry(monitor);
        stop_le_discovery(monitor);

        if (!was_known_connected && monitor->connected_callback) {
            monitor->connected_callback(info, monitor->connected_user_data);
        }
    } else {
        /* Remove from known devices */
        g_hash_table_remove(monitor->known_devices, object_path);

        if (was_known_connected && monitor->disconnected_callback) {
            monitor->disconnected_callback(info, monitor->disconnected_user_data);
        }

        if (g_hash_table_size(monitor->known_devices) == 0)
            start_le_discovery(monitor);
    }

    bluez_device_info_free(info);
}

static void on_interfaces_added(GDBusConnection *connection G_GNUC_UNUSED,
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
        bool is_powered = powered == NULL || g_variant_get_boolean(powered);
        remember_adapter_power(monitor, obj_path, is_powered);
        if (is_powered) {
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
        /* InterfacesAdded usually carries the complete initial Device1
         * dictionary. Parse it directly; if UUIDs are absent, resolve the
         * candidate asynchronously. */
        info = device_info_from_properties(obj_path, device_properties);
        bool is_airpods = verify_airpods_device(
            monitor, obj_path, device_properties);
        if (!is_airpods) {
            bool uuids_supplied = false;
            properties_identify_airpods(device_properties, &uuids_supplied);
            if (!uuids_supplied)
                resolve_device_async(monitor, obj_path);
        }
        if (is_airpods) {
            cache_paired_device(monitor, obj_path, info, true);
            if (info->connected) {
                g_message("New connected AirPods discovered: %s", info->name);

                g_hash_table_insert(monitor->known_devices,
                                    g_strdup(obj_path),
                                    bluez_device_info_copy(info));

                cancel_discovery_retry(monitor);
                stop_le_discovery(monitor);

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
    bool removed_known_device = info != NULL;
    if (info) {
        g_message("AirPods device removed: %s", info->name);

        if (monitor->disconnected_callback) {
            monitor->disconnected_callback(info, monitor->disconnected_user_data);
        }

        g_hash_table_remove(monitor->known_devices, obj_path);
    }

    g_hash_table_remove(monitor->paired_devices, obj_path);
    g_hash_table_remove(monitor->connect_in_flight, obj_path);
    g_hash_table_remove(monitor->device_resolve_in_flight, obj_path);
    g_hash_table_remove(monitor->adapter_power_states, obj_path);
    g_hash_table_remove(monitor->advertisement_rssi, obj_path);
    g_hash_table_remove(monitor->auto_connect_states, obj_path);
    g_hash_table_remove(monitor->auto_connect_state_models, obj_path);
    bool removed_discovery_adapter = forget_discovery_adapter(monitor,
                                                               obj_path);

    if ((removed_known_device || removed_discovery_adapter) &&
        g_hash_table_size(monitor->known_devices) == 0)
        start_le_discovery(monitor);
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
    if (!monitor->disposed && monitor->monitoring_started &&
        monitor->auto_connect_on_wear &&
        monitor->discovery_adapters->len == 0 &&
        g_hash_table_size(monitor->discovery_start_in_flight) == 0 &&
        !monitor->managed_objects_in_flight &&
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

static bool discovery_should_run(BluezMonitor *monitor)
{
    return !monitor->disposed && monitor->monitoring_started &&
           monitor->auto_connect_on_wear &&
           g_hash_table_size(monitor->known_devices) == 0;
}

typedef struct {
    BluezMonitor *monitor;
    char *object_path;
} StartDiscoveryRequest;

static void start_discovery_request_free(StartDiscoveryRequest *request)
{
    bluez_monitor_unref(request->monitor);
    g_free(request->object_path);
    g_free(request);
}

static void finish_discovery_start(StartDiscoveryRequest *request,
                                   bool active)
{
    BluezMonitor *monitor = request->monitor;
    g_hash_table_remove(monitor->discovery_start_in_flight,
                        request->object_path);

    if (active && !discovery_adapter_is_owned(monitor,
                                               request->object_path)) {
        g_ptr_array_add(monitor->discovery_adapters,
                        g_strdup(request->object_path));
    }

    if (active && (!discovery_should_run(monitor) ||
                   !adapter_is_powered(monitor, request->object_path))) {
        request_stop_discovery(monitor, request->object_path);
    } else if (active) {
        cancel_discovery_retry(monitor);
    } else {
        schedule_discovery_retry(monitor);
    }
}

static void on_start_discovery_finished(GObject *source_object,
                                        GAsyncResult *result,
                                        gpointer user_data)
{
    StartDiscoveryRequest *request = user_data;
    GError *error = NULL;
    GVariant *reply = g_dbus_connection_call_finish(
        G_DBUS_CONNECTION(source_object), result, &error);
    bool active = error == NULL;

    if (error != NULL) {
        gchar *remote_error = g_dbus_error_get_remote_error(error);
        active = g_strcmp0(remote_error,
                           "org.bluez.Error.InProgress") == 0;
        if (!active && !request->monitor->disposed) {
            g_warning("Could not start LE discovery on %s: %s",
                      request->object_path, error->message);
        }
        g_free(remote_error);
        g_error_free(error);
    } else {
        g_message("Continuous LE discovery started for wear auto-connect");
    }

    if (reply != NULL)
        g_variant_unref(reply);
    finish_discovery_start(request, active);
    start_discovery_request_free(request);
}

static void on_set_discovery_filter_finished(GObject *source_object,
                                             GAsyncResult *result,
                                             gpointer user_data)
{
    StartDiscoveryRequest *request = user_data;
    BluezMonitor *monitor = request->monitor;
    GError *error = NULL;
    GVariant *reply = g_dbus_connection_call_finish(
        G_DBUS_CONNECTION(source_object), result, &error);

    if (reply != NULL)
        g_variant_unref(reply);
    if (error != NULL) {
        if (!monitor->disposed) {
            g_warning("Could not set LE discovery filter on %s: %s",
                      request->object_path, error->message);
        }
        g_error_free(error);
        finish_discovery_start(request, false);
        start_discovery_request_free(request);
        return;
    }

    if (!discovery_should_run(monitor) ||
        !adapter_is_powered(monitor, request->object_path)) {
        finish_discovery_start(request, false);
        start_discovery_request_free(request);
        return;
    }

    g_dbus_connection_call(
            monitor->connection,
            BLUEZ_SERVICE,
            request->object_path,
            BLUEZ_ADAPTER_INTERFACE,
            "StartDiscovery",
            NULL,
            G_VARIANT_TYPE("()"),
            G_DBUS_CALL_FLAGS_NONE,
            BLUEZ_CALL_TIMEOUT_MSEC,
            NULL,
            on_start_discovery_finished,
            request);
}

static void request_start_discovery(BluezMonitor *monitor,
                                    const char *object_path)
{
    if (discovery_adapter_is_owned(monitor, object_path) ||
        g_hash_table_contains(monitor->discovery_start_in_flight,
                              object_path)) {
        return;
    }

    g_hash_table_add(monitor->discovery_start_in_flight,
                     g_strdup(object_path));
    StartDiscoveryRequest *request = g_new0(StartDiscoveryRequest, 1);
    request->monitor = bluez_monitor_ref(monitor);
    request->object_path = g_strdup(object_path);

    GVariantBuilder filter;
    g_variant_builder_init(&filter, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&filter, "{sv}", "Transport",
                          g_variant_new_string("le"));
    g_variant_builder_add(&filter, "{sv}", "DuplicateData",
                          g_variant_new_boolean(TRUE));
    g_variant_builder_add(&filter, "{sv}", "RSSI",
                          g_variant_new_int16(AUTO_CONNECT_MIN_RSSI_DBM));

    g_dbus_connection_call(
        monitor->connection,
        BLUEZ_SERVICE,
        object_path,
        BLUEZ_ADAPTER_INTERFACE,
        "SetDiscoveryFilter",
        g_variant_new("(a{sv})", &filter),
        G_VARIANT_TYPE("()"),
        G_DBUS_CALL_FLAGS_NONE,
        BLUEZ_CALL_TIMEOUT_MSEC,
        NULL,
        on_set_discovery_filter_finished,
        request);
}

static void start_le_discovery(BluezMonitor *monitor)
{
    if (!discovery_should_run(monitor))
        return;

    bool found_powered_adapter = false;
    GHashTableIter iter;
    gpointer object_path = NULL;
    gpointer encoded_power = NULL;
    g_hash_table_iter_init(&iter, monitor->adapter_power_states);
    while (g_hash_table_iter_next(&iter, &object_path, &encoded_power)) {
        if (GPOINTER_TO_INT(encoded_power) != 2)
            continue;
        found_powered_adapter = true;
        request_start_discovery(monitor, object_path);
    }

    if (g_hash_table_size(monitor->adapter_power_states) == 0) {
        request_managed_objects(monitor);
        return;
    }

    if (monitor->discovery_adapters->len > 0 ||
        g_hash_table_size(monitor->discovery_start_in_flight) > 0 ||
        !found_powered_adapter) {
        cancel_discovery_retry(monitor);
    } else {
        schedule_discovery_retry(monitor);
    }
}

typedef struct {
    BluezMonitor *monitor;
    char *object_path;
    guint attempts;
} StopDiscoveryRequest;

static void stop_discovery_request_free(StopDiscoveryRequest *request)
{
    bluez_monitor_unref(request->monitor);
    g_free(request->object_path);
    g_free(request);
}

static StopDiscoveryRequest *stop_discovery_request_copy(
    const StopDiscoveryRequest *request)
{
    StopDiscoveryRequest *copy = g_new0(StopDiscoveryRequest, 1);
    copy->monitor = bluez_monitor_ref(request->monitor);
    copy->object_path = g_strdup(request->object_path);
    copy->attempts = request->attempts;
    return copy;
}

static bool stop_discovery_error_is_terminal(const GError *error)
{
    if (error == NULL)
        return false;

    gchar *remote_error = g_dbus_error_get_remote_error(error);
    bool terminal =
        g_strcmp0(remote_error, "org.bluez.Error.NotReady") == 0 ||
        g_strcmp0(remote_error,
                  "org.freedesktop.DBus.Error.UnknownObject") == 0 ||
        g_strcmp0(remote_error,
                  "org.freedesktop.DBus.Error.ServiceUnknown") == 0 ||
        g_strcmp0(remote_error,
                  "org.freedesktop.DBus.Error.NameHasNoOwner") == 0;
    g_free(remote_error);
    return terminal;
}

static void send_stop_discovery_request(StopDiscoveryRequest *request);

static gboolean retry_stop_discovery(gpointer user_data)
{
    StopDiscoveryRequest *request = user_data;
    send_stop_discovery_request(stop_discovery_request_copy(request));
    return G_SOURCE_REMOVE;
}

static void finish_stop_discovery(StopDiscoveryRequest *request,
                                  bool session_is_gone)
{
    BluezMonitor *monitor = request->monitor;
    g_hash_table_remove(monitor->stop_in_flight, request->object_path);
    if (session_is_gone)
        forget_discovery_adapter(monitor, request->object_path);

    /* Connected=false can race the asynchronous StopDiscovery reply. Once the
     * session is gone (or stale ownership is discarded after bounded retry),
     * let StartDiscovery re-evaluate the adapter and re-adopt InProgress. */
    if (session_is_gone && !monitor->disposed &&
        monitor->monitoring_started &&
        monitor->auto_connect_on_wear &&
        g_hash_table_size(monitor->known_devices) == 0) {
        start_le_discovery(monitor);
    }
}

static void on_stop_discovery_finished(GObject *source_object,
                                       GAsyncResult *result,
                                       gpointer user_data)
{
    StopDiscoveryRequest *request = user_data;
    BluezMonitor *monitor = request->monitor;
    GError *error = NULL;
    GVariant *reply = g_dbus_connection_call_finish(
        G_DBUS_CONNECTION(source_object), result, &error);

    if (error == NULL) {
        if (reply != NULL)
            g_variant_unref(reply);
        finish_stop_discovery(request, true);
        stop_discovery_request_free(request);
        return;
    }

    if (stop_discovery_error_is_terminal(error)) {
        g_debug("Owned LE discovery session is already gone on %s: %s",
                request->object_path, error->message);
        finish_stop_discovery(request, true);
        g_error_free(error);
        stop_discovery_request_free(request);
        return;
    }

    request->attempts++;
    guint max_attempts = monitor->disposed
                             ? STOP_DISCOVERY_DISPOSE_ATTEMPTS
                             : STOP_DISCOVERY_MAX_ATTEMPTS;
    if (request->attempts >= max_attempts) {
        if (monitor->disposed) {
            g_debug("Giving up LE discovery cleanup on %s during shutdown: %s",
                    request->object_path, error->message);
        } else {
            g_warning("Giving up LE discovery stop on %s after %u attempts; re-evaluating adapter state: %s",
                      request->object_path, request->attempts,
                      error->message);
        }
        finish_stop_discovery(request, !monitor->disposed);
        g_error_free(error);
        stop_discovery_request_free(request);
        return;
    }

    if (request->attempts == 1) {
        g_warning("Could not stop owned LE discovery on %s; retrying: %s",
                  request->object_path, error->message);
    } else {
        g_debug("Retrying LE discovery stop on %s after: %s",
                request->object_path, error->message);
    }
    g_error_free(error);

    /* Transfer this request reference to the timeout source. Its destroy
     * notify releases the monitor even if the source is cancelled. */
    guint retry_shift = MIN(request->attempts - 1, 4u);
    guint retry_delay = STOP_DISCOVERY_RETRY_MSEC << retry_shift;
    g_timeout_add_full(G_PRIORITY_DEFAULT,
                       retry_delay,
                       retry_stop_discovery,
                       request,
                       (GDestroyNotify)stop_discovery_request_free);
}

static void send_stop_discovery_request(StopDiscoveryRequest *request)
{
    g_dbus_connection_call(
        request->monitor->connection,
        BLUEZ_SERVICE,
        request->object_path,
        BLUEZ_ADAPTER_INTERFACE,
        "StopDiscovery",
        NULL,
        G_VARIANT_TYPE("()"),
        G_DBUS_CALL_FLAGS_NONE,
        BLUEZ_CALL_TIMEOUT_MSEC,
        NULL,
        on_stop_discovery_finished,
        request);
}

static void request_stop_discovery(BluezMonitor *monitor,
                                   const char *object_path)
{
    if (g_hash_table_contains(monitor->stop_in_flight, object_path))
        return;

    g_hash_table_add(monitor->stop_in_flight, g_strdup(object_path));
    StopDiscoveryRequest *request = g_new0(StopDiscoveryRequest, 1);
    request->monitor = bluez_monitor_ref(monitor);
    request->object_path = g_strdup(object_path);
    send_stop_discovery_request(request);
}

static void stop_le_discovery(BluezMonitor *monitor)
{
    /* Keep ownership paths until BlueZ confirms that each session ended. This
     * prevents a failed asynchronous stop from being mistaken for success and
     * permits a safe retry without duplicate requests. */
    for (guint i = 0; i < monitor->discovery_adapters->len; i++) {
        const char *object_path = g_ptr_array_index(
            monitor->discovery_adapters, i);
        request_stop_discovery(monitor, object_path);
    }
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
    monitor->ref_count = 1;
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
    monitor->auto_connect_state_models = g_hash_table_new_full(
        g_str_hash, g_str_equal, g_free, NULL
    );
    monitor->auto_connect_model_attempts = g_hash_table_new_full(
        g_direct_hash, g_direct_equal, NULL, g_free
    );
    monitor->handoff_suppressions = g_hash_table_new_full(
        g_direct_hash, g_direct_equal, NULL, g_free
    );
    monitor->connect_in_flight = g_hash_table_new_full(
        g_str_hash, g_str_equal, g_free, NULL
    );
    monitor->disconnect_in_flight = g_hash_table_new_full(
        g_str_hash, g_str_equal, g_free, NULL
    );
    monitor->device_resolve_in_flight = g_hash_table_new_full(
        g_str_hash, g_str_equal, g_free, NULL
    );
    monitor->discovery_start_in_flight = g_hash_table_new_full(
        g_str_hash, g_str_equal, g_free, NULL
    );
    monitor->stop_in_flight = g_hash_table_new_full(
        g_str_hash, g_str_equal, g_free, NULL
    );
    monitor->adapter_power_states = g_hash_table_new_full(
        g_str_hash, g_str_equal, g_free, NULL
    );
    monitor->advertisement_rssi = g_hash_table_new_full(
        g_str_hash, g_str_equal, g_free, g_free
    );
    monitor->discovery_adapters = g_ptr_array_new_with_free_func(g_free);

    return monitor;
}

static void bluez_monitor_unref(BluezMonitor *monitor)
{
    if (!g_atomic_int_dec_and_test(&monitor->ref_count))
        return;

    g_hash_table_destroy(monitor->known_devices);
    g_hash_table_destroy(monitor->paired_devices);
    g_hash_table_destroy(monitor->auto_connect_states);
    g_hash_table_destroy(monitor->auto_connect_state_models);
    g_hash_table_destroy(monitor->auto_connect_model_attempts);
    g_hash_table_destroy(monitor->handoff_suppressions);
    g_hash_table_unref(monitor->connect_in_flight);
    g_hash_table_unref(monitor->disconnect_in_flight);
    g_hash_table_unref(monitor->device_resolve_in_flight);
    g_hash_table_unref(monitor->discovery_start_in_flight);
    g_hash_table_unref(monitor->stop_in_flight);
    g_hash_table_destroy(monitor->adapter_power_states);
    g_hash_table_destroy(monitor->advertisement_rssi);
    g_ptr_array_free(monitor->discovery_adapters, TRUE);
    g_object_unref(monitor->connection);
    g_free(monitor);
}

void bluez_monitor_free(BluezMonitor *monitor)
{
    if (monitor == NULL || monitor->disposed)
        return;

    monitor->disposed = true;
    bluez_monitor_stop(monitor);
    monitor->connected_callback = NULL;
    monitor->disconnected_callback = NULL;
    bluez_monitor_unref(monitor);
}

bool bluez_monitor_start(BluezMonitor *monitor)
{
    if (monitor == NULL || monitor->disposed)
        return false;
    if (monitor->monitoring_started)
        return true;

    monitor->monitoring_started = true;

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
    if (monitor == NULL || !monitor->monitoring_started)
        return;

    monitor->monitoring_started = false;
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

bool bluez_monitor_suppress_auto_connect_until_unworn(
    BluezMonitor *monitor,
    const char *selector)
{
    if (monitor == NULL || selector == NULL)
        return false;

    BluezDeviceInfo *matched = find_cached_target(monitor, selector, false);
    if (matched == NULL || matched->product_id == 0) {
        bluez_device_info_free(matched);
        return false;
    }

    uint16_t ble_model = ble_airpods_bluez_product_id(matched->product_id);
    gpointer model_key = GUINT_TO_POINTER((guint)ble_model);
    BleHandoffSuppressionState *state = g_new0(
        BleHandoffSuppressionState, 1);
    ble_handoff_suppression_activate(state);
    g_hash_table_replace(monitor->handoff_suppressions, model_key, state);
    g_debug("Suppressed wear auto-connect until removal for BLE model 0x%04x",
            ble_model);
    bluez_device_info_free(matched);
    return true;
}

bool bluez_monitor_rearm_auto_connect_after_removal(
    BluezMonitor *monitor,
    const char *selector)
{
    if (monitor == NULL || selector == NULL || selector[0] == '\0')
        return false;

    BluezDeviceInfo *matched = find_cached_target(monitor, selector, false);
    bool rearmed = matched != NULL && rearm_auto_connect_model(
        monitor->auto_connect_states,
        monitor->auto_connect_state_models,
        monitor->auto_connect_model_attempts,
        monitor->handoff_suppressions,
        matched->product_id);
    if (rearmed) {
        g_debug("Rearmed wear auto-connect after confirmed removal of %s",
                matched->name != NULL ? matched->name : "AirPods");
    }
    bluez_device_info_free(matched);
    return rearmed;
}

static void on_managed_objects_finished(GObject *source_object,
                                        GAsyncResult *async_result,
                                        gpointer user_data)
{
    BluezMonitor *monitor = user_data;
    monitor->managed_objects_in_flight = false;
    GError *error = NULL;
    GVariant *result = g_dbus_connection_call_finish(
        G_DBUS_CONNECTION(source_object), async_result, &error);
    if (error != NULL) {
        if (!monitor->disposed) {
            g_warning("Failed to get BlueZ managed objects: %s",
                      error->message);
        }
        g_error_free(error);
        schedule_discovery_retry(monitor);
        bluez_monitor_unref(monitor);
        return;
    }

    if (monitor->disposed || !monitor->monitoring_started) {
        g_variant_unref(result);
        bluez_monitor_unref(monitor);
        return;
    }

    GVariant *objects = NULL;
    g_variant_get(result, "(@a{oa{sa{sv}}})", &objects);

    GVariantIter iter;
    const gchar *object_path;
    GVariant *interfaces;

    g_variant_iter_init(&iter, objects);
    while (g_variant_iter_next(&iter, "{&o@a{sa{sv}}}", &object_path, &interfaces)) {
        GVariant *adapter_properties = g_variant_lookup_value(
            interfaces, BLUEZ_ADAPTER_INTERFACE, G_VARIANT_TYPE("a{sv}"));
        if (adapter_properties != NULL) {
            GVariant *powered = g_variant_lookup_value(
                adapter_properties, "Powered", G_VARIANT_TYPE_BOOLEAN);
            remember_adapter_power(monitor, object_path,
                                   powered == NULL ||
                                       g_variant_get_boolean(powered));
            if (powered != NULL)
                g_variant_unref(powered);
            g_variant_unref(adapter_properties);
        }

        GVariant *device_properties = g_variant_lookup_value(
            interfaces, BLUEZ_DEVICE_INTERFACE, G_VARIANT_TYPE("a{sv}"));
        if (device_properties != NULL) {
            BluezDeviceInfo *info = device_info_from_properties(
                object_path, device_properties);
            /* Random advertisement objects are neither paired nor connected
             * and normally have no UUID cache. GetManagedObjects already
             * supplied the full dictionary, so never issue a nested blocking
             * UUID lookup from this completion callback. */
            if (info != NULL && (info->paired || info->connected) &&
                verify_airpods_device(monitor, object_path,
                                      device_properties)) {
                cache_paired_device(monitor, object_path, info, true);
                if (info && info->connected) {
                    bool already_known = g_hash_table_contains(
                        monitor->known_devices, object_path);
                    g_message("Found already connected AirPods: %s (%s)",
                              info->name ? info->name : "Unknown",
                              info->address ? info->address : "Unknown");

                    g_hash_table_insert(monitor->known_devices,
                                        g_strdup(object_path),
                                        bluez_device_info_copy(info));

                    if (!already_known && monitor->connected_callback) {
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

    if (g_hash_table_size(monitor->known_devices) == 0) {
        if (g_hash_table_size(monitor->adapter_power_states) > 0)
            start_le_discovery(monitor);
        else
            schedule_discovery_retry(monitor);
    }

    bluez_monitor_unref(monitor);
}

static void request_managed_objects(BluezMonitor *monitor)
{
    if (monitor == NULL || monitor->disposed ||
        !monitor->monitoring_started || monitor->managed_objects_in_flight) {
        return;
    }

    monitor->managed_objects_in_flight = true;
    g_dbus_connection_call(
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
        on_managed_objects_finished,
        bluez_monitor_ref(monitor));
}

void bluez_monitor_check_existing_devices(BluezMonitor *monitor)
{
    request_managed_objects(monitor);
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
