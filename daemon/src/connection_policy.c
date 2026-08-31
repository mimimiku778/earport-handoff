/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 EarPort Contributors
 */

#include "connection_policy.h"

#include <stddef.h>
#include <strings.h>

static bool address_is_valid(const char *address)
{
    return address != NULL && address[0] != '\0';
}

bool airpods_address_equal(const char *left, const char *right)
{
    return address_is_valid(left) && address_is_valid(right) &&
           strcasecmp(left, right) == 0;
}

ConnectionDecision connection_policy_device_connected(const char *current_address,
                                                       const char *event_address)
{
    if (!address_is_valid(event_address))
        return CONNECTION_DECISION_IGNORE;

    if (!address_is_valid(current_address))
        return CONNECTION_DECISION_CONNECT;

    if (airpods_address_equal(current_address, event_address))
        return CONNECTION_DECISION_CURRENT;

    return CONNECTION_DECISION_SWITCH;
}

ConnectionDecision connection_policy_device_disconnected(const char *current_address,
                                                          const char *event_address)
{
    if (!airpods_address_equal(current_address, event_address))
        return CONNECTION_DECISION_IGNORE;

    return CONNECTION_DECISION_DISCONNECT_CURRENT;
}
