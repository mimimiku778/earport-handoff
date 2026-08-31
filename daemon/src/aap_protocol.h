/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2024 EarPort Contributors
 *
 * Apple AirPods Protocol (AAP) implementation
 * Based on reverse-engineered protocol from LibrePods project
 */

#ifndef AAP_PROTOCOL_H
#define AAP_PROTOCOL_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "airpods_state.h"

/* AAP packet header */
#define AAP_HEADER_SIZE 4
#define AAP_HEADER_BYTE0 0x04
#define AAP_HEADER_BYTE1 0x00
#define AAP_HEADER_BYTE2 0x04
#define AAP_HEADER_BYTE3 0x00

/* Handshake header (different from standard) */
#define AAP_HANDSHAKE_HEADER_BYTE0 0x00
#define AAP_HANDSHAKE_HEADER_BYTE1 0x00

/* Opcodes */
#define AAP_OPCODE_BATTERY               0x04
#define AAP_OPCODE_EAR_DETECTION_REQUEST 0x05
#define AAP_OPCODE_EAR_DETECTION         0x06
#define AAP_OPCODE_CONTROL       0x09
#define AAP_OPCODE_AUDIO_SOURCE  0x0E
#define AAP_OPCODE_NOTIFICATIONS 0x0F
#define AAP_OPCODE_SMART_ROUTING_RESPONSE 0x11
#define AAP_OPCODE_HEAD_TRACKING 0x17
#define AAP_OPCODE_METADATA      0x1D
#define AAP_OPCODE_CA_DETECTION  0x4B
#define AAP_OPCODE_SET_FEATURES  0x4D

/* Control command identifiers (byte after opcode 0x09) */
#define AAP_CTRL_NOISE_CONTROL       0x0D
#define AAP_CTRL_OWNS_CONNECTION     0x06
#define AAP_CTRL_LISTENING_MODES     0x1A
#define AAP_CTRL_ONE_BUD_ANC         0x1B
#define AAP_CTRL_CONV_AWARENESS      0x28
#define AAP_CTRL_ADAPTIVE_LEVEL      0x2E
#define AAP_CTRL_ALLOW_OFF_OPTION    0x34

/* Listening modes bitmask values (for 0x1A ListeningModeConfigs) */
#define AAP_LISTENING_MODE_OFF          0x01
#define AAP_LISTENING_MODE_ANC          0x02
#define AAP_LISTENING_MODE_TRANSPARENCY 0x04
#define AAP_LISTENING_MODE_ADAPTIVE     0x08

/* Battery component types */
#define AAP_BATTERY_SINGLE 0x01  /* AirPods Max (headphones) */
#define AAP_BATTERY_RIGHT  0x02
#define AAP_BATTERY_LEFT   0x04
#define AAP_BATTERY_CASE   0x08

/* Ear detection status */
#define AAP_EAR_IN_EAR   0x00
#define AAP_EAR_OUT      0x01
#define AAP_EAR_IN_CASE  0x02
#define AAP_EAR_DISCONNECTED 0x03

/* Packet sizes */
#define AAP_HANDSHAKE_SIZE       16
#define AAP_REQUEST_NOTIF_SIZE   10
#define AAP_EAR_DETECTION_REQUEST_SIZE 6
#define AAP_SET_FEATURES_SIZE    14
#define AAP_CONTROL_CMD_SIZE     11
#define AAP_MIN_BATTERY_SIZE     12

/* AAP connection-initialization packets do not all use the regular packet
 * parser.  Keep their small state machine here so ordering and duplicate ACK
 * handling can be tested without a live Bluetooth socket. */
typedef enum {
    AAP_INIT_ACTION_NONE,
    AAP_INIT_ACTION_SEND_FEATURES,
    AAP_INIT_ACTION_REQUEST_NOTIFICATIONS,
} AapInitAction;

typedef struct {
    bool features_sent;
    bool notifications_requested;
} AapInitState;

/* Pre-built packets */
extern const uint8_t AAP_PKT_HANDSHAKE[AAP_HANDSHAKE_SIZE];
extern const uint8_t AAP_PKT_REQUEST_NOTIFICATIONS[AAP_REQUEST_NOTIF_SIZE];
extern const uint8_t AAP_PKT_REQUEST_EAR_DETECTION[AAP_EAR_DETECTION_REQUEST_SIZE];
extern const uint8_t AAP_PKT_SET_FEATURES[AAP_SET_FEATURES_SIZE];

/** Reset the per-L2CAP-session initialization state. */
void aap_init_state_reset(AapInitState *state);

/**
 * Return the next initialization action implied by an incoming packet.
 *
 * A duplicate ACK returns the same action until the caller confirms a
 * successful send with aap_init_mark_action_sent().  Once confirmed, later
 * duplicate ACKs are harmless no-ops.
 */
AapInitAction aap_init_next_action(const AapInitState *state,
                                   const uint8_t *data,
                                   size_t len);

/** Record that an initialization action was successfully written. */
void aap_init_mark_action_sent(AapInitState *state, AapInitAction action);

extern const uint8_t AAP_PKT_NC_OFF[AAP_CONTROL_CMD_SIZE];
extern const uint8_t AAP_PKT_NC_ANC[AAP_CONTROL_CMD_SIZE];
extern const uint8_t AAP_PKT_NC_TRANSPARENCY[AAP_CONTROL_CMD_SIZE];
extern const uint8_t AAP_PKT_NC_ADAPTIVE[AAP_CONTROL_CMD_SIZE];

extern const uint8_t AAP_PKT_CA_ENABLE[AAP_CONTROL_CMD_SIZE];
extern const uint8_t AAP_PKT_CA_DISABLE[AAP_CONTROL_CMD_SIZE];

/* Parsed packet result */
typedef enum {
    AAP_PARSE_OK,
    AAP_PARSE_INCOMPLETE,
    AAP_PARSE_INVALID_HEADER,
    AAP_PARSE_UNKNOWN_OPCODE,
    AAP_PARSE_MALFORMED,
} AapParseResult;

/* Packet type after parsing */
typedef enum {
    AAP_PKT_TYPE_UNKNOWN,
    AAP_PKT_TYPE_BATTERY,
    AAP_PKT_TYPE_EAR_DETECTION,
    AAP_PKT_TYPE_AUDIO_SOURCE,
    AAP_PKT_TYPE_OWNERSHIP_RELEASE_REQUEST,
    AAP_PKT_TYPE_NOISE_CONTROL,
    AAP_PKT_TYPE_CONV_AWARENESS,
    AAP_PKT_TYPE_CA_DETECTION,
    AAP_PKT_TYPE_METADATA,
    AAP_PKT_TYPE_LISTENING_MODES,
} AapPacketType;

/* Parsed battery data */
typedef struct {
    int8_t left_level;      /* 0-100 or -1 */
    int8_t right_level;
    int8_t case_level;
    BatteryStatus left_status;
    BatteryStatus right_status;
    BatteryStatus case_status;
} AapBatteryData;

/* Parsed ear detection data */
typedef struct {
    bool primary_in_ear;
    bool secondary_in_ear;
} AapEarDetectionData;

/* Device currently providing audio to the AirPods (TiPi AudioSource). */
typedef enum {
    AAP_AUDIO_SOURCE_NONE = 0x00,
    AAP_AUDIO_SOURCE_CALL = 0x01,
    AAP_AUDIO_SOURCE_MEDIA = 0x02,
} AapAudioSourceType;

typedef struct {
    uint8_t device_address[6];  /* Wire order (Bluetooth address reversed) */
    AapAudioSourceType type;
} AapAudioSourceData;

/* Parsed metadata */
typedef struct {
    char device_name[64];
    char model_number[16];
    char manufacturer[32];
} AapMetadata;

/* Listening modes configuration (bitmask) */
typedef struct {
    bool off_enabled;
    bool transparency_enabled;
    bool anc_enabled;
    bool adaptive_enabled;
    uint8_t raw_value;  /* Raw bitmask for debugging */
} AapListeningModes;

/* Parse result union */
typedef struct {
    AapPacketType type;
    union {
        AapBatteryData battery;
        AapEarDetectionData ear_detection;
        AapAudioSourceData audio_source;
        NoiseControlMode noise_control;
        bool conversational_awareness;
        int ca_volume_level;
        AapMetadata metadata;
        AapListeningModes listening_modes;
    } data;
} AapParsedPacket;

/**
 * Check if buffer starts with valid AAP header
 */
bool aap_has_valid_header(const uint8_t *data, size_t len);

/**
 * Get opcode from packet (assumes valid header)
 */
uint8_t aap_get_opcode(const uint8_t *data, size_t len);

/**
 * Parse incoming AAP packet
 *
 * @param data Raw packet data
 * @param len Data length
 * @param result Output parsed packet
 * @return Parse result code
 */
AapParseResult aap_parse_packet(const uint8_t *data, size_t len, AapParsedPacket *result);

/**
 * Parse battery packet
 */
AapParseResult aap_parse_battery(const uint8_t *data, size_t len, AapBatteryData *battery);

/**
 * Parse ear detection packet
 */
AapParseResult aap_parse_ear_detection(const uint8_t *data, size_t len, AapEarDetectionData *ear);

/** Parse a TiPi AudioSource notification (opcode 0x0E). */
AapParseResult aap_parse_audio_source(const uint8_t *data, size_t len, AapAudioSourceData *source);

/**
 * Parse noise control response
 */
AapParseResult aap_parse_noise_control(const uint8_t *data, size_t len, NoiseControlMode *mode);

/**
 * Build noise control command packet
 *
 * @param mode Desired noise control mode
 * @param buffer Output buffer (must be AAP_CONTROL_CMD_SIZE bytes)
 */
void aap_build_noise_control_cmd(NoiseControlMode mode, uint8_t *buffer);

/**
 * Build adaptive noise level command
 *
 * @param level Level 0-100
 * @param buffer Output buffer (must be AAP_CONTROL_CMD_SIZE bytes)
 */
void aap_build_adaptive_level_cmd(int level, uint8_t *buffer);

/**
 * Build conversational awareness command
 *
 * @param enable Enable or disable
 * @param buffer Output buffer (must be AAP_CONTROL_CMD_SIZE bytes)
 */
void aap_build_conv_awareness_cmd(bool enable, uint8_t *buffer);

/** Build an OwnsConnection command used to claim/release the audio source. */
void aap_build_owns_connection_cmd(bool claim, uint8_t *buffer);

/**
 * Build listening modes configuration command
 *
 * @param modes Bitmask of enabled modes (use AAP_LISTENING_MODE_* constants)
 * @param buffer Output buffer (must be AAP_CONTROL_CMD_SIZE bytes)
 */
void aap_build_listening_modes_cmd(uint8_t modes, uint8_t *buffer);

/**
 * Debug: print packet as hex string
 */
void aap_debug_print_packet(const char *prefix, const uint8_t *data, size_t len);

#endif /* AAP_PROTOCOL_H */
