/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "ble_autoconnect.h"
#include "airpods_state.h"

/* Apple Continuity Proximity Pairing message. */
#define APPLE_PROXIMITY_TYPE 0x07
#define APPLE_PROXIMITY_LENGTH 25
#define APPLE_PROXIMITY_PREFIX 0x01

bool ble_airpods_model_supports_wear_autoconnect(uint16_t model)
{
    switch ((AirPodsModel)model) {
    case AIRPODS_MODEL_4:
    case AIRPODS_MODEL_4_ANC:
    case AIRPODS_MODEL_MAX_2:
        return true;
    case AIRPODS_MODEL_UNKNOWN:
    default:
        return false;
    }
}

static bool model_status_is_worn(uint16_t model, uint8_t status)
{
    /* Bits 1 and 3 are the public per-side wear flags on the supported
     * models. Either side is enough: AirPods 4 must support one-bud use, and
     * Max 2 may report only one cup bit on some hosts. Case/status bits are
     * intentionally ignored. */
    (void)model;
    return (status & ((1u << 1) | (1u << 3))) != 0;
}

bool ble_airpods_parse_manufacturer_data(const uint8_t *data,
                                         size_t len,
                                         BleAirPodsAdvertisement *advertisement)
{
    if (data == NULL || advertisement == NULL)
        return false;

    size_t offset = 0;
    while (offset + 2 <= len) {
        uint8_t type = data[offset];
        size_t message_len = data[offset + 1];
        size_t payload_offset = offset + 2;

        if (message_len > len - payload_offset)
            return false;

        if (type == APPLE_PROXIMITY_TYPE &&
            message_len == APPLE_PROXIMITY_LENGTH) {
            const uint8_t *payload = data + payload_offset;
            if (payload[0] != APPLE_PROXIMITY_PREFIX)
                return false;

            uint16_t model = ((uint16_t)payload[1] << 8) | payload[2];
            if (!ble_airpods_model_supports_wear_autoconnect(model))
                return false;

            advertisement->model = model;
            advertisement->status = payload[3];
            advertisement->worn = model_status_is_worn(model, payload[3]);
            return true;
        }

        offset = payload_offset + message_len;
    }

    return false;
}

uint16_t ble_airpods_bluez_product_id(uint16_t ble_model)
{
    return (uint16_t)((ble_model << 8) | (ble_model >> 8));
}

bool ble_autoconnect_observe(BleAutoConnectState *state,
                             bool worn,
                             bool confirm_first_worn,
                             int64_t now_usec,
                             int64_t confirmation_window_usec,
                             int64_t cooldown_usec)
{
    if (state == NULL || confirmation_window_usec < 0 || cooldown_usec < 0)
        return false;

    if (!worn) {
        state->has_observed_unworn = true;
        state->worn_sequence_active = false;
        state->worn_observations = 0;
        state->first_worn_observation_usec = 0;
        state->sequence_consumed = false;
        return false;
    }

    if (!state->has_observed_unworn)
        return false;

    /* A successful attempt consumes the entire wear edge. Do not turn a
     * continuously-worn advertisement into a new edge merely because the
     * confirmation window elapsed; that can reconnect Linux after an iPhone
     * handoff and steal AirPods 4 back. Only an explicit unworn observation
     * above may re-arm the state machine. */
    if (state->sequence_consumed)
        return false;

    bool outside_window = state->worn_sequence_active &&
        (now_usec < state->first_worn_observation_usec ||
         now_usec - state->first_worn_observation_usec > confirmation_window_usec);

    if (!state->worn_sequence_active || outside_window) {
        state->worn_sequence_active = true;
        state->worn_observations = 1;
        state->first_worn_observation_usec = now_usec;
        state->sequence_consumed = false;
        if (!confirm_first_worn)
            return false;
    }

    if (state->worn_observations < 2)
        state->worn_observations++;

    if (state->worn_observations < 2 || state->sequence_consumed)
        return false;

    state->sequence_consumed = true;

    if (state->has_attempted &&
        (now_usec < state->last_attempt_usec ||
         now_usec - state->last_attempt_usec < cooldown_usec)) {
        return false;
    }

    state->has_attempted = true;
    state->last_attempt_usec = now_usec;
    return true;
}

void ble_handoff_suppression_activate(BleHandoffSuppressionState *state)
{
    if (state == NULL)
        return;

    state->active = true;
    state->first_unworn_observation_usec = 0;
}

bool ble_handoff_suppression_observe(BleHandoffSuppressionState *state,
                                     bool worn,
                                     int64_t now_usec,
                                     int64_t unworn_rearm_usec)
{
    if (state == NULL || !state->active || unworn_rearm_usec < 0)
        return false;

    if (worn) {
        state->first_unworn_observation_usec = 0;
        return true;
    }

    if (state->first_unworn_observation_usec == 0 ||
        now_usec < state->first_unworn_observation_usec) {
        state->first_unworn_observation_usec = now_usec;
        return true;
    }

    if (now_usec - state->first_unworn_observation_usec <
        unworn_rearm_usec) {
        return true;
    }

    state->active = false;
    state->first_unworn_observation_usec = 0;
    return false;
}
