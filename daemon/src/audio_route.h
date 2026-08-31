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

/* Stop pending route work and asynchronously restore the non-Bluetooth sink
 * which was the configured default before EarPort selected AirPods.  The
 * restore is compare-and-set: it is abandoned unless WirePlumber still names
 * EarPort's managed AirPods sink as default. */
gboolean audio_route_restore_previous(AudioRoute *route);

/* Pure parsing helpers kept public so the pactl boundary can be unit-tested.
 * Explicit HFP/HSP/headset endpoints are not eligible playback sinks: they
 * are commonly exposed before A2DP during startup, so callers should retry. */
gchar *audio_route_find_bluez_sink(const char *pactl_output,
                                   const char *device_address);
GPtrArray *audio_route_parse_sink_input_ids(const char *pactl_output);
gchar *audio_route_parse_default_sink(const char *pactl_output);
gchar *audio_route_parse_configured_sink(const char *metadata_output);
gchar *audio_route_find_restore_sink(const char *pactl_output,
                                     const char *preferred_sink);
gchar *audio_route_find_sink_index(const char *pactl_output,
                                   const char *sink_name);
GPtrArray *audio_route_parse_sink_input_ids_for_sink(
    const char *pactl_output,
    const char *sink_index);
gboolean audio_route_should_restore_configured(const char *configured_sink,
                                               const char *managed_sink);

#endif /* AUDIO_ROUTE_H */
