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

/* A fully-removed device is intentionally disconnected and may reconnect as
 * soon as it is worn again. Keep this state separate from an Apple-host
 * handoff: only the exact same Bluetooth address may inherit ear-pause
 * ownership across the disconnect. */
typedef struct {
    char *address;
    bool disconnect_pending;
    bool reworn_while_disconnect_pending;
} RemovalLifecycle;

#define REMOVAL_DISCONNECT_RETRY_BASE_MSEC 250u
#define REMOVAL_DISCONNECT_MAX_ATTEMPTS 4u

bool airpods_address_equal(const char *left, const char *right);

ConnectionDecision connection_policy_device_connected(const char *current_address,
                                                       const char *event_address);

ConnectionDecision connection_policy_device_disconnected(const char *current_address,
                                                          const char *event_address);

void removal_lifecycle_mark(RemovalLifecycle *lifecycle,
                            const char *address);
void removal_lifecycle_clear(RemovalLifecycle *lifecycle);
bool removal_lifecycle_matches(const RemovalLifecycle *lifecycle,
                               const char *address);

/* Preserve the lifecycle for a same-device reconnect. A different device
 * consumes it so pause ownership can never leak between two AirPods pairs. */
bool removal_lifecycle_note_connected(RemovalLifecycle *lifecycle,
                                      const char *address);

/* Finish a queued removal disconnect. Returns true when the same device was
 * put back on while BlueZ was still completing that request, in which case
 * the caller must immediately reconnect it. */
bool removal_lifecycle_note_disconnected(RemovalLifecycle *lifecycle,
                                         const char *address);

/* Complete the removal cycle on the first same-device worn state. A valid
 * wear event from another selected device clears the old cycle defensively. */
bool removal_lifecycle_note_wear(RemovalLifecycle *lifecycle,
                                 const char *address,
                                 bool left_in_ear,
                                 bool right_in_ear);

/* A removal Disconnect has already completed `completed_attempts` times with
 * an error. Retry only transient errors while the exact device is still fully
 * removed, and cap the whole removal cycle at four attempts. */
bool removal_disconnect_retry_should_schedule(
    unsigned int completed_attempts,
    bool retryable_error,
    bool still_fully_removed);
unsigned int removal_disconnect_retry_delay_msec(
    unsigned int completed_attempts);

#endif /* CONNECTION_POLICY_H */
