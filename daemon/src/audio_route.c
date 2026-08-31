/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 EarPort Contributors
 */

#include "audio_route.h"

#include <gio/gio.h>
#include <string.h>

#define ROUTE_RETRY_INTERVAL_MSEC 250
#define ROUTE_DEADLINE_MSEC 5000
#define ROUTE_PROCESS_TIMEOUT_MSEC 750
#define ROUTE_SECOND_INPUT_PASS_MSEC 150
#define ROUTE_TRANSPORT_RESTART_MSEC 200

typedef enum {
    ROUTE_OPERATION_FIND_SINK,
    ROUTE_OPERATION_CAPTURE_CURRENT_DEFAULT,
    ROUTE_OPERATION_CAPTURE_CONFIGURED_DEFAULT,
    ROUTE_OPERATION_SUSPEND_SINK,
    ROUTE_OPERATION_RESUME_SINK,
    ROUTE_OPERATION_SET_DEFAULT,
    ROUTE_OPERATION_LIST_INPUTS,
    ROUTE_OPERATION_MOVE_INPUT,
    ROUTE_OPERATION_RESTORE_CHECK_CONFIGURED,
    ROUTE_OPERATION_RESTORE_LIST_SINKS,
    ROUTE_OPERATION_RESTORE_RECHECK_CONFIGURED,
    ROUTE_OPERATION_RESTORE_SET_DEFAULT,
    ROUTE_OPERATION_RESTORE_LIST_INPUTS,
    ROUTE_OPERATION_RESTORE_MOVE_INPUT,
} RouteOperationType;

typedef enum {
    ROUTE_MODE_IDLE,
    ROUTE_MODE_SELECTING,
    ROUTE_MODE_RESTORING,
} RouteMode;

typedef struct {
    AudioRoute *route;
    guint generation;
    RouteOperationType type;
    GSubprocess *process;
    GCancellable *cancellable;
    guint timeout_id;
    gchar *sink_name;
    gchar *source_sink_index;
} RouteOperation;

struct AudioRoute {
    gint ref_count;
    guint generation;
    guint retry_timeout_id;
    guint second_input_pass_timeout_id;
    guint resume_sink_timeout_id;
    gint64 deadline_usec;
    gchar *device_address;
    gchar *second_input_pass_sink;
    gchar *resume_sink;
    GCancellable *cancellable;
    RouteMode mode;

    /* Persist across cancellation so a later disconnect can undo only the
     * default which this instance selected. */
    gchar *previous_default_sink;
    gchar *managed_sink;

    /* Scratch state for one bounded restore attempt. */
    gchar *restore_sink;
    gchar *restore_managed_index;

    /* Non-owning keys. Each RouteOperation owns its process until its async
     * completion callback. This table lets cancellation terminate stale pactl
     * children immediately. */
    GHashTable *active_processes;
};

static AudioRoute *audio_route_ref(AudioRoute *route)
{
    g_atomic_int_inc(&route->ref_count);
    return route;
}

static void audio_route_unref(AudioRoute *route)
{
    if (!g_atomic_int_dec_and_test(&route->ref_count))
        return;

    g_clear_object(&route->cancellable);
    g_clear_pointer(&route->device_address, g_free);
    g_clear_pointer(&route->second_input_pass_sink, g_free);
    g_clear_pointer(&route->resume_sink, g_free);
    g_clear_pointer(&route->previous_default_sink, g_free);
    g_clear_pointer(&route->managed_sink, g_free);
    g_clear_pointer(&route->restore_sink, g_free);
    g_clear_pointer(&route->restore_managed_index, g_free);
    g_clear_pointer(&route->active_processes, g_hash_table_unref);
    g_free(route);
}

static gboolean valid_device_address(const char *address)
{
    if (address == NULL || strlen(address) != 17)
        return FALSE;

    for (gsize i = 0; i < 17; i++) {
        if ((i + 1) % 3 == 0) {
            if (address[i] != ':')
                return FALSE;
        } else if (!g_ascii_isxdigit(address[i])) {
            return FALSE;
        }
    }

    return TRUE;
}

static gboolean sink_name_matches_device(const char *sink_name,
                                         const char *device_address)
{
    if (sink_name == NULL || !valid_device_address(device_address))
        return FALSE;

    gchar *lower_name = g_ascii_strdown(sink_name, -1);
    gchar *colon_token = g_ascii_strdown(device_address, -1);
    gchar *underscore_token = g_strdup(colon_token);
    for (gchar *p = underscore_token; *p != '\0'; p++) {
        if (*p == ':')
            *p = '_';
    }

    gboolean matches = g_strstr_len(lower_name, -1, "bluez") != NULL &&
                       (g_strstr_len(lower_name, -1, colon_token) != NULL ||
                        g_strstr_len(lower_name, -1, underscore_token) != NULL);
    g_free(lower_name);
    g_free(colon_token);
    g_free(underscore_token);
    return matches;
}

static unsigned int sink_name_priority(const char *sink_name)
{
    gchar *lower_name = g_ascii_strdown(sink_name, -1);
    unsigned int priority = 1;

    if (g_strstr_len(lower_name, -1, "headset") != NULL ||
        g_strstr_len(lower_name, -1, "handsfree") != NULL ||
        g_strstr_len(lower_name, -1, "hands-free") != NULL ||
        g_strstr_len(lower_name, -1, "head_unit") != NULL ||
        g_strstr_len(lower_name, -1, "head-unit") != NULL ||
        g_strstr_len(lower_name, -1, "hfp") != NULL ||
        g_strstr_len(lower_name, -1, "hsp") != NULL) {
        priority = 0;
    } else if (g_strstr_len(lower_name, -1, "a2dp") != NULL ||
               g_strstr_len(lower_name, -1, "high-fidelity") != NULL ||
               g_strstr_len(lower_name, -1, "high_fidelity") != NULL) {
        priority = 2;
    }

    g_free(lower_name);
    return priority;
}

static gboolean text_has_a2dp_profile(const char *lower_text)
{
    return g_strstr_len(lower_text, -1, "a2dp") != NULL ||
           g_strstr_len(lower_text, -1, "high-fidelity") != NULL ||
           g_strstr_len(lower_text, -1, "high_fidelity") != NULL;
}

static gboolean text_has_call_profile(const char *lower_text)
{
    return g_strstr_len(lower_text, -1, "headset") != NULL ||
           g_strstr_len(lower_text, -1, "handsfree") != NULL ||
           g_strstr_len(lower_text, -1, "hands-free") != NULL ||
           g_strstr_len(lower_text, -1, "head_unit") != NULL ||
           g_strstr_len(lower_text, -1, "head-unit") != NULL ||
           g_strstr_len(lower_text, -1, "hfp") != NULL ||
           g_strstr_len(lower_text, -1, "hsp") != NULL;
}

static gboolean line_describes_active_profile(const char *lower_line)
{
    return g_strstr_len(lower_line, -1, "api.bluez5.profile") != NULL ||
           g_strstr_len(lower_line, -1, "bluez5.profile") != NULL ||
           g_strstr_len(lower_line, -1, "bluetooth.protocol") != NULL ||
           g_strstr_len(lower_line, -1, "device.profile.name") != NULL ||
           g_strstr_len(lower_line, -1, "device.profile.description") != NULL ||
           g_str_has_prefix(lower_line, "active port:");
}

static void consider_verbose_sink(gchar **selected,
                                  const gchar *sink_name,
                                  gboolean profile_is_a2dp,
                                  gboolean profile_is_call,
                                  const char *device_address)
{
    if (*selected != NULL || sink_name == NULL ||
        !sink_name_matches_device(sink_name, device_address)) {
        return;
    }

    unsigned int name_priority = sink_name_priority(sink_name);
    if (profile_is_call || name_priority == 0)
        return;

    /* PipeWire often uses an opaque suffix such as `.1`; accept it only when
     * the active node properties identify A2DP. Explicit legacy PulseAudio
     * A2DP names remain valid even if no profile property is printed. */
    if (profile_is_a2dp || name_priority == 2)
        *selected = g_strdup(sink_name);
}

static gchar *find_sink_in_verbose_output(const char *pactl_output,
                                          const char *device_address,
                                          gboolean *recognized)
{
    gchar *selected = NULL;
    gchar *current_name = NULL;
    gboolean current_a2dp = FALSE;
    gboolean current_call = FALSE;
    gboolean in_sink = FALSE;
    gchar **lines = g_strsplit(pactl_output, "\n", -1);

    *recognized = FALSE;
    for (gchar **line = lines; *line != NULL; line++) {
        gchar *trimmed = g_strdup(*line);
        g_strstrip(trimmed);

        if (g_str_has_prefix(trimmed, "Sink #")) {
            if (in_sink) {
                consider_verbose_sink(&selected,
                                      current_name,
                                      current_a2dp,
                                      current_call,
                                      device_address);
            }
            *recognized = TRUE;
            in_sink = TRUE;
            g_clear_pointer(&current_name, g_free);
            current_a2dp = FALSE;
            current_call = FALSE;
        } else if (in_sink && g_str_has_prefix(trimmed, "Name:")) {
            const gchar *name = trimmed + strlen("Name:");
            while (g_ascii_isspace(*name))
                name++;
            g_free(current_name);
            current_name = g_strdup(name);
        } else if (in_sink) {
            gchar *lower = g_ascii_strdown(trimmed, -1);
            if (line_describes_active_profile(lower)) {
                current_a2dp |= text_has_a2dp_profile(lower);
                current_call |= text_has_call_profile(lower);
            }
            g_free(lower);
        }
        g_free(trimmed);
    }

    if (in_sink) {
        consider_verbose_sink(&selected,
                              current_name,
                              current_a2dp,
                              current_call,
                              device_address);
    }

    g_free(current_name);
    g_strfreev(lines);
    return selected;
}

static gchar *find_sink_for_address(const char *pactl_output,
                                    const char *device_address)
{
    if (pactl_output == NULL || !valid_device_address(device_address))
        return NULL;

    gboolean verbose_output = FALSE;
    gchar *sink_name = find_sink_in_verbose_output(pactl_output,
                                                    device_address,
                                                    &verbose_output);
    if (verbose_output)
        return sink_name;

    /* Legacy short output does not expose the active Bluetooth profile.
     * Only names which explicitly say A2DP/high-fidelity are safe. */
    gchar **lines = g_strsplit(pactl_output, "\n", -1);
    for (gchar **line = lines; *line != NULL; line++) {
        gchar *first_tab = strchr(*line, '\t');
        gchar *second_tab = first_tab != NULL
                                ? strchr(first_tab + 1, '\t')
                                : NULL;
        if (first_tab != NULL && second_tab != NULL &&
            second_tab > first_tab + 1) {
            gchar *candidate = g_strndup(
                first_tab + 1,
                (gsize)(second_tab - first_tab - 1));
            if (sink_name_matches_device(candidate, device_address)) {
                unsigned int priority = sink_name_priority(candidate);
                if (priority == 2 && sink_name == NULL)
                    sink_name = g_strdup(candidate);
            }
            g_free(candidate);
        }
    }

    g_strfreev(lines);
    return sink_name;
}

gchar *audio_route_find_bluez_sink(const char *pactl_output,
                                   const char *device_address)
{
    return find_sink_for_address(pactl_output, device_address);
}

GPtrArray *audio_route_parse_sink_input_ids(const char *pactl_output)
{
    GPtrArray *ids = g_ptr_array_new_with_free_func(g_free);
    if (pactl_output == NULL)
        return ids;

    gchar **lines = g_strsplit(pactl_output, "\n", -1);
    for (gchar **line = lines; *line != NULL; line++) {
        const gchar *end = *line;
        while (g_ascii_isdigit(*end))
            end++;

        if (end == *line || (*end != '\t' && !g_ascii_isspace(*end)))
            continue;

        g_ptr_array_add(ids, g_strndup(*line, (gsize)(end - *line)));
    }
    g_strfreev(lines);
    return ids;
}

static gboolean sink_name_is_bluetooth(const char *sink_name)
{
    if (sink_name == NULL)
        return FALSE;

    gchar *lower = g_ascii_strdown(sink_name, -1);
    gboolean bluetooth = g_strstr_len(lower, -1, "bluez") != NULL;
    g_free(lower);
    return bluetooth;
}

static gchar *parse_single_sink_name(const char *text)
{
    if (text == NULL)
        return NULL;

    gchar *line = g_strdup(text);
    gchar *newline = strpbrk(line, "\r\n");
    if (newline != NULL)
        *newline = '\0';
    g_strstrip(line);

    if (*line == '\0' || strpbrk(line, " \t\r\n") != NULL) {
        g_free(line);
        return NULL;
    }
    return line;
}

gchar *audio_route_parse_default_sink(const char *pactl_output)
{
    return parse_single_sink_name(pactl_output);
}

gchar *audio_route_parse_configured_sink(const char *metadata_output)
{
    if (metadata_output == NULL)
        return NULL;

    gchar **lines = g_strsplit(metadata_output, "\n", -1);
    gchar *result = NULL;
    for (gchar **line = lines; *line != NULL && result == NULL; line++) {
        if (g_strstr_len(*line, -1,
                         "key:'default.configured.audio.sink'") == NULL) {
            continue;
        }

        const gchar *name = g_strstr_len(*line, -1, "\"name\"");
        if (name == NULL)
            continue;
        name = strchr(name + strlen("\"name\""), ':');
        if (name == NULL)
            continue;
        name++;
        while (g_ascii_isspace(*name))
            name++;
        if (*name != '"')
            continue;
        name++;

        const gchar *end = name;
        while (*end != '\0' && *end != '"' &&
               !g_ascii_iscntrl(*end) && !g_ascii_isspace(*end)) {
            end++;
        }
        if (*end == '"' && end > name)
            result = g_strndup(name, (gsize)(end - name));
    }

    g_strfreev(lines);
    return result;
}

static gboolean parse_short_sink_line(const char *line,
                                      gchar **index_out,
                                      gchar **name_out)
{
    gchar **fields = g_strsplit(line, "\t", 3);
    if (fields[0] == NULL || fields[1] == NULL) {
        g_strfreev(fields);
        return FALSE;
    }

    g_strstrip(fields[0]);
    g_strstrip(fields[1]);
    const gchar *digit = fields[0];
    while (g_ascii_isdigit(*digit))
        digit++;
    gboolean valid = fields[0][0] != '\0' && *digit == '\0' &&
                     fields[1][0] != '\0';
    if (valid) {
        if (index_out != NULL)
            *index_out = g_strdup(fields[0]);
        if (name_out != NULL)
            *name_out = g_strdup(fields[1]);
    }
    g_strfreev(fields);
    return valid;
}

gchar *audio_route_find_restore_sink(const char *pactl_output,
                                     const char *preferred_sink)
{
    if (pactl_output == NULL)
        return NULL;

    gchar *first_alsa = NULL;
    gchar *first_other = NULL;
    gchar **lines = g_strsplit(pactl_output, "\n", -1);
    for (gchar **line = lines; *line != NULL; line++) {
        gchar *name = NULL;
        if (!parse_short_sink_line(*line, NULL, &name))
            continue;
        if (sink_name_is_bluetooth(name)) {
            g_free(name);
            continue;
        }

        if (preferred_sink != NULL &&
            g_strcmp0(name, preferred_sink) == 0) {
            g_clear_pointer(&first_alsa, g_free);
            g_clear_pointer(&first_other, g_free);
            g_strfreev(lines);
            return name;
        }

        if (g_str_has_prefix(name, "alsa_output.") && first_alsa == NULL)
            first_alsa = g_strdup(name);
        else if (first_other == NULL)
            first_other = g_strdup(name);
        g_free(name);
    }

    g_strfreev(lines);
    if (first_alsa != NULL) {
        g_free(first_other);
        return first_alsa;
    }
    return first_other;
}

gchar *audio_route_find_sink_index(const char *pactl_output,
                                   const char *sink_name)
{
    if (pactl_output == NULL || sink_name == NULL)
        return NULL;

    gchar **lines = g_strsplit(pactl_output, "\n", -1);
    gchar *result = NULL;
    for (gchar **line = lines; *line != NULL && result == NULL; line++) {
        gchar *index = NULL;
        gchar *name = NULL;
        if (parse_short_sink_line(*line, &index, &name) &&
            g_strcmp0(name, sink_name) == 0) {
            result = g_steal_pointer(&index);
        }
        g_free(index);
        g_free(name);
    }
    g_strfreev(lines);
    return result;
}

GPtrArray *audio_route_parse_sink_input_ids_for_sink(
    const char *pactl_output,
    const char *sink_index)
{
    GPtrArray *ids = g_ptr_array_new_with_free_func(g_free);
    if (pactl_output == NULL || sink_index == NULL)
        return ids;

    gchar **lines = g_strsplit(pactl_output, "\n", -1);
    for (gchar **line = lines; *line != NULL; line++) {
        gchar **fields = g_strsplit(*line, "\t", 3);
        if (fields[0] != NULL && fields[1] != NULL) {
            g_strstrip(fields[0]);
            g_strstrip(fields[1]);
            const gchar *digit = fields[0];
            while (g_ascii_isdigit(*digit))
                digit++;
            if (fields[0][0] != '\0' && *digit == '\0' &&
                g_strcmp0(fields[1], sink_index) == 0) {
                g_ptr_array_add(ids, g_strdup(fields[0]));
            }
        }
        g_strfreev(fields);
    }
    g_strfreev(lines);
    return ids;
}

gboolean audio_route_should_restore_configured(const char *configured_sink,
                                               const char *managed_sink)
{
    return configured_sink != NULL && managed_sink != NULL &&
           g_strcmp0(configured_sink, managed_sink) == 0;
}

static gboolean route_is_current(RouteOperation *operation)
{
    AudioRoute *route = operation->route;
    return operation->generation == route->generation &&
           route->cancellable == operation->cancellable &&
           !g_cancellable_is_cancelled(operation->cancellable);
}

static void route_schedule_retry(AudioRoute *route);
static void route_query_sink(AudioRoute *route);
static void route_list_inputs(AudioRoute *route, const gchar *sink_name);
static void route_set_default(AudioRoute *route, const gchar *sink_name);
static void restore_query_configured(AudioRoute *route,
                                     RouteOperationType type);

static void route_operation_free(RouteOperation *operation)
{
    if (operation->timeout_id > 0)
        g_source_remove(operation->timeout_id);

    g_hash_table_remove(operation->route->active_processes,
                        operation->process);
    g_clear_object(&operation->process);
    g_clear_object(&operation->cancellable);
    g_clear_pointer(&operation->sink_name, g_free);
    g_clear_pointer(&operation->source_sink_index, g_free);
    audio_route_unref(operation->route);
    g_free(operation);
}

static gboolean route_process_timeout_cb(gpointer user_data)
{
    RouteOperation *operation = user_data;
    operation->timeout_id = 0;
    g_subprocess_force_exit(operation->process);
    return G_SOURCE_REMOVE;
}

static void route_operation_finished(GObject *source_object,
                                     GAsyncResult *result,
                                     gpointer user_data);

static gboolean route_spawn(AudioRoute *route,
                            RouteOperationType type,
                            const gchar *const *argv,
                            const gchar *sink_name)
{
    GError *error = NULL;
    GSubprocessFlags flags = G_SUBPROCESS_FLAGS_STDOUT_PIPE |
                             G_SUBPROCESS_FLAGS_STDERR_SILENCE;
    GSubprocessLauncher *launcher = g_subprocess_launcher_new(flags);
    g_subprocess_launcher_setenv(launcher, "LC_ALL", "C", TRUE);
    GSubprocess *process = g_subprocess_launcher_spawnv(launcher,
                                                        argv,
                                                        &error);
    g_object_unref(launcher);
    if (process == NULL) {
        g_debug("Could not start the audio route helper: %s",
                error != NULL ? error->message : "unknown error");
        g_clear_error(&error);
        return FALSE;
    }

    RouteOperation *operation = g_new0(RouteOperation, 1);
    operation->route = audio_route_ref(route);
    operation->generation = route->generation;
    operation->type = type;
    operation->process = process;
    operation->cancellable = g_object_ref(route->cancellable);
    operation->sink_name = g_strdup(sink_name);
    g_hash_table_add(route->active_processes, process);

    operation->timeout_id = g_timeout_add(ROUTE_PROCESS_TIMEOUT_MSEC,
                                          route_process_timeout_cb,
                                          operation);
    g_subprocess_communicate_utf8_async(process,
                                        NULL,
                                        operation->cancellable,
                                        route_operation_finished,
                                        operation);
    return TRUE;
}

static void route_move_inputs(AudioRoute *route,
                              const gchar *sink_name,
                              const gchar *pactl_output)
{
    GPtrArray *ids = audio_route_parse_sink_input_ids(pactl_output);
    for (guint i = 0; i < ids->len; i++) {
        const gchar *argv[] = {
            "pactl", "move-sink-input", g_ptr_array_index(ids, i),
            sink_name, NULL
        };
        route_spawn(route, ROUTE_OPERATION_MOVE_INPUT, argv, NULL);
    }

    if (ids->len > 0)
        g_debug("Queued %u active audio stream(s) for AirPods output", ids->len);
    g_ptr_array_unref(ids);
}

static gboolean route_second_input_pass_cb(gpointer user_data)
{
    AudioRoute *route = user_data;
    route->second_input_pass_timeout_id = 0;

    if (route->cancellable != NULL &&
        !g_cancellable_is_cancelled(route->cancellable) &&
        route->second_input_pass_sink != NULL) {
        route_list_inputs(route, route->second_input_pass_sink);
    }

    g_clear_pointer(&route->second_input_pass_sink, g_free);
    return G_SOURCE_REMOVE;
}

static void route_schedule_second_input_pass(AudioRoute *route,
                                             const gchar *sink_name)
{
    if (route->second_input_pass_timeout_id > 0 || sink_name == NULL ||
        route->cancellable == NULL ||
        g_cancellable_is_cancelled(route->cancellable)) {
        return;
    }

    g_free(route->second_input_pass_sink);
    route->second_input_pass_sink = g_strdup(sink_name);
    route->second_input_pass_timeout_id = g_timeout_add_full(
        G_PRIORITY_DEFAULT,
        ROUTE_SECOND_INPUT_PASS_MSEC,
        route_second_input_pass_cb,
        audio_route_ref(route),
        (GDestroyNotify)audio_route_unref);
}

static gboolean route_resume_sink_cb(gpointer user_data)
{
    AudioRoute *route = user_data;
    route->resume_sink_timeout_id = 0;

    if (route->mode == ROUTE_MODE_SELECTING &&
        route->cancellable != NULL &&
        !g_cancellable_is_cancelled(route->cancellable) &&
        route->resume_sink != NULL) {
        const gchar *argv[] = {
            "pactl", "suspend-sink", route->resume_sink, "0", NULL
        };
        if (!route_spawn(route,
                         ROUTE_OPERATION_RESUME_SINK,
                         argv,
                         route->resume_sink)) {
            route_set_default(route, route->resume_sink);
        }
    }

    g_clear_pointer(&route->resume_sink, g_free);
    return G_SOURCE_REMOVE;
}

static void route_schedule_sink_resume(AudioRoute *route,
                                       const gchar *sink_name)
{
    if (route->resume_sink_timeout_id > 0)
        g_source_remove(route->resume_sink_timeout_id);

    g_free(route->resume_sink);
    route->resume_sink = g_strdup(sink_name);
    route->resume_sink_timeout_id = g_timeout_add_full(
        G_PRIORITY_DEFAULT,
        ROUTE_TRANSPORT_RESTART_MSEC,
        route_resume_sink_cb,
        audio_route_ref(route),
        (GDestroyNotify)audio_route_unref);
}

static void route_restart_sink(AudioRoute *route, const gchar *sink_name)
{
    const gchar *argv[] = {
        "pactl", "suspend-sink", sink_name, "1", NULL
    };
    if (!route_spawn(route,
                     ROUTE_OPERATION_SUSPEND_SINK,
                     argv,
                     sink_name)) {
        route_set_default(route, sink_name);
    }
}

static void route_set_default(AudioRoute *route, const gchar *sink_name)
{
    g_free(route->managed_sink);
    route->managed_sink = g_strdup(sink_name);

    const gchar *argv[] = {
        "pactl", "set-default-sink", sink_name, NULL
    };
    if (!route_spawn(route, ROUTE_OPERATION_SET_DEFAULT, argv, sink_name))
        route_schedule_retry(route);
}

static void route_list_inputs(AudioRoute *route, const gchar *sink_name)
{
    const gchar *argv[] = {
        "pactl", "list", "sink-inputs", "short", NULL
    };
    route_spawn(route, ROUTE_OPERATION_LIST_INPUTS, argv, sink_name);
}

static void route_capture_configured_default(AudioRoute *route,
                                             const gchar *sink_name)
{
    const gchar *argv[] = {"pw-metadata", "-n", "default", NULL};
    if (!route_spawn(route,
                     ROUTE_OPERATION_CAPTURE_CONFIGURED_DEFAULT,
                     argv,
                     sink_name)) {
        if (route->previous_default_sink != NULL)
            route_set_default(route, sink_name);
        else
            route_schedule_retry(route);
    }
}

static void route_capture_current_default(AudioRoute *route,
                                          const gchar *sink_name)
{
    const gchar *argv[] = {"pactl", "get-default-sink", NULL};
    if (!route_spawn(route,
                     ROUTE_OPERATION_CAPTURE_CURRENT_DEFAULT,
                     argv,
                     sink_name)) {
        route_schedule_retry(route);
    }
}

static void restore_finish(AudioRoute *route, gboolean relinquish_ownership)
{
    if (route->retry_timeout_id > 0) {
        g_source_remove(route->retry_timeout_id);
        route->retry_timeout_id = 0;
    }
    route->mode = ROUTE_MODE_IDLE;
    route->deadline_usec = 0;
    g_clear_pointer(&route->restore_sink, g_free);
    g_clear_pointer(&route->restore_managed_index, g_free);
    if (relinquish_ownership) {
        g_clear_pointer(&route->managed_sink, g_free);
        g_clear_pointer(&route->previous_default_sink, g_free);
    }
}

static void restore_list_sinks(AudioRoute *route)
{
    const gchar *argv[] = {"pactl", "list", "sinks", "short", NULL};
    if (!route_spawn(route,
                     ROUTE_OPERATION_RESTORE_LIST_SINKS,
                     argv,
                     NULL)) {
        route_schedule_retry(route);
    }
}

static void restore_query_configured(AudioRoute *route,
                                     RouteOperationType type)
{
    const gchar *argv[] = {"pw-metadata", "-n", "default", NULL};
    if (!route_spawn(route, type, argv, NULL))
        route_schedule_retry(route);
}

static void restore_set_default(AudioRoute *route)
{
    const gchar *argv[] = {
        "pactl", "set-default-sink", route->restore_sink, NULL
    };
    if (!route_spawn(route,
                     ROUTE_OPERATION_RESTORE_SET_DEFAULT,
                     argv,
                     route->restore_sink)) {
        route_schedule_retry(route);
    }
}

static void restore_list_inputs(AudioRoute *route)
{
    const gchar *argv[] = {
        "pactl", "list", "sink-inputs", "short", NULL
    };
    if (!route_spawn(route,
                     ROUTE_OPERATION_RESTORE_LIST_INPUTS,
                     argv,
                     route->restore_sink)) {
        restore_finish(route, TRUE);
    }
}

static void restore_move_inputs(AudioRoute *route,
                                const gchar *destination_sink,
                                const gchar *source_sink_index,
                                const gchar *pactl_output)
{
    GPtrArray *ids = audio_route_parse_sink_input_ids_for_sink(
        pactl_output, source_sink_index);
    for (guint i = 0; i < ids->len; i++) {
        const gchar *argv[] = {
            "pactl", "move-sink-input", g_ptr_array_index(ids, i),
            destination_sink, NULL
        };
        route_spawn(route, ROUTE_OPERATION_RESTORE_MOVE_INPUT, argv, NULL);
    }
    if (ids->len > 0) {
        g_debug("Queued %u AirPods audio stream(s) for restored output",
                ids->len);
    }
    g_ptr_array_unref(ids);
}

static void route_operation_finished(GObject *source_object,
                                     GAsyncResult *result,
                                     gpointer user_data)
{
    RouteOperation *operation = user_data;
    AudioRoute *route = operation->route;
    gchar *standard_output = NULL;
    GError *error = NULL;
    gboolean communicated = g_subprocess_communicate_utf8_finish(
        G_SUBPROCESS(source_object), result, &standard_output, NULL, &error);
    gboolean successful = communicated &&
                          g_subprocess_get_successful(operation->process);
    gboolean current = route_is_current(operation);

    if (current && successful) {
        switch (operation->type) {
        case ROUTE_OPERATION_FIND_SINK: {
            gchar *sink_name = find_sink_for_address(standard_output,
                                                     route->device_address);
            if (sink_name != NULL) {
                route_capture_current_default(route, sink_name);
                g_free(sink_name);
            } else {
                route_schedule_retry(route);
            }
            break;
        }
        case ROUTE_OPERATION_CAPTURE_CURRENT_DEFAULT: {
            gchar *current_default = audio_route_parse_default_sink(
                standard_output);
            if (current_default != NULL &&
                !sink_name_is_bluetooth(current_default)) {
                g_free(route->previous_default_sink);
                route->previous_default_sink = g_steal_pointer(
                    &current_default);
            }
            g_free(current_default);
            route_capture_configured_default(route,
                                             operation->sink_name);
            break;
        }
        case ROUTE_OPERATION_CAPTURE_CONFIGURED_DEFAULT: {
            gchar *configured = audio_route_parse_configured_sink(
                standard_output);
            if (configured != NULL &&
                !sink_name_is_bluetooth(configured)) {
                g_free(route->previous_default_sink);
                route->previous_default_sink = g_steal_pointer(&configured);
            }
            g_free(configured);
            /* Restart the A2DP transport after an Apple host yielded it.
             * Selecting and moving streams alone does not reliably reopen
             * the transport on BlueZ/PipeWire. Keep the proven delay off the
             * GLib main thread so wear and handoff packets remain responsive. */
            route_restart_sink(route, operation->sink_name);
            break;
        }
        case ROUTE_OPERATION_SUSPEND_SINK:
            route_schedule_sink_resume(route, operation->sink_name);
            break;
        case ROUTE_OPERATION_RESUME_SINK:
            route_set_default(route, operation->sink_name);
            break;
        case ROUTE_OPERATION_SET_DEFAULT:
            g_message("Selected AirPods as the default audio output");
            route->mode = ROUTE_MODE_IDLE;
            route_list_inputs(route, operation->sink_name);
            route_schedule_second_input_pass(route, operation->sink_name);
            break;
        case ROUTE_OPERATION_LIST_INPUTS:
            route_move_inputs(route, operation->sink_name, standard_output);
            break;
        case ROUTE_OPERATION_MOVE_INPUT:
            break;
        case ROUTE_OPERATION_RESTORE_CHECK_CONFIGURED: {
            gchar *configured = audio_route_parse_configured_sink(
                standard_output);
            if (audio_route_should_restore_configured(
                    configured, route->managed_sink)) {
                restore_list_sinks(route);
            } else {
                g_debug("Audio default changed outside EarPort; not restoring it");
                restore_finish(route, TRUE);
            }
            g_free(configured);
            break;
        }
        case ROUTE_OPERATION_RESTORE_LIST_SINKS:
            g_free(route->restore_sink);
            route->restore_sink = audio_route_find_restore_sink(
                standard_output, route->previous_default_sink);
            g_free(route->restore_managed_index);
            route->restore_managed_index = audio_route_find_sink_index(
                standard_output, route->managed_sink);
            if (route->restore_sink != NULL) {
                restore_query_configured(
                    route, ROUTE_OPERATION_RESTORE_RECHECK_CONFIGURED);
            } else {
                route_schedule_retry(route);
            }
            break;
        case ROUTE_OPERATION_RESTORE_RECHECK_CONFIGURED: {
            gchar *configured = audio_route_parse_configured_sink(
                standard_output);
            if (audio_route_should_restore_configured(
                    configured, route->managed_sink)) {
                restore_set_default(route);
            } else {
                g_debug("Audio default changed during restore; leaving it untouched");
                restore_finish(route, TRUE);
            }
            g_free(configured);
            break;
        }
        case ROUTE_OPERATION_RESTORE_SET_DEFAULT:
            g_message("Restored the previous non-Bluetooth audio output");
            if (route->restore_managed_index != NULL)
                restore_list_inputs(route);
            else
                restore_finish(route, TRUE);
            break;
        case ROUTE_OPERATION_RESTORE_LIST_INPUTS:
            restore_move_inputs(route,
                                operation->sink_name,
                                route->restore_managed_index,
                                standard_output);
            restore_finish(route, TRUE);
            break;
        case ROUTE_OPERATION_RESTORE_MOVE_INPUT:
            break;
        }
    } else if (current) {
        switch (operation->type) {
        case ROUTE_OPERATION_CAPTURE_CONFIGURED_DEFAULT:
            if (route->previous_default_sink != NULL)
                route_restart_sink(route, operation->sink_name);
            else
                route_schedule_retry(route);
            break;
        case ROUTE_OPERATION_SUSPEND_SINK:
        case ROUTE_OPERATION_RESUME_SINK:
            /* Routing is still useful if PipeWire rejects an explicit
             * suspend/resume (for example while the transport is changing). */
            route_set_default(route, operation->sink_name);
            break;
        case ROUTE_OPERATION_FIND_SINK:
        case ROUTE_OPERATION_CAPTURE_CURRENT_DEFAULT:
        case ROUTE_OPERATION_SET_DEFAULT:
        case ROUTE_OPERATION_RESTORE_CHECK_CONFIGURED:
        case ROUTE_OPERATION_RESTORE_LIST_SINKS:
        case ROUTE_OPERATION_RESTORE_RECHECK_CONFIGURED:
        case ROUTE_OPERATION_RESTORE_SET_DEFAULT:
            route_schedule_retry(route);
            break;
        case ROUTE_OPERATION_RESTORE_LIST_INPUTS:
            restore_finish(route, TRUE);
            break;
        case ROUTE_OPERATION_LIST_INPUTS:
        case ROUTE_OPERATION_MOVE_INPUT:
        case ROUTE_OPERATION_RESTORE_MOVE_INPUT:
            break;
        }
    }

    if (!communicated && error != NULL &&
        !g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
        g_debug("Audio route helper did not complete: %s", error->message);
    }

    g_clear_error(&error);
    g_free(standard_output);
    route_operation_free(operation);
}

static gboolean route_retry_timeout_cb(gpointer user_data)
{
    AudioRoute *route = user_data;
    route->retry_timeout_id = 0;
    if (route->mode == ROUTE_MODE_SELECTING) {
        route_query_sink(route);
    } else if (route->mode == ROUTE_MODE_RESTORING) {
        restore_query_configured(
            route, ROUTE_OPERATION_RESTORE_CHECK_CONFIGURED);
    }
    return G_SOURCE_REMOVE;
}

static void route_schedule_retry(AudioRoute *route)
{
    if (route->retry_timeout_id > 0 || route->cancellable == NULL ||
        g_cancellable_is_cancelled(route->cancellable)) {
        return;
    }

    if (g_get_monotonic_time() >= route->deadline_usec) {
        if (route->mode == ROUTE_MODE_RESTORING) {
            g_warning("Previous audio output could not be restored within the route window");
            /* Retain ownership metadata so a later release trigger can retry. */
            restore_finish(route, FALSE);
        } else {
            g_debug("AirPods audio sink did not become ready within the route window");
            route->mode = ROUTE_MODE_IDLE;
        }
        return;
    }

    route->retry_timeout_id = g_timeout_add_full(
        G_PRIORITY_DEFAULT,
        ROUTE_RETRY_INTERVAL_MSEC,
        route_retry_timeout_cb,
        audio_route_ref(route),
        (GDestroyNotify)audio_route_unref);
}

static void route_query_sink(AudioRoute *route)
{
    if (route->mode != ROUTE_MODE_SELECTING ||
        route->cancellable == NULL ||
        g_cancellable_is_cancelled(route->cancellable) ||
        g_get_monotonic_time() >= route->deadline_usec) {
        return;
    }

    const gchar *argv[] = {"pactl", "list", "sinks", NULL};
    if (!route_spawn(route, ROUTE_OPERATION_FIND_SINK, argv, NULL))
        route_schedule_retry(route);
}

AudioRoute *audio_route_new(void)
{
    AudioRoute *route = g_new0(AudioRoute, 1);
    route->ref_count = 1;
    route->active_processes = g_hash_table_new(g_direct_hash, g_direct_equal);
    return route;
}

void audio_route_cancel(AudioRoute *route)
{
    if (route == NULL)
        return;

    route->generation++;
    if (route->retry_timeout_id > 0) {
        g_source_remove(route->retry_timeout_id);
        route->retry_timeout_id = 0;
    }
    if (route->second_input_pass_timeout_id > 0) {
        g_source_remove(route->second_input_pass_timeout_id);
        route->second_input_pass_timeout_id = 0;
    }
    if (route->resume_sink_timeout_id > 0) {
        g_source_remove(route->resume_sink_timeout_id);
        route->resume_sink_timeout_id = 0;
    }

    if (route->cancellable != NULL)
        g_cancellable_cancel(route->cancellable);

    GHashTableIter iter;
    gpointer process;
    g_hash_table_iter_init(&iter, route->active_processes);
    while (g_hash_table_iter_next(&iter, &process, NULL))
        g_subprocess_force_exit(G_SUBPROCESS(process));

    g_clear_object(&route->cancellable);
    g_clear_pointer(&route->device_address, g_free);
    g_clear_pointer(&route->second_input_pass_sink, g_free);
    g_clear_pointer(&route->resume_sink, g_free);
    g_clear_pointer(&route->restore_sink, g_free);
    g_clear_pointer(&route->restore_managed_index, g_free);
    route->deadline_usec = 0;
    route->mode = ROUTE_MODE_IDLE;
}

gboolean audio_route_start(AudioRoute *route, const char *device_address)
{
    if (route == NULL || !valid_device_address(device_address))
        return FALSE;

    audio_route_cancel(route);
    route->device_address = g_strdup(device_address);
    route->cancellable = g_cancellable_new();
    route->mode = ROUTE_MODE_SELECTING;
    route->deadline_usec = g_get_monotonic_time() +
                           ROUTE_DEADLINE_MSEC * G_TIME_SPAN_MILLISECOND;
    route_query_sink(route);
    return TRUE;
}

gboolean audio_route_restore_previous(AudioRoute *route)
{
    if (route == NULL)
        return FALSE;
    if (route->mode == ROUTE_MODE_RESTORING)
        return TRUE;

    audio_route_cancel(route);
    if (route->managed_sink == NULL)
        return FALSE;

    route->cancellable = g_cancellable_new();
    route->mode = ROUTE_MODE_RESTORING;
    route->deadline_usec = g_get_monotonic_time() +
                           ROUTE_DEADLINE_MSEC * G_TIME_SPAN_MILLISECOND;
    restore_query_configured(route,
                             ROUTE_OPERATION_RESTORE_CHECK_CONFIGURED);
    return TRUE;
}

void audio_route_free(AudioRoute *route)
{
    if (route == NULL)
        return;

    audio_route_cancel(route);
    g_clear_pointer(&route->managed_sink, g_free);
    g_clear_pointer(&route->previous_default_sink, g_free);
    audio_route_unref(route);
}
