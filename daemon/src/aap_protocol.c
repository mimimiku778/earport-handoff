/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2024 EarPort Contributors
 */

#include "aap_protocol.h"
#include <glib.h>
#include <string.h>
#include <stdio.h>

/* Pre-built packets */
const uint8_t AAP_PKT_HANDSHAKE[AAP_HANDSHAKE_SIZE] = {
    0x00, 0x00, 0x04, 0x00, 0x01, 0x00, 0x02, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

const uint8_t AAP_PKT_REQUEST_NOTIFICATIONS[AAP_REQUEST_NOTIF_SIZE] = {
    0x04, 0x00, 0x04, 0x00, 0x0F, 0x00, 0xFF, 0xFF, 0xFF, 0xFF
};

const uint8_t AAP_PKT_REQUEST_EAR_DETECTION[AAP_EAR_DETECTION_REQUEST_SIZE] = {
    0x04, 0x00, 0x04, 0x00, AAP_OPCODE_EAR_DETECTION_REQUEST, 0x00
};

const uint8_t AAP_PKT_SET_FEATURES[AAP_SET_FEATURES_SIZE] = {
    0x04, 0x00, 0x04, 0x00, 0x4D, 0x00, 0xFF, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

/* LibrePods names these the handshake and features ACK prefixes.  The first
 * deliberately has a non-standard AAP header; the latter is the device's
 * readiness response after feature negotiation. */
static const uint8_t AAP_HANDSHAKE_ACK_PREFIX[] = {
    0x01, 0x00, 0x04, 0x00
};

static const uint8_t AAP_FEATURES_ACK_PREFIX[] = {
    0x04, 0x00, 0x04, 0x00, 0x2B, 0x00
};

static bool packet_has_prefix(const uint8_t *data,
                              size_t len,
                              const uint8_t *prefix,
                              size_t prefix_len)
{
    return data != NULL && len >= prefix_len &&
           memcmp(data, prefix, prefix_len) == 0;
}

void aap_init_state_reset(AapInitState *state)
{
    if (state != NULL)
        memset(state, 0, sizeof(*state));
}

AapInitAction aap_init_next_action(const AapInitState *state,
                                   const uint8_t *data,
                                   size_t len)
{
    if (state == NULL)
        return AAP_INIT_ACTION_NONE;

    if (packet_has_prefix(data, len,
                          AAP_HANDSHAKE_ACK_PREFIX,
                          sizeof(AAP_HANDSHAKE_ACK_PREFIX))) {
        return state->features_sent ? AAP_INIT_ACTION_NONE
                                    : AAP_INIT_ACTION_SEND_FEATURES;
    }

    if (packet_has_prefix(data, len,
                          AAP_FEATURES_ACK_PREFIX,
                          sizeof(AAP_FEATURES_ACK_PREFIX))) {
        return state->notifications_requested
                   ? AAP_INIT_ACTION_NONE
                   : AAP_INIT_ACTION_REQUEST_NOTIFICATIONS;
    }

    return AAP_INIT_ACTION_NONE;
}

void aap_init_mark_action_sent(AapInitState *state, AapInitAction action)
{
    if (state == NULL)
        return;

    switch (action) {
    case AAP_INIT_ACTION_SEND_FEATURES:
        state->features_sent = true;
        break;
    case AAP_INIT_ACTION_REQUEST_NOTIFICATIONS:
        state->notifications_requested = true;
        break;
    case AAP_INIT_ACTION_NONE:
    default:
        break;
    }
}

const uint8_t AAP_PKT_NC_OFF[AAP_CONTROL_CMD_SIZE] = {
    0x04, 0x00, 0x04, 0x00, 0x09, 0x00, 0x0D, 0x01, 0x00, 0x00, 0x00
};

const uint8_t AAP_PKT_NC_ANC[AAP_CONTROL_CMD_SIZE] = {
    0x04, 0x00, 0x04, 0x00, 0x09, 0x00, 0x0D, 0x02, 0x00, 0x00, 0x00
};

const uint8_t AAP_PKT_NC_TRANSPARENCY[AAP_CONTROL_CMD_SIZE] = {
    0x04, 0x00, 0x04, 0x00, 0x09, 0x00, 0x0D, 0x03, 0x00, 0x00, 0x00
};

const uint8_t AAP_PKT_NC_ADAPTIVE[AAP_CONTROL_CMD_SIZE] = {
    0x04, 0x00, 0x04, 0x00, 0x09, 0x00, 0x0D, 0x04, 0x00, 0x00, 0x00
};

const uint8_t AAP_PKT_CA_ENABLE[AAP_CONTROL_CMD_SIZE] = {
    0x04, 0x00, 0x04, 0x00, 0x09, 0x00, 0x28, 0x01, 0x00, 0x00, 0x00
};

const uint8_t AAP_PKT_CA_DISABLE[AAP_CONTROL_CMD_SIZE] = {
    0x04, 0x00, 0x04, 0x00, 0x09, 0x00, 0x28, 0x02, 0x00, 0x00, 0x00
};

bool aap_has_valid_header(const uint8_t *data, size_t len)
{
    if (data == NULL || len < AAP_HEADER_SIZE)
        return false;

    return (data[0] == AAP_HEADER_BYTE0 &&
            data[1] == AAP_HEADER_BYTE1 &&
            data[2] == AAP_HEADER_BYTE2 &&
            data[3] == AAP_HEADER_BYTE3);
}

uint8_t aap_get_opcode(const uint8_t *data, size_t len)
{
    if (data == NULL || len < 5)
        return 0;
    return data[4];
}

AapParseResult aap_parse_battery(const uint8_t *data, size_t len, AapBatteryData *battery)
{
    if (len < AAP_MIN_BATTERY_SIZE)
        return AAP_PARSE_INCOMPLETE;

    /* Header: 04 00 04 00 04 00 [count] ... */
    if (data[4] != AAP_OPCODE_BATTERY || data[5] != 0x00)
        return AAP_PARSE_MALFORMED;

    uint8_t count = data[6];
    if (count == 0 || count > 3)
        return AAP_PARSE_MALFORMED;

    /* Verify we have enough data: header (7) + count * 5 bytes per component */
    size_t expected_len = 7 + (count * 5);
    if (len < expected_len)
        return AAP_PARSE_INCOMPLETE;

    /* Initialize to unavailable */
    battery->left_level = -1;
    battery->right_level = -1;
    battery->case_level = -1;
    battery->left_status = BATTERY_STATUS_UNKNOWN;
    battery->right_status = BATTERY_STATUS_UNKNOWN;
    battery->case_status = BATTERY_STATUS_UNKNOWN;

    /* Parse each component (5 bytes: component, spacer, level, status, end_marker) */
    for (uint8_t i = 0; i < count; i++) {
        size_t offset = 7 + (i * 5);
        uint8_t component = data[offset];
        uint8_t level = data[offset + 2];
        uint8_t status = data[offset + 3];

        BatteryStatus bat_status;
        switch (status) {
        case 0x01:
            bat_status = BATTERY_STATUS_CHARGING;
            break;
        case 0x02:
            bat_status = BATTERY_STATUS_DISCHARGING;
            break;
        case 0x04:
            bat_status = BATTERY_STATUS_DISCONNECTED;
            break;
        default:
            bat_status = BATTERY_STATUS_UNKNOWN;
            break;
        }

        switch (component) {
        case AAP_BATTERY_SINGLE:
            /* AirPods Max: single battery, store in left_level */
            battery->left_level = (level <= 100) ? (int8_t)level : -1;
            battery->left_status = bat_status;
            break;
        case AAP_BATTERY_LEFT:
            battery->left_level = (level <= 100) ? (int8_t)level : -1;
            battery->left_status = bat_status;
            break;
        case AAP_BATTERY_RIGHT:
            battery->right_level = (level <= 100) ? (int8_t)level : -1;
            battery->right_status = bat_status;
            break;
        case AAP_BATTERY_CASE:
            battery->case_level = (level <= 100) ? (int8_t)level : -1;
            battery->case_status = bat_status;
            break;
        }
    }

    return AAP_PARSE_OK;
}

static bool ear_status_is_valid(uint8_t status)
{
    return status == AAP_EAR_IN_EAR ||
           status == AAP_EAR_OUT ||
           status == AAP_EAR_IN_CASE ||
           status == AAP_EAR_DISCONNECTED;
}

static bool bounded_bytes_contain(const uint8_t *data,
                                  size_t len,
                                  const uint8_t *needle,
                                  size_t needle_len)
{
    if (data == NULL || needle == NULL || needle_len == 0 || len < needle_len)
        return false;

    for (size_t offset = 0; offset <= len - needle_len; offset++) {
        if (memcmp(data + offset, needle, needle_len) == 0)
            return true;
    }
    return false;
}

static AapParseResult parse_smart_routing_response(const uint8_t *data,
                                                   size_t len,
                                                   AapParsedPacket *result)
{
    static const uint8_t release_marker[] = "SetOwnershipToFalse";

    /* Smart-routing payloads contain binary/plist framing and need not be
     * NUL-terminated. LibrePods treats this bounded marker as an explicit
     * request from another Apple host for Linux to yield ownership. */
    if (len < 6)
        return AAP_PARSE_INCOMPLETE;

    if (bounded_bytes_contain(data + 6, len - 6,
                              release_marker,
                              sizeof(release_marker) - 1)) {
        result->type = AAP_PKT_TYPE_OWNERSHIP_RELEASE_REQUEST;
    }
    return AAP_PARSE_OK;
}

AapParseResult aap_parse_ear_detection(const uint8_t *data, size_t len, AapEarDetectionData *ear)
{
    /* Packet: 04 00 04 00 06 00 [primary] [secondary] */
    if (len < 8)
        return AAP_PARSE_INCOMPLETE;

    if (data[4] != AAP_OPCODE_EAR_DETECTION || data[5] != 0x00)
        return AAP_PARSE_MALFORMED;

    uint8_t primary_status = data[6];
    uint8_t secondary_status = data[7];

    if (!ear_status_is_valid(primary_status) ||
        !ear_status_is_valid(secondary_status))
        return AAP_PARSE_MALFORMED;

    ear->primary_in_ear = (primary_status == AAP_EAR_IN_EAR);
    ear->secondary_in_ear = (secondary_status == AAP_EAR_IN_EAR);

    return AAP_PARSE_OK;
}

AapParseResult aap_parse_audio_source(const uint8_t *data, size_t len, AapAudioSourceData *source)
{
    /* Packet: 04 00 04 00 0E [unknown] [6-byte address] [source type] */
    if (len < 13)
        return AAP_PARSE_INCOMPLETE;

    if (data[4] != AAP_OPCODE_AUDIO_SOURCE)
        return AAP_PARSE_MALFORMED;

    uint8_t source_type = data[12];
    if (source_type > AAP_AUDIO_SOURCE_MEDIA)
        return AAP_PARSE_MALFORMED;

    memcpy(source->device_address, data + 6, sizeof(source->device_address));
    source->type = (AapAudioSourceType)source_type;
    return AAP_PARSE_OK;
}

AapParseResult aap_parse_noise_control(const uint8_t *data, size_t len, NoiseControlMode *mode)
{
    /* Control response: 04 00 04 00 09 00 0D [mode] ... */
    if (len < 8)
        return AAP_PARSE_INCOMPLETE;

    if (data[4] != AAP_OPCODE_CONTROL || data[6] != AAP_CTRL_NOISE_CONTROL)
        return AAP_PARSE_MALFORMED;

    uint8_t mode_byte = data[7];
    switch (mode_byte) {
    case 0x01:
        *mode = NOISE_CONTROL_OFF;
        break;
    case 0x02:
        *mode = NOISE_CONTROL_ANC;
        break;
    case 0x03:
        *mode = NOISE_CONTROL_TRANSPARENCY;
        break;
    case 0x04:
        *mode = NOISE_CONTROL_ADAPTIVE;
        break;
    default:
        *mode = NOISE_CONTROL_OFF;
        break;
    }

    return AAP_PARSE_OK;
}

static bool aap_parse_metadata_string(const uint8_t *data,
                                      size_t len,
                                      size_t *position,
                                      char *destination,
                                      size_t destination_size)
{
    size_t output_position = 0;

    while (*position < len && data[*position] != '\0') {
        if (output_position + 1 < destination_size)
            destination[output_position++] = (char)data[*position];
        (*position)++;
    }

    destination[output_position] = '\0';
    if (*position >= len)
        return false;

    (*position)++;  /* Skip the field's NUL terminator. */
    return true;
}

static AapParseResult aap_parse_metadata(const uint8_t *data, size_t len, AapMetadata *metadata)
{
    /* Metadata packet: 04 00 04 00 1D 00 [6 bytes] [device_name\0] [model_number\0] [manufacturer\0] */
    if (len < 12)
        return AAP_PARSE_INCOMPLETE;

    memset(metadata, 0, sizeof(AapMetadata));

    /* Skip header (4) + opcode (1) + 00 (1) + 6 unknown bytes = position 12 */
    size_t pos = 12;

    /* Continue consuming a field after its destination buffer fills. This
     * preserves the boundaries of the following fields while safely
     * truncating unusually long values. */
    if (!aap_parse_metadata_string(data, len, &pos,
                                   metadata->device_name,
                                   sizeof(metadata->device_name)) ||
        !aap_parse_metadata_string(data, len, &pos,
                                   metadata->model_number,
                                   sizeof(metadata->model_number)) ||
        !aap_parse_metadata_string(data, len, &pos,
                                   metadata->manufacturer,
                                   sizeof(metadata->manufacturer))) {
        return AAP_PARSE_INCOMPLETE;
    }

    return AAP_PARSE_OK;
}

static AapParseResult parse_control_packet(const uint8_t *data, size_t len, AapParsedPacket *result)
{
    if (len < 8)
        return AAP_PARSE_INCOMPLETE;

    uint8_t ctrl_id = data[6];

    switch (ctrl_id) {
    case AAP_CTRL_NOISE_CONTROL:
        result->type = AAP_PKT_TYPE_NOISE_CONTROL;
        return aap_parse_noise_control(data, len, &result->data.noise_control);

    case AAP_CTRL_CONV_AWARENESS:
        result->type = AAP_PKT_TYPE_CONV_AWARENESS;
        result->data.conversational_awareness = (data[7] == 0x01);
        return AAP_PARSE_OK;

    case AAP_CTRL_LISTENING_MODES:
        result->type = AAP_PKT_TYPE_LISTENING_MODES;
        {
            uint8_t modes = data[7];
            result->data.listening_modes.raw_value = modes;
            result->data.listening_modes.off_enabled = (modes & AAP_LISTENING_MODE_OFF) != 0;
            result->data.listening_modes.transparency_enabled = (modes & AAP_LISTENING_MODE_TRANSPARENCY) != 0;
            result->data.listening_modes.anc_enabled = (modes & AAP_LISTENING_MODE_ANC) != 0;
            result->data.listening_modes.adaptive_enabled = (modes & AAP_LISTENING_MODE_ADAPTIVE) != 0;
        }
        return AAP_PARSE_OK;

    default:
        result->type = AAP_PKT_TYPE_UNKNOWN;
        return AAP_PARSE_OK;  /* Not an error, just unhandled */
    }
}

AapParseResult aap_parse_packet(const uint8_t *data, size_t len, AapParsedPacket *result)
{
    if (!aap_has_valid_header(data, len))
        return AAP_PARSE_INVALID_HEADER;

    if (len < 5)
        return AAP_PARSE_INCOMPLETE;

    uint8_t opcode = data[4];
    result->type = AAP_PKT_TYPE_UNKNOWN;

    switch (opcode) {
    case AAP_OPCODE_BATTERY:
        result->type = AAP_PKT_TYPE_BATTERY;
        return aap_parse_battery(data, len, &result->data.battery);

    case AAP_OPCODE_EAR_DETECTION:
        result->type = AAP_PKT_TYPE_EAR_DETECTION;
        return aap_parse_ear_detection(data, len, &result->data.ear_detection);

    case AAP_OPCODE_AUDIO_SOURCE:
        result->type = AAP_PKT_TYPE_AUDIO_SOURCE;
        return aap_parse_audio_source(data, len, &result->data.audio_source);

    case AAP_OPCODE_SMART_ROUTING_RESPONSE:
        return parse_smart_routing_response(data, len, result);

    case AAP_OPCODE_CONTROL:
        return parse_control_packet(data, len, result);

    case AAP_OPCODE_CA_DETECTION:
        result->type = AAP_PKT_TYPE_CA_DETECTION;
        /* Packet: 04 00 04 00 4B 00 02 00 01 [level] */
        if (len < 10)
            return AAP_PARSE_INCOMPLETE;
        result->data.ca_volume_level = data[9];
        return AAP_PARSE_OK;

    case AAP_OPCODE_METADATA:
        result->type = AAP_PKT_TYPE_METADATA;
        return aap_parse_metadata(data, len, &result->data.metadata);

    default:
        return AAP_PARSE_UNKNOWN_OPCODE;
    }
}

void aap_build_noise_control_cmd(NoiseControlMode mode, uint8_t *buffer)
{
    const uint8_t *src;
    switch (mode) {
    case NOISE_CONTROL_ANC:
        src = AAP_PKT_NC_ANC;
        break;
    case NOISE_CONTROL_TRANSPARENCY:
        src = AAP_PKT_NC_TRANSPARENCY;
        break;
    case NOISE_CONTROL_ADAPTIVE:
        src = AAP_PKT_NC_ADAPTIVE;
        break;
    default:
        src = AAP_PKT_NC_OFF;
        break;
    }
    memcpy(buffer, src, AAP_CONTROL_CMD_SIZE);
}

void aap_build_adaptive_level_cmd(int level, uint8_t *buffer)
{
    /* 04 00 04 00 09 00 2E [level] 00 00 00 */
    buffer[0] = 0x04;
    buffer[1] = 0x00;
    buffer[2] = 0x04;
    buffer[3] = 0x00;
    buffer[4] = 0x09;
    buffer[5] = 0x00;
    buffer[6] = AAP_CTRL_ADAPTIVE_LEVEL;
    buffer[7] = (uint8_t)(level < 0 ? 0 : (level > 100 ? 100 : level));
    buffer[8] = 0x00;
    buffer[9] = 0x00;
    buffer[10] = 0x00;
}

void aap_build_conv_awareness_cmd(bool enable, uint8_t *buffer)
{
    memcpy(buffer, enable ? AAP_PKT_CA_ENABLE : AAP_PKT_CA_DISABLE, AAP_CONTROL_CMD_SIZE);
}

void aap_build_owns_connection_cmd(bool claim, uint8_t *buffer)
{
    /* 04 00 04 00 09 00 06 [claim] 00 00 00 */
    buffer[0] = 0x04;
    buffer[1] = 0x00;
    buffer[2] = 0x04;
    buffer[3] = 0x00;
    buffer[4] = AAP_OPCODE_CONTROL;
    buffer[5] = 0x00;
    buffer[6] = AAP_CTRL_OWNS_CONNECTION;
    buffer[7] = claim ? 0x01 : 0x00;
    buffer[8] = 0x00;
    buffer[9] = 0x00;
    buffer[10] = 0x00;
}

void aap_build_listening_modes_cmd(uint8_t modes, uint8_t *buffer)
{
    /* 04 00 04 00 09 00 1A [modes] 00 00 00 */
    buffer[0] = 0x04;
    buffer[1] = 0x00;
    buffer[2] = 0x04;
    buffer[3] = 0x00;
    buffer[4] = 0x09;
    buffer[5] = 0x00;
    buffer[6] = AAP_CTRL_LISTENING_MODES;
    buffer[7] = modes;
    buffer[8] = 0x00;
    buffer[9] = 0x00;
    buffer[10] = 0x00;
}

void aap_debug_print_packet(const char *prefix, const uint8_t *data, size_t len)
{
    /* Raw AAP dumps are useful while reverse-engineering, but they are far
     * too noisy for a long-running desktop daemon.  Keep them opt-in so the
     * normal path does no per-packet formatting or journal I/O. */
    if (g_strcmp0(g_getenv("EARPORT_DEBUG_PACKETS"), "1") != 0)
        return;

    fprintf(stderr, "%s: ", prefix);
    for (size_t i = 0; i < len && i < 64; i++) {
        fprintf(stderr, "%02X ", data[i]);
    }
    if (len > 64) {
        fprintf(stderr, "... (%zu more bytes)", len - 64);
    }
    fprintf(stderr, "\n");
}
