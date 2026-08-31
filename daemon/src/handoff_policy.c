/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "handoff_policy.h"

#include <stddef.h>

void handoff_policy_reset(HandoffPolicy *policy)
{
    if (policy != NULL) {
        policy->yielded_to_remote = false;
        policy->confirmed_linux_source = false;
    }
}

void handoff_policy_note_source(HandoffPolicy *policy, HandoffSource source)
{
    if (policy == NULL)
        return;

    if (source == HANDOFF_SOURCE_REMOTE) {
        policy->yielded_to_remote = true;
        policy->confirmed_linux_source = false;
    } else if (source == HANDOFF_SOURCE_LOCAL) {
        policy->yielded_to_remote = false;
        policy->confirmed_linux_source = true;
    }

    /* NONE is intentionally sticky for both fields. AirPods 4 emits transient
     * NONE frames while another host is still completing smart routing. */
}

void handoff_policy_note_remote_request(HandoffPolicy *policy)
{
    if (policy != NULL) {
        policy->yielded_to_remote = true;
        policy->confirmed_linux_source = false;
    }
}

void handoff_policy_note_linux_claim(HandoffPolicy *policy)
{
    if (policy != NULL)
        policy->yielded_to_remote = false;

    /* Sending OwnsConnection is only a request.  Keep source confirmation
     * unchanged until an AudioSource LOCAL notification arrives. */
}

bool handoff_policy_is_yielded(const HandoffPolicy *policy)
{
    return policy != NULL && policy->yielded_to_remote;
}

bool handoff_policy_has_confirmed_linux_source(const HandoffPolicy *policy)
{
    return policy != NULL && policy->confirmed_linux_source;
}
