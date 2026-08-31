/* SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef HANDOFF_POLICY_H
#define HANDOFF_POLICY_H

#include <stdbool.h>

typedef enum {
    HANDOFF_SOURCE_NONE,
    HANDOFF_SOURCE_LOCAL,
    HANDOFF_SOURCE_REMOTE,
} HandoffSource;

typedef struct {
    bool yielded_to_remote;
} HandoffPolicy;

void handoff_policy_reset(HandoffPolicy *policy);
void handoff_policy_note_source(HandoffPolicy *policy, HandoffSource source);
void handoff_policy_note_remote_request(HandoffPolicy *policy);
void handoff_policy_note_linux_claim(HandoffPolicy *policy);
bool handoff_policy_is_yielded(const HandoffPolicy *policy);

#endif /* HANDOFF_POLICY_H */
