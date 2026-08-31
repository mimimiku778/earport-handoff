/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 EarPort Contributors
 *
 * Short-lived PulseAudio/PipeWire route acceleration for newly connected
 * AirPods.  PipeWire exposes its PulseAudio compatibility API through pactl.
 */

#ifndef AUDIO_ROUTE_H
#define AUDIO_ROUTE_H

#include <glib.h>

typedef struct AudioRoute AudioRoute;

AudioRoute *audio_route_new(void);
void audio_route_free(AudioRoute *route);

/* Look briefly for the A2DP BlueZ sink belonging to device_address, make it
 * the default, and move existing playback streams. A newer call supersedes
 * any pending attempt. Returns whether the attempt was accepted. */
gboolean audio_route_start(AudioRoute *route, const char *device_address);

/* Cancel pending retries and child processes. */
void audio_route_cancel(AudioRoute *route);

/* Pure parsing helpers kept public so the pactl boundary can be unit-tested.
 * Explicit HFP/HSP/headset endpoints are not eligible playback sinks: they
 * are commonly exposed before A2DP during startup, so callers should retry. */
gchar *audio_route_find_bluez_sink(const char *pactl_output,
                                   const char *device_address);
GPtrArray *audio_route_parse_sink_input_ids(const char *pactl_output);

#endif /* AUDIO_ROUTE_H */
