/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Small, side-effect-free helpers for AirPods BLE wear auto-connect.
 */

#ifndef BLE_AUTOCONNECT_H
#define BLE_AUTOCONNECT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint16_t model;
    uint8_t status;
    bool worn;
} BleAirPodsAdvertisement;

typedef struct {
    bool has_observed_unworn;
    bool worn_sequence_active;
    unsigned int worn_observations;
    int64_t first_worn_observation_usec;
    bool sequence_consumed;
    bool has_attempted;
    int64_t last_attempt_usec;
} BleAutoConnectState;

typedef struct {
    bool active;
    int64_t first_unworn_observation_usec;
} BleHandoffSuppressionState;

/* Parse Apple company (0x004c) ManufacturerData. The company identifier is
 * not part of data: BlueZ uses it as the a{qv} dictionary key. */
bool ble_airpods_parse_manufacturer_data(const uint8_t *data,
                                         size_t len,
                                         BleAirPodsAdvertisement *advertisement);

/* BlueZ Modalias uses the USB-style product byte order (for example,
 * BLE 0x2d20 is bluetooth:v004Cp202D...). */
uint16_t ble_airpods_bluez_product_id(uint16_t ble_model);

/* True only for models whose disconnected BLE wear bits are implemented.
 * Removal-disconnect must use the same gate or older models could disconnect
 * without any supported wear path to bring them back. */
bool ble_airpods_model_supports_wear_autoconnect(uint16_t model);

/* Require an unworn baseline followed by two worn advertisements inside
 * confirmation_window_usec. When confirm_first_worn is true, the first worn
 * advertisement is sufficient; callers use that only for a very strong RSSI.
 * Binding the edge to one advertising Device1 reduces false triggers from
 * unrelated same-model AirPods. A continuous worn stream is consumed only
 * once; another unworn sample rearms it. */
bool ble_autoconnect_observe(BleAutoConnectState *state,
                             bool worn,
                             bool confirm_first_worn,
                             int64_t now_usec,
                             int64_t confirmation_window_usec,
                             int64_t cooldown_usec);

/* Keep Linux from reconnecting a continuously worn device after yielding to
 * another Apple host. A stable unworn interval clears the suppression, and
 * the ordinary unworn -> worn edge detector then handles the next use. */
void ble_handoff_suppression_activate(BleHandoffSuppressionState *state);
bool ble_handoff_suppression_observe(BleHandoffSuppressionState *state,
                                     bool worn,
                                     int64_t now_usec,
                                     int64_t unworn_rearm_usec);

#endif /* BLE_AUTOCONNECT_H */
