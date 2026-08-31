/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2024 EarPort Contributors
 *
 * BlueZ D-Bus monitoring for AirPods detection
 */

#ifndef BLUEZ_MONITOR_H
#define BLUEZ_MONITOR_H

#include <glib.h>
#include <gio/gio.h>
#include <stdbool.h>
#include <stdint.h>

/* BlueZ D-Bus constants */
#define BLUEZ_SERVICE           "org.bluez"
#define BLUEZ_ADAPTER_INTERFACE "org.bluez.Adapter1"
#define BLUEZ_DEVICE_INTERFACE  "org.bluez.Device1"
#define DBUS_PROPERTIES_INTERFACE "org.freedesktop.DBus.Properties"
#define DBUS_OBJECT_MANAGER_INTERFACE "org.freedesktop.DBus.ObjectManager"

/* Device info from BlueZ */
typedef struct {
    char *address;
    char *name;
    char *object_path;
    bool connected;
    bool paired;
    uint16_t vendor_id;
    uint16_t product_id;
} BluezDeviceInfo;

/* Callback types */
typedef void (*BluezDeviceCallback)(const BluezDeviceInfo *device, void *user_data);

/* Disconnect calls are not cancellable once BlueZ accepts them. The retry
 * predicate is consulted only after an error and again immediately before a
 * retry, so rewear can stop future attempts without disturbing an accepted
 * request. The finished callback reports the final outcome. */
typedef bool (*BluezDisconnectRetryCheck)(const char *device_address,
                                          void *user_data);
typedef void (*BluezDisconnectFinishedCallback)(const char *device_address,
                                                 bool completed,
                                                 void *user_data);

/* BlueZ monitor context */
typedef struct BluezMonitor BluezMonitor;

/**
 * Create a new BlueZ monitor
 *
 * @return New monitor or NULL on error
 */
BluezMonitor *bluez_monitor_new(void);

/**
 * Free BlueZ monitor
 */
void bluez_monitor_free(BluezMonitor *monitor);

/**
 * Start monitoring for AirPods devices
 *
 * @param monitor Monitor context
 * @return true on success
 */
bool bluez_monitor_start(BluezMonitor *monitor);

/**
 * Stop monitoring
 */
void bluez_monitor_stop(BluezMonitor *monitor);

/**
 * Set callback for device connected events
 */
void bluez_monitor_set_connected_callback(BluezMonitor *monitor,
                                           BluezDeviceCallback callback,
                                           void *user_data);

/**
 * Set callback for device disconnected events
 */
void bluez_monitor_set_disconnected_callback(BluezMonitor *monitor,
                                              BluezDeviceCallback callback,
                                              void *user_data);

/** Enable continuous LE discovery and connect-on-confirmed-wear handling. */
void bluez_monitor_set_auto_connect_on_wear(BluezMonitor *monitor, bool enabled);

/* Suppress wear auto-connect for the selected paired AirPods after Linux has
 * yielded to another Apple host. A stable BLE unworn observation rearms it. */
bool bluez_monitor_suppress_auto_connect_until_unworn(
    BluezMonitor *monitor,
    const char *selector);

/* Clear the retry cooldown and remembered BLE edge for a removed AirPods
 * device. The next connection still requires a fresh unworn -> worn edge. */
bool bluez_monitor_rearm_auto_connect_after_removal(
    BluezMonitor *monitor,
    const char *selector);

/**
 * Asynchronously disconnect a currently connected, cached AirPods device.
 * selector may be either its Bluetooth address or its BlueZ object path; an
 * arbitrary path is never called unless it matches a paired/known AirPods.
 * Returns true when a request was queued or is already in flight.
 */
bool bluez_monitor_disconnect_device(BluezMonitor *monitor,
                                     const char *selector,
                                     BluezDisconnectRetryCheck retry_check,
                                     BluezDisconnectFinishedCallback callback,
                                     void *user_data);

/* Asynchronously connect an exact paired, currently disconnected AirPods.
 * Used when a device is reworn while an uncancellable removal Disconnect call
 * is still completing. */
bool bluez_monitor_connect_device(BluezMonitor *monitor,
                                  const char *selector);

/**
 * Check for already connected AirPods devices
 * Will trigger connected callback for each found device
 */
void bluez_monitor_check_existing_devices(BluezMonitor *monitor);

/**
 * Return a copy of any currently connected AirPods other than exclude_address.
 * The caller owns the returned value. Returns NULL when no fallback exists.
 */
BluezDeviceInfo *bluez_monitor_find_connected_device(BluezMonitor *monitor,
                                                      const char *exclude_address);

/**
 * Free device info structure
 */
void bluez_device_info_free(BluezDeviceInfo *info);

/**
 * Copy device info structure
 */
BluezDeviceInfo *bluez_device_info_copy(const BluezDeviceInfo *info);

#endif /* BLUEZ_MONITOR_H */
