/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "handoff_policy.h"

#include <stddef.h>

void handoff_policy_reset(HandoffPolicy *policy)
{
    if (policy != NULL)
        policy->yielded_to_remote = false;
}

void handoff_policy_note_source(HandoffPolicy *policy, HandoffSource source)
{
    if (policy == NULL)
        return;

    if (source == HANDOFF_SOURCE_REMOTE)
        policy->yielded_to_remote = true;
    else if (source == HANDOFF_SOURCE_LOCAL)
        policy->yielded_to_remote = false;

    /* NONE is intentionally sticky. AirPods 4 emits transient NONE frames
     * while another Apple host is still completing smart routing. */
}

void handoff_policy_note_remote_request(HandoffPolicy *policy)
{
    if (policy != NULL)
        policy->yielded_to_remote = true;
}

void handoff_policy_note_linux_claim(HandoffPolicy *policy)
{
    if (policy != NULL)
        policy->yielded_to_remote = false;
}

bool handoff_policy_is_yielded(const HandoffPolicy *policy)
{
    return policy != NULL && policy->yielded_to_remote;
}
