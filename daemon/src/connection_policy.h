/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 EarPort Contributors
 */

#ifndef CONNECTION_POLICY_H
#define CONNECTION_POLICY_H

#include <stdbool.h>

typedef enum {
    CONNECTION_DECISION_IGNORE,
    CONNECTION_DECISION_CONNECT,
    CONNECTION_DECISION_CURRENT,
    CONNECTION_DECISION_SWITCH,
    CONNECTION_DECISION_DISCONNECT_CURRENT,
} ConnectionDecision;

bool airpods_address_equal(const char *left, const char *right);

ConnectionDecision connection_policy_device_connected(const char *current_address,
                                                       const char *event_address);

ConnectionDecision connection_policy_device_disconnected(const char *current_address,
                                                          const char *event_address);

#endif /* CONNECTION_POLICY_H */
