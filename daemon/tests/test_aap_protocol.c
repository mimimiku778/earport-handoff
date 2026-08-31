/* SPDX-License-Identifier: GPL-3.0-or-later */

#include <glib.h>
#include <string.h>

#include "aap_protocol.h"

static void test_audio_source_parse(void)
{
    const uint8_t packet[] = {
        0x04, 0x00, 0x04, 0x00, 0x0E, 0x00,
        0x66, 0x55, 0x44, 0x33, 0x22, 0x11,
        AAP_AUDIO_SOURCE_MEDIA,
    };
    const uint8_t expected_address[] = {0x66, 0x55, 0x44, 0x33, 0x22, 0x11};

    AapParsedPacket parsed;
    g_assert_cmpint(aap_parse_packet(packet, sizeof(packet), &parsed), ==,
                    AAP_PARSE_OK);
    g_assert_cmpint(parsed.type, ==, AAP_PKT_TYPE_AUDIO_SOURCE);
    g_assert_cmpint(parsed.data.audio_source.type, ==,
                    AAP_AUDIO_SOURCE_MEDIA);
    g_assert_cmpint(memcmp(parsed.data.audio_source.device_address,
                           expected_address, sizeof(expected_address)), ==, 0);
}

static void test_audio_source_rejects_bad_packets(void)
{
    const uint8_t short_packet[] = {
        0x04, 0x00, 0x04, 0x00, 0x0E, 0x00,
    };
    const uint8_t invalid_type_packet[] = {
        0x04, 0x00, 0x04, 0x00, 0x0E, 0x00,
        0, 0, 0, 0, 0, 0, 0x03,
    };
    AapParsedPacket parsed;

    g_assert_cmpint(aap_parse_packet(short_packet, sizeof(short_packet), &parsed),
                    ==, AAP_PARSE_INCOMPLETE);
    g_assert_cmpint(aap_parse_packet(invalid_type_packet,
                                     sizeof(invalid_type_packet), &parsed),
                    ==, AAP_PARSE_MALFORMED);
}

static void test_owns_connection_command(void)
{
    const uint8_t expected_claim[] = {
        0x04, 0x00, 0x04, 0x00, 0x09, 0x00,
        0x06, 0x01, 0x00, 0x00, 0x00,
    };
    const uint8_t expected_release[] = {
        0x04, 0x00, 0x04, 0x00, 0x09, 0x00,
        0x06, 0x00, 0x00, 0x00, 0x00,
    };
    uint8_t command[AAP_CONTROL_CMD_SIZE];

    aap_build_owns_connection_cmd(true, command);
    g_assert_cmpint(memcmp(command, expected_claim, sizeof(command)), ==, 0);

    aap_build_owns_connection_cmd(false, command);
    g_assert_cmpint(memcmp(command, expected_release, sizeof(command)), ==, 0);
}

static void test_ear_detection_request_packet(void)
{
    const uint8_t expected[] = {
        0x04, 0x00, 0x04, 0x00, 0x05, 0x00,
    };

    g_assert_cmpuint(sizeof(AAP_PKT_REQUEST_EAR_DETECTION), ==,
                     sizeof(expected));
    g_assert_cmpint(memcmp(AAP_PKT_REQUEST_EAR_DETECTION,
                           expected, sizeof(expected)), ==, 0);
}

static void test_ear_detection_status_validation(void)
{
    const uint8_t valid_packets[][8] = {
        {0x04, 0x00, 0x04, 0x00, AAP_OPCODE_EAR_DETECTION, 0x00,
         AAP_EAR_IN_EAR, AAP_EAR_OUT},
        {0x04, 0x00, 0x04, 0x00, AAP_OPCODE_EAR_DETECTION, 0x00,
         AAP_EAR_OUT, AAP_EAR_IN_CASE},
        {0x04, 0x00, 0x04, 0x00, AAP_OPCODE_EAR_DETECTION, 0x00,
         AAP_EAR_IN_CASE, AAP_EAR_IN_EAR},
        {0x04, 0x00, 0x04, 0x00, AAP_OPCODE_EAR_DETECTION, 0x00,
         AAP_EAR_IN_EAR, AAP_EAR_DISCONNECTED},
        {0x04, 0x00, 0x04, 0x00, AAP_OPCODE_EAR_DETECTION, 0x00,
         AAP_EAR_DISCONNECTED, AAP_EAR_IN_EAR},
    };
    const uint8_t invalid_primary[] = {
        0x04, 0x00, 0x04, 0x00, AAP_OPCODE_EAR_DETECTION, 0x00,
        0xFF, AAP_EAR_IN_EAR,
    };
    const uint8_t invalid_secondary[] = {
        0x04, 0x00, 0x04, 0x00, AAP_OPCODE_EAR_DETECTION, 0x00,
        AAP_EAR_OUT, 0x04,
    };
    const bool expected_primary[] = {true, false, false, true, false};
    const bool expected_secondary[] = {false, false, true, false, true};
    AapParsedPacket parsed;

    for (size_t i = 0; i < G_N_ELEMENTS(valid_packets); i++) {
        g_assert_cmpint(aap_parse_packet(valid_packets[i],
                                         sizeof(valid_packets[i]),
                                         &parsed),
                        ==, AAP_PARSE_OK);
        g_assert_cmpint(parsed.type, ==, AAP_PKT_TYPE_EAR_DETECTION);
        g_assert_cmpint(parsed.data.ear_detection.primary_in_ear, ==,
                        expected_primary[i]);
        g_assert_cmpint(parsed.data.ear_detection.secondary_in_ear, ==,
                        expected_secondary[i]);
    }

    g_assert_cmpint(aap_parse_packet(invalid_primary,
                                     sizeof(invalid_primary),
                                     &parsed),
                    ==, AAP_PARSE_MALFORMED);
    g_assert_cmpint(aap_parse_packet(invalid_secondary,
                                     sizeof(invalid_secondary),
                                     &parsed),
                    ==, AAP_PARSE_MALFORMED);
}

static void test_smart_routing_release_request(void)
{
    const uint8_t packet[] = {
        0x04, 0x00, 0x04, 0x00, AAP_OPCODE_SMART_ROUTING_RESPONSE, 0x00,
        0x00, 0xff, 'S', 'e', 't', 'O', 'w', 'n', 'e', 'r', 's', 'h', 'i', 'p',
        'T', 'o', 'F', 'a', 'l', 's', 'e',
    };
    const uint8_t truncated_marker[] = {
        0x04, 0x00, 0x04, 0x00, AAP_OPCODE_SMART_ROUTING_RESPONSE, 0x00,
        'S', 'e', 't', 'O', 'w', 'n', 'e', 'r', 's', 'h', 'i', 'p',
        'T', 'o', 'F', 'a', 'l', 's',
    };
    const uint8_t case_changed_marker[] = {
        0x04, 0x00, 0x04, 0x00, AAP_OPCODE_SMART_ROUTING_RESPONSE, 0x00,
        's', 'e', 't', 'O', 'w', 'n', 'e', 'r', 's', 'h', 'i', 'p',
        'T', 'o', 'F', 'a', 'l', 's', 'e',
    };
    const uint8_t short_packet[] = {
        0x04, 0x00, 0x04, 0x00, AAP_OPCODE_SMART_ROUTING_RESPONSE,
    };
    AapParsedPacket parsed;

    g_assert_cmpint(aap_parse_packet(packet, sizeof(packet), &parsed), ==,
                    AAP_PARSE_OK);
    g_assert_cmpint(parsed.type, ==,
                    AAP_PKT_TYPE_OWNERSHIP_RELEASE_REQUEST);

    g_assert_cmpint(aap_parse_packet(truncated_marker,
                                     sizeof(truncated_marker), &parsed), ==,
                    AAP_PARSE_OK);
    g_assert_cmpint(parsed.type, ==, AAP_PKT_TYPE_UNKNOWN);

    g_assert_cmpint(aap_parse_packet(case_changed_marker,
                                     sizeof(case_changed_marker), &parsed), ==,
                    AAP_PARSE_OK);
    g_assert_cmpint(parsed.type, ==, AAP_PKT_TYPE_UNKNOWN);

    g_assert_cmpint(aap_parse_packet(short_packet, sizeof(short_packet),
                                     &parsed), ==,
                    AAP_PARSE_INCOMPLETE);
}

static void test_initialization_ack_sequence(void)
{
    const uint8_t handshake_ack[] = {
        0x01, 0x00, 0x04, 0x00, 0x00, 0x00, 0x01, 0x00,
        0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00,
    };
    const uint8_t features_ack[] = {
        0x04, 0x00, 0x04, 0x00, 0x2B, 0x00, 0x01,
    };
    AapInitState state;

    aap_init_state_reset(&state);

    g_assert_cmpint(aap_init_next_action(&state,
                                         handshake_ack,
                                         sizeof(handshake_ack)),
                    ==, AAP_INIT_ACTION_SEND_FEATURES);

    /* A failed write may be retried when the device repeats its ACK. */
    g_assert_cmpint(aap_init_next_action(&state,
                                         handshake_ack,
                                         sizeof(handshake_ack)),
                    ==, AAP_INIT_ACTION_SEND_FEATURES);

    aap_init_mark_action_sent(&state, AAP_INIT_ACTION_SEND_FEATURES);
    g_assert_cmpint(aap_init_next_action(&state,
                                         handshake_ack,
                                         sizeof(handshake_ack)),
                    ==, AAP_INIT_ACTION_NONE);

    g_assert_cmpint(aap_init_next_action(&state,
                                         features_ack,
                                         sizeof(features_ack)),
                    ==, AAP_INIT_ACTION_REQUEST_NOTIFICATIONS);
    aap_init_mark_action_sent(&state,
                              AAP_INIT_ACTION_REQUEST_NOTIFICATIONS);
    g_assert_cmpint(aap_init_next_action(&state,
                                         features_ack,
                                         sizeof(features_ack)),
                    ==, AAP_INIT_ACTION_NONE);
}

static void test_initialization_rejects_unrelated_and_short_packets(void)
{
    const uint8_t short_handshake_ack[] = {0x01, 0x00, 0x04};
    const uint8_t ordinary_packet[] = {
        0x04, 0x00, 0x04, 0x00, AAP_OPCODE_EAR_DETECTION, 0x00,
        AAP_EAR_OUT, AAP_EAR_OUT,
    };
    AapInitState state;

    aap_init_state_reset(&state);
    g_assert_cmpint(aap_init_next_action(&state,
                                         short_handshake_ack,
                                         sizeof(short_handshake_ack)),
                    ==, AAP_INIT_ACTION_NONE);
    g_assert_cmpint(aap_init_next_action(&state,
                                         ordinary_packet,
                                         sizeof(ordinary_packet)),
                    ==, AAP_INIT_ACTION_NONE);
}

static void test_ca_detection_requires_volume_byte(void)
{
    const uint8_t short_packet[] = {
        0x04, 0x00, 0x04, 0x00, AAP_OPCODE_CA_DETECTION,
        0x00, 0x02, 0x00, 0x01,
    };
    const uint8_t complete_packet[] = {
        0x04, 0x00, 0x04, 0x00, AAP_OPCODE_CA_DETECTION,
        0x00, 0x02, 0x00, 0x01, 73,
    };
    AapParsedPacket parsed;

    memset(&parsed, 0xA5, sizeof(parsed));
    g_assert_cmpint(aap_parse_packet(short_packet, sizeof(short_packet),
                                     &parsed), ==, AAP_PARSE_INCOMPLETE);

    g_assert_cmpint(aap_parse_packet(complete_packet,
                                     sizeof(complete_packet), &parsed), ==,
                    AAP_PARSE_OK);
    g_assert_cmpint(parsed.type, ==, AAP_PKT_TYPE_CA_DETECTION);
    g_assert_cmpuint(parsed.data.ca_volume_level, ==, 73);
}

static void test_metadata_truncation_preserves_field_boundaries(void)
{
    uint8_t packet[12 + 80 + 1 + 5 + 1 + 5 + 1] = {
        0x04, 0x00, 0x04, 0x00, AAP_OPCODE_METADATA, 0x00,
    };
    size_t position = 12;
    memset(packet + position, 'N', 80);
    position += 80;
    packet[position++] = '\0';
    memcpy(packet + position, "A3454", 5);
    position += 5;
    packet[position++] = '\0';
    memcpy(packet + position, "Apple", 5);
    position += 5;
    packet[position++] = '\0';

    AapParsedPacket parsed;
    g_assert_cmpuint(position, ==, sizeof(packet));
    g_assert_cmpint(aap_parse_packet(packet, sizeof(packet), &parsed), ==,
                    AAP_PARSE_OK);
    g_assert_cmpuint(strlen(parsed.data.metadata.device_name), ==,
                     sizeof(parsed.data.metadata.device_name) - 1);
    g_assert_cmpstr(parsed.data.metadata.model_number, ==, "A3454");
    g_assert_cmpstr(parsed.data.metadata.manufacturer, ==, "Apple");
}

static void test_metadata_requires_terminated_fields(void)
{
    const uint8_t packet[] = {
        0x04, 0x00, 0x04, 0x00, AAP_OPCODE_METADATA, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        'N', 'a', 'm', 'e', '\0', 'A', '3', '4', '5', '4',
    };
    AapParsedPacket parsed;

    g_assert_cmpint(aap_parse_packet(packet, sizeof(packet), &parsed), ==,
                    AAP_PARSE_INCOMPLETE);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/aap/audio-source/parse", test_audio_source_parse);
    g_test_add_func("/aap/audio-source/reject-invalid",
                    test_audio_source_rejects_bad_packets);
    g_test_add_func("/aap/owns-connection/build",
                    test_owns_connection_command);
    g_test_add_func("/aap/ear-detection/request-packet",
                    test_ear_detection_request_packet);
    g_test_add_func("/aap/ear-detection/status-validation",
                    test_ear_detection_status_validation);
    g_test_add_func("/aap/smart-routing/release-request",
                    test_smart_routing_release_request);
    g_test_add_func("/aap/initialization/ack-sequence",
                    test_initialization_ack_sequence);
    g_test_add_func("/aap/initialization/reject-unrelated",
                    test_initialization_rejects_unrelated_and_short_packets);
    g_test_add_func("/aap/ca-detection/requires-volume-byte",
                    test_ca_detection_requires_volume_byte);
    g_test_add_func("/aap/metadata/truncation-preserves-boundaries",
                    test_metadata_truncation_preserves_field_boundaries);
    g_test_add_func("/aap/metadata/requires-terminated-fields",
                    test_metadata_requires_terminated_fields);
    return g_test_run();
}
