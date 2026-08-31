/* SPDX-License-Identifier: GPL-3.0-or-later */

#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>

#include "config.h"

static gchar *read_file(const char *path)
{
    gchar *contents = NULL;
    g_assert_true(g_file_get_contents(path, &contents, NULL, NULL));
    return contents;
}

static void test_display_name_preserves_utf8_boundary(void)
{
    /* 61 ASCII bytes followed by a four-byte emoji crosses the 63-byte payload
     * limit. The partial emoji must be omitted, never byte-truncated. */
    char source[80];
    memset(source, 'A', 61);
    memcpy(source + 61, "\xF0\x9F\x8E\xA7", 4);
    source[65] = 'Z';
    source[66] = '\0';

    char destination[DEVICE_PROFILE_DISPLAY_NAME_SIZE];
    config_copy_display_name(destination, source);
    g_assert_true(g_utf8_validate(destination, -1, NULL));
    g_assert_cmpuint(strlen(destination), ==, 61);
    g_assert_cmpmem(destination, 61, source, 61);

    /* A multibyte code point that ends exactly at the boundary is retained. */
    memset(source, 'B', 59);
    memcpy(source + 59, "\xF0\x9F\x8E\xA7", 4);
    source[63] = 'Z';
    source[64] = '\0';
    config_copy_display_name(destination, source);
    g_assert_true(g_utf8_validate(destination, -1, NULL));
    g_assert_cmpuint(strlen(destination), ==, 63);
}

static void test_config_preserves_existing_files(void)
{
    const char legacy_daemon[] =
        "[Settings]\n"
        "ear_pause_mode=0\n"
        "handoff_enabled=false\n";
    const char legacy_devices[] = "[legacy]\nname=keep-me\n";
    const char current_devices[] = "[current]\nname=do-not-replace\n";
    const char malformed_daemon[] = "[Settings\near_pause_mode=1\n";
    const char malformed_devices[] = "[broken\nvalue=1\n";

    gchar *config_root = g_dir_make_tmp("earport-config-test-XXXXXX", NULL);
    g_assert_nonnull(config_root);
    g_setenv("XDG_CONFIG_HOME", config_root, TRUE);

    gchar *legacy_dir = g_build_filename(config_root, "librepods", NULL);
    gchar *current_dir = g_build_filename(config_root, "earport", NULL);
    g_assert_cmpint(g_mkdir(legacy_dir, 0700), ==, 0);
    g_assert_cmpint(g_mkdir(current_dir, 0700), ==, 0);

    gchar *legacy_daemon_path = g_build_filename(legacy_dir, "daemon.conf", NULL);
    gchar *legacy_devices_path = g_build_filename(legacy_dir, "devices.conf", NULL);
    gchar *current_daemon_path = g_build_filename(current_dir, "daemon.conf", NULL);
    gchar *current_devices_path = g_build_filename(current_dir, "devices.conf", NULL);

    g_assert_true(g_file_set_contents(legacy_daemon_path, legacy_daemon, -1, NULL));
    g_assert_true(g_file_set_contents(legacy_devices_path, legacy_devices, -1, NULL));
    g_assert_true(g_file_set_contents(current_devices_path, current_devices, -1, NULL));

    EarPortConfig config;
    g_assert_true(config_load(&config));
    g_assert_cmpint(config.ear_pause_mode, ==, 0);
    g_assert_false(config.handoff_enabled);

    gchar *contents = read_file(current_daemon_path);
    g_assert_cmpstr(contents, ==, legacy_daemon);
    g_free(contents);
    contents = read_file(current_devices_path);
    g_assert_cmpstr(contents, ==, current_devices);
    g_free(contents);
    contents = read_file(legacy_devices_path);
    g_assert_cmpstr(contents, ==, legacy_devices);
    g_free(contents);

    /* A parse error returns defaults for this run, but keeps the user's file
     * byte-for-byte so it can be repaired. */
    g_assert_true(g_file_set_contents(current_daemon_path,
                                      malformed_daemon, -1, NULL));
    g_test_expect_message(NULL, G_LOG_LEVEL_WARNING,
                          "*Failed to load config file*");
    g_assert_false(config_load(&config));
    g_test_assert_expected_messages();
    contents = read_file(current_daemon_path);
    g_assert_cmpstr(contents, ==, malformed_daemon);
    g_free(contents);

    /* Explicit per-device saves likewise refuse to destroy a malformed
     * devices.conf file. */
    g_assert_true(g_file_set_contents(current_devices_path,
                                      malformed_devices, -1, NULL));
    DeviceProfile profile;
    config_get_default_profile(&profile);
    g_test_expect_message(NULL, G_LOG_LEVEL_WARNING,
                          "*Refusing to overwrite devices config*");
    g_assert_false(config_save_device_profile("AA:BB:CC:DD:EE:FF", &profile));
    g_test_assert_expected_messages();
    contents = read_file(current_devices_path);
    g_assert_cmpstr(contents, ==, malformed_devices);
    g_free(contents);

    /* Only ENOENT causes a default daemon.conf to be created. */
    g_assert_cmpint(g_remove(current_daemon_path), ==, 0);
    g_assert_true(config_load(&config));
    g_assert_true(g_file_test(current_daemon_path, G_FILE_TEST_IS_REGULAR));

    g_assert_cmpint(g_remove(current_daemon_path), ==, 0);
    g_assert_cmpint(g_remove(current_devices_path), ==, 0);
    g_assert_cmpint(g_remove(legacy_devices_path), ==, 0);
    g_assert_cmpint(g_rmdir(current_dir), ==, 0);
    g_assert_cmpint(g_rmdir(legacy_dir), ==, 0);
    g_assert_cmpint(g_rmdir(config_root), ==, 0);

    g_free(current_devices_path);
    g_free(current_daemon_path);
    g_free(legacy_devices_path);
    g_free(legacy_daemon_path);
    g_free(current_dir);
    g_free(legacy_dir);
    g_free(config_root);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/config/display-name-utf8-boundary",
                    test_display_name_preserves_utf8_boundary);
    g_test_add_func("/config/preserves-existing-files",
                    test_config_preserves_existing_files);
    return g_test_run();
}
