/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 EarPort Contributors
 */

#include "connection_policy.h"

#include <glib.h>
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

void removal_lifecycle_mark(RemovalLifecycle *lifecycle,
                            const char *address)
{
    if (lifecycle == NULL)
        return;

    if (!address_is_valid(address)) {
        removal_lifecycle_clear(lifecycle);
        return;
    }

    if (!airpods_address_equal(lifecycle->address, address)) {
        g_free(lifecycle->address);
        lifecycle->address = g_strdup(address);
    }
    lifecycle->disconnect_pending = true;
    lifecycle->reworn_while_disconnect_pending = false;
}

void removal_lifecycle_clear(RemovalLifecycle *lifecycle)
{
    if (lifecycle == NULL)
        return;

    g_clear_pointer(&lifecycle->address, g_free);
    lifecycle->disconnect_pending = false;
    lifecycle->reworn_while_disconnect_pending = false;
}

bool removal_lifecycle_matches(const RemovalLifecycle *lifecycle,
                               const char *address)
{
    return lifecycle != NULL &&
           airpods_address_equal(lifecycle->address, address);
}

bool removal_lifecycle_note_connected(RemovalLifecycle *lifecycle,
                                      const char *address)
{
    if (removal_lifecycle_matches(lifecycle, address))
        return true;

    removal_lifecycle_clear(lifecycle);
    return false;
}

bool removal_lifecycle_note_disconnected(RemovalLifecycle *lifecycle,
                                         const char *address)
{
    if (!removal_lifecycle_matches(lifecycle, address))
        return false;

    bool reconnect = lifecycle->disconnect_pending &&
                     lifecycle->reworn_while_disconnect_pending;
    lifecycle->disconnect_pending = false;
    lifecycle->reworn_while_disconnect_pending = false;
    return reconnect;
}

bool removal_lifecycle_note_wear(RemovalLifecycle *lifecycle,
                                 const char *address,
                                 bool left_in_ear,
                                 bool right_in_ear)
{
    if (lifecycle == NULL)
        return false;

    if (!removal_lifecycle_matches(lifecycle, address)) {
        /* A wear event from a different selected device must never inherit
         * pause ownership from an older AirPods pair. */
        if (address_is_valid(address))
            removal_lifecycle_clear(lifecycle);
        return false;
    }

    if (!left_in_ear && !right_in_ear) {
        if (lifecycle->disconnect_pending)
            lifecycle->reworn_while_disconnect_pending = false;
        return false;
    }

    /* A BlueZ Disconnect call cannot be recalled after it has been sent. Keep
     * the lifecycle until its Connected=false event arrives so main can issue
     * an immediate same-device Connect instead of waiting for a second BLE
     * unworn -> worn edge. */
    if (lifecycle->disconnect_pending) {
        lifecycle->reworn_while_disconnect_pending = true;
        return false;
    }

    removal_lifecycle_clear(lifecycle);
    return true;
}

bool removal_disconnect_retry_should_schedule(
    unsigned int completed_attempts,
    bool retryable_error,
    bool still_fully_removed)
{
    return completed_attempts > 0 &&
           completed_attempts < REMOVAL_DISCONNECT_MAX_ATTEMPTS &&
           retryable_error && still_fully_removed;
}

unsigned int removal_disconnect_retry_delay_msec(
    unsigned int completed_attempts)
{
    if (completed_attempts == 0)
        return REMOVAL_DISCONNECT_RETRY_BASE_MSEC;

    unsigned int shift = completed_attempts - 1;
    unsigned int max_shift = REMOVAL_DISCONNECT_MAX_ATTEMPTS - 2;
    if (shift > max_shift)
        shift = max_shift;
    return REMOVAL_DISCONNECT_RETRY_BASE_MSEC << shift;
}
