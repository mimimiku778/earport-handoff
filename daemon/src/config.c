/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2024 EarPort Contributors
 *
 * Configuration file management
 */

#include "config.h"
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include <gio/gio.h>
#include <glib/gstdio.h>

#define CONFIG_DIR_NAME "earport"
#define LEGACY_CONFIG_DIR_NAME "librepods"  /* Pre-rename directory */
#define CONFIG_FILE_NAME "daemon.conf"
#define CONFIG_GROUP "Settings"

static gchar *get_config_dir(void)
{
    const gchar *config_home = g_get_user_config_dir();
    return g_build_filename(config_home, CONFIG_DIR_NAME, NULL);
}

static bool ensure_config_dir(void);

/* One-time migration of the configuration files from the project's former
 * name. The new directory may already exist but empty (systemd's
 * ConfigurationDirectory= pre-creates it), so migrate file by file as long
 * as the new config has not been written yet. */
static void migrate_legacy_config_dir(void)
{
    static bool migration_checked = false;
    static const char *config_files[] = {CONFIG_FILE_NAME, "devices.conf", NULL};

    if (migration_checked)
        return;
    migration_checked = true;

    gchar *new_dir = get_config_dir();
    gchar *old_dir = g_build_filename(g_get_user_config_dir(),
                                      LEGACY_CONFIG_DIR_NAME, NULL);
    if (g_file_test(old_dir, G_FILE_TEST_IS_DIR)) {
        if (!ensure_config_dir())
            goto out;

        for (int i = 0; config_files[i] != NULL; i++) {
            gchar *src = g_build_filename(old_dir, config_files[i], NULL);
            gchar *dst = g_build_filename(new_dir, config_files[i], NULL);
            if (g_file_test(src, G_FILE_TEST_EXISTS)) {
                GFile *source = g_file_new_for_path(src);
                GFile *destination = g_file_new_for_path(dst);
                GError *error = NULL;

                /* G_FILE_COPY_NONE deliberately omits OVERWRITE: unlike a
                 * check followed by rename(2), this remains no-clobber if a
                 * destination appears concurrently. */
                if (g_file_move(source, destination, G_FILE_COPY_NONE,
                                NULL, NULL, NULL, &error)) {
                    g_message("Migrated %s to %s", src, dst);
                } else if (!g_error_matches(error, G_IO_ERROR,
                                            G_IO_ERROR_EXISTS)) {
                    g_warning("Failed to migrate %s: %s", src,
                              error ? error->message : "unknown error");
                } else {
                    g_debug("Keeping existing config file %s", dst);
                }

                g_clear_error(&error);
                g_object_unref(destination);
                g_object_unref(source);
            }
            g_free(src);
            g_free(dst);
        }

        /* Remove the old directory if now empty */
        g_rmdir(old_dir);
    }

out:
    g_free(old_dir);
    g_free(new_dir);
}

static gchar *get_config_path(void)
{
    gchar *config_dir = get_config_dir();
    gchar *config_path = g_build_filename(config_dir, CONFIG_FILE_NAME, NULL);
    g_free(config_dir);
    return config_path;
}

static bool ensure_config_dir(void)
{
    gchar *config_dir = get_config_dir();
    int result = g_mkdir_with_parents(config_dir, 0700);
    g_free(config_dir);

    if (result != 0 && errno != EEXIST) {
        g_warning("Failed to create config directory: %s", g_strerror(errno));
        return false;
    }

    return true;
}

void config_get_defaults(EarPortConfig *config)
{
    config->ear_pause_mode = 2;  /* EAR_PAUSE_BOTH_OUT; supports one-bud use */
    config->handoff_enabled = true;
    config->auto_connect_on_wear = true;
    config->disconnect_on_removal = true;
}

bool config_load(EarPortConfig *config)
{
    /* Start with defaults */
    config_get_defaults(config);

    migrate_legacy_config_dir();

    gchar *config_path = get_config_path();
    GKeyFile *keyfile = g_key_file_new();
    GError *error = NULL;

    if (!g_key_file_load_from_file(keyfile, config_path, G_KEY_FILE_NONE, &error)) {
        bool missing = error != NULL &&
                       g_error_matches(error, G_FILE_ERROR,
                                       G_FILE_ERROR_NOENT);
        if (!missing)
            g_warning("Failed to load config file: %s",
                      error ? error->message : "unknown error");

        g_clear_error(&error);
        g_key_file_free(keyfile);
        g_free(config_path);

        /* A missing file is the only safe case in which to create defaults.
         * A malformed or unreadable file may contain the user's only copy of
         * their settings and must never be overwritten implicitly. */
        return missing ? config_save(config) : false;
    }

    /* Read settings */
    if (g_key_file_has_key(keyfile, CONFIG_GROUP, "ear_pause_mode", NULL)) {
        config->ear_pause_mode = g_key_file_get_integer(keyfile, CONFIG_GROUP, "ear_pause_mode", NULL);

        /* Validate range */
        if (config->ear_pause_mode < 0 || config->ear_pause_mode > 2) {
            config->ear_pause_mode = 2;
        }
    }

    if (g_key_file_has_key(keyfile, CONFIG_GROUP, "handoff_enabled", NULL)) {
        config->handoff_enabled = g_key_file_get_boolean(
            keyfile, CONFIG_GROUP, "handoff_enabled", NULL);
    }

    if (g_key_file_has_key(keyfile, CONFIG_GROUP, "auto_connect_on_wear", NULL)) {
        config->auto_connect_on_wear = g_key_file_get_boolean(
            keyfile, CONFIG_GROUP, "auto_connect_on_wear", NULL);
    }

    if (g_key_file_has_key(keyfile, CONFIG_GROUP, "disconnect_on_removal", NULL)) {
        config->disconnect_on_removal = g_key_file_get_boolean(
            keyfile, CONFIG_GROUP, "disconnect_on_removal", NULL);
    }

    g_message("Config loaded: ear_pause_mode=%d, handoff_enabled=%s, auto_connect_on_wear=%s, disconnect_on_removal=%s",
              config->ear_pause_mode,
              config->handoff_enabled ? "true" : "false",
              config->auto_connect_on_wear ? "true" : "false",
              config->disconnect_on_removal ? "true" : "false");

    g_key_file_free(keyfile);
    g_free(config_path);
    return true;
}

bool config_save(const EarPortConfig *config)
{
    if (!ensure_config_dir()) {
        return false;
    }

    GKeyFile *keyfile = g_key_file_new();

    /* Write settings */
    g_key_file_set_integer(keyfile, CONFIG_GROUP, "ear_pause_mode", config->ear_pause_mode);
    g_key_file_set_boolean(keyfile, CONFIG_GROUP, "handoff_enabled", config->handoff_enabled);
    g_key_file_set_boolean(keyfile, CONFIG_GROUP, "auto_connect_on_wear",
                           config->auto_connect_on_wear);
    g_key_file_set_boolean(keyfile, CONFIG_GROUP, "disconnect_on_removal",
                           config->disconnect_on_removal);

    /* Add comment */
    g_key_file_set_comment(keyfile, CONFIG_GROUP, NULL,
                           "EarPort daemon configuration\n"
                           "ear_pause_mode: 0=disabled, 1=pause when one removed, 2=pause when both removed\n"
                           "handoff_enabled: claim AirPods audio when Linux playback starts\n"
                           "auto_connect_on_wear: scan BLE and connect a uniquely matched paired AirPods device when worn\n"
                           "disconnect_on_removal: disconnect BlueZ after both AAP wear slots stay out for one second",
                           NULL);

    gchar *config_path = get_config_path();
    GError *error = NULL;

    if (!g_key_file_save_to_file(keyfile, config_path, &error)) {
        g_warning("Failed to save config file: %s", error->message);
        g_error_free(error);
        g_key_file_free(keyfile);
        g_free(config_path);
        return false;
    }

    g_message("Config saved: ear_pause_mode=%d, handoff_enabled=%s, auto_connect_on_wear=%s, disconnect_on_removal=%s",
              config->ear_pause_mode,
              config->handoff_enabled ? "true" : "false",
              config->auto_connect_on_wear ? "true" : "false",
              config->disconnect_on_removal ? "true" : "false");

    g_key_file_free(keyfile);
    g_free(config_path);
    return true;
}

#define DEVICES_FILE_NAME "devices.conf"

static gchar *get_devices_config_path(void)
{
    gchar *config_dir = get_config_dir();
    gchar *config_path = g_build_filename(config_dir, DEVICES_FILE_NAME, NULL);
    g_free(config_dir);
    return config_path;
}

static bool load_existing_key_file_for_update(GKeyFile *keyfile,
                                              const char *config_path)
{
    GError *error = NULL;
    if (g_key_file_load_from_file(keyfile, config_path,
                                  G_KEY_FILE_KEEP_COMMENTS, &error)) {
        return true;
    }

    if (error != NULL &&
        g_error_matches(error, G_FILE_ERROR, G_FILE_ERROR_NOENT)) {
        g_clear_error(&error);
        return true;
    }

    g_warning("Refusing to overwrite devices config: %s",
              error ? error->message : "unknown error");
    g_clear_error(&error);
    return false;
}

/* Convert MAC address to group name (replace : with _) */
static gchar *address_to_group(const char *address)
{
    gchar *group = g_strdup(address);
    for (gchar *p = group; *p; p++) {
        if (*p == ':') *p = '_';
    }
    return group;
}

/* ============================================================================
 * Complete device profile management
 * ========================================================================== */

void config_copy_display_name(
    char destination[DEVICE_PROFILE_DISPLAY_NAME_SIZE],
    const char *source)
{
    g_return_if_fail(destination != NULL);

    if (source == NULL || source[0] == '\0') {
        destination[0] = '\0';
        return;
    }

    gchar *valid = g_utf8_make_valid(source, -1);
    gsize valid_len = strlen(valid);
    gsize copy_len = MIN(valid_len,
                         (gsize)DEVICE_PROFILE_DISPLAY_NAME_SIZE - 1);
    if (copy_len < valid_len) {
        const gchar *valid_end = NULL;
        if (!g_utf8_validate(valid, (gssize)copy_len, &valid_end))
            copy_len = (gsize)(valid_end - valid);
    }

    memcpy(destination, valid, copy_len);
    destination[copy_len] = '\0';
    g_free(valid);
}

void config_get_default_profile(DeviceProfile *profile)
{
    memset(profile, 0, sizeof(DeviceProfile));

    /* Empty display_name means use device model */
    profile->display_name[0] = '\0';

    /* Default listening modes (like Apple defaults) */
    profile->listening_modes.off_enabled = false;
    profile->listening_modes.transparency_enabled = true;
    profile->listening_modes.anc_enabled = true;
    profile->listening_modes.adaptive_enabled = false;

    /* Default feature settings */
    profile->conversational_awareness = false;
    profile->adaptive_noise_level = 50;

    profile->has_saved_settings = false;
}

bool config_load_device_profile(const char *device_address, DeviceProfile *profile)
{
    /* Start with defaults */
    config_get_default_profile(profile);

    if (device_address == NULL || device_address[0] == '\0') {
        return false;
    }

    gchar *config_path = get_devices_config_path();
    GKeyFile *keyfile = g_key_file_new();
    GError *error = NULL;

    if (!g_key_file_load_from_file(keyfile, config_path, G_KEY_FILE_NONE, &error)) {
        if (error != NULL) {
            if (!g_error_matches(error, G_FILE_ERROR, G_FILE_ERROR_NOENT)) {
                g_warning("Failed to load devices config: %s", error->message);
            }
            g_error_free(error);
        }
        g_key_file_free(keyfile);
        g_free(config_path);
        return false;
    }

    gchar *group = address_to_group(device_address);

    if (!g_key_file_has_group(keyfile, group)) {
        g_key_file_free(keyfile);
        g_free(config_path);
        g_free(group);
        return false;
    }

    /* Read display name */
    if (g_key_file_has_key(keyfile, group, "display_name", NULL)) {
        gchar *name = g_key_file_get_string(keyfile, group, "display_name", NULL);
        if (name) {
            config_copy_display_name(profile->display_name, name);
            g_free(name);
        }
    }

    /* Read listening modes */
    if (g_key_file_has_key(keyfile, group, "listening_mode_off", NULL)) {
        profile->listening_modes.off_enabled = g_key_file_get_boolean(keyfile, group, "listening_mode_off", NULL);
    }
    if (g_key_file_has_key(keyfile, group, "listening_mode_transparency", NULL)) {
        profile->listening_modes.transparency_enabled = g_key_file_get_boolean(keyfile, group, "listening_mode_transparency", NULL);
    }
    if (g_key_file_has_key(keyfile, group, "listening_mode_anc", NULL)) {
        profile->listening_modes.anc_enabled = g_key_file_get_boolean(keyfile, group, "listening_mode_anc", NULL);
    }
    if (g_key_file_has_key(keyfile, group, "listening_mode_adaptive", NULL)) {
        profile->listening_modes.adaptive_enabled = g_key_file_get_boolean(keyfile, group, "listening_mode_adaptive", NULL);
    }

    /* Read feature settings */
    if (g_key_file_has_key(keyfile, group, "conversational_awareness", NULL)) {
        profile->conversational_awareness = g_key_file_get_boolean(keyfile, group, "conversational_awareness", NULL);
    }
    if (g_key_file_has_key(keyfile, group, "adaptive_noise_level", NULL)) {
        profile->adaptive_noise_level = g_key_file_get_integer(keyfile, group, "adaptive_noise_level", NULL);
        /* Validate range */
        if (profile->adaptive_noise_level < 0) profile->adaptive_noise_level = 0;
        if (profile->adaptive_noise_level > 100) profile->adaptive_noise_level = 100;
    }
    /* Check if this profile has been explicitly saved */
    if (g_key_file_has_key(keyfile, group, "has_saved_settings", NULL)) {
        profile->has_saved_settings = g_key_file_get_boolean(keyfile, group, "has_saved_settings", NULL);
    }

    g_message("Loaded profile for %s: display_name='%s', ca=%d, adaptive_level=%d",
              device_address, profile->display_name,
              profile->conversational_awareness, profile->adaptive_noise_level);

    g_key_file_free(keyfile);
    g_free(config_path);
    g_free(group);
    return true;
}

bool config_save_device_profile(const char *device_address, const DeviceProfile *profile)
{
    if (device_address == NULL || device_address[0] == '\0') {
        g_warning("Cannot save profile: no device address");
        return false;
    }

    if (!ensure_config_dir()) {
        return false;
    }

    gchar *config_path = get_devices_config_path();
    GKeyFile *keyfile = g_key_file_new();

    /* Preserve other device profiles, and never replace a malformed or
     * unreadable file with a partial one. */
    if (!load_existing_key_file_for_update(keyfile, config_path)) {
        g_key_file_free(keyfile);
        g_free(config_path);
        return false;
    }

    gchar *group = address_to_group(device_address);

    /* Write only valid, bounded UTF-8 even if a future caller constructs the
     * fixed profile field without going through config_copy_display_name(). */
    gchar *raw_display_name = g_strndup(
        profile->display_name, DEVICE_PROFILE_DISPLAY_NAME_SIZE - 1);
    char display_name[DEVICE_PROFILE_DISPLAY_NAME_SIZE];
    config_copy_display_name(display_name, raw_display_name);
    g_free(raw_display_name);
    g_key_file_set_string(keyfile, group, "display_name", display_name);

    /* Write listening modes */
    g_key_file_set_boolean(keyfile, group, "listening_mode_off", profile->listening_modes.off_enabled);
    g_key_file_set_boolean(keyfile, group, "listening_mode_transparency", profile->listening_modes.transparency_enabled);
    g_key_file_set_boolean(keyfile, group, "listening_mode_anc", profile->listening_modes.anc_enabled);
    g_key_file_set_boolean(keyfile, group, "listening_mode_adaptive", profile->listening_modes.adaptive_enabled);

    /* Write feature settings */
    g_key_file_set_boolean(keyfile, group, "conversational_awareness", profile->conversational_awareness);
    g_key_file_set_integer(keyfile, group, "adaptive_noise_level",
                           CLAMP(profile->adaptive_noise_level, 0, 100));
    g_key_file_set_boolean(keyfile, group, "has_saved_settings", true);

    GError *error = NULL;
    if (!g_key_file_save_to_file(keyfile, config_path, &error)) {
        g_warning("Failed to save device profile: %s", error->message);
        g_error_free(error);
        g_key_file_free(keyfile);
        g_free(config_path);
        g_free(group);
        return false;
    }

    g_message("Saved profile for %s: display_name='%s', ca=%d, adaptive_level=%d",
              device_address, display_name,
              profile->conversational_awareness, profile->adaptive_noise_level);

    g_key_file_free(keyfile);
    g_free(config_path);
    g_free(group);
    return true;
}
