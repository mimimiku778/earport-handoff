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

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/aap/audio-source/parse", test_audio_source_parse);
    g_test_add_func("/aap/audio-source/reject-invalid",
                    test_audio_source_rejects_bad_packets);
    g_test_add_func("/aap/owns-connection/build",
                    test_owns_connection_command);
    g_test_add_func("/aap/initialization/ack-sequence",
                    test_initialization_ack_sequence);
    g_test_add_func("/aap/initialization/reject-unrelated",
                    test_initialization_rejects_unrelated_and_short_packets);
    return g_test_run();
}
