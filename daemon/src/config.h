/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2024 EarPort Contributors
 *
 * Configuration file management
 */

#ifndef CONFIG_H
#define CONFIG_H

#include <glib.h>
#include <stdbool.h>
#include "airpods_state.h"

/* Configuration data structure */
typedef struct {
    int ear_pause_mode;   /* 0=disabled, 1=one_out, 2=both_out */
    bool handoff_enabled; /* Claim/release AudioSource based on MPRIS playback */
    bool auto_connect_on_wear; /* Connect paired AirPods after confirmed BLE wear */
    bool disconnect_on_removal; /* Disconnect BlueZ after both wear slots stay out */
} EarPortConfig;

/**
 * Load configuration from file
 * Creates default config if file doesn't exist
 *
 * @param config Pointer to config structure to fill
 * @return true on success
 */
bool config_load(EarPortConfig *config);

/**
 * Save configuration to file
 *
 * @param config Pointer to config structure to save
 * @return true on success
 */
bool config_save(const EarPortConfig *config);

/**
 * Get default configuration values
 *
 * @param config Pointer to config structure to fill with defaults
 */
void config_get_defaults(EarPortConfig *config);

/* Complete device profile (per-device settings) */
#define DEVICE_PROFILE_DISPLAY_NAME_SIZE 64

typedef struct {
    char display_name[DEVICE_PROFILE_DISPLAY_NAME_SIZE]; /* Empty = use model */
    ListeningModesConfig listening_modes;
    bool conversational_awareness;      /* CA enabled */
    int adaptive_noise_level;           /* 0-100 */
    bool has_saved_settings;            /* Whether profile has saved settings */
} DeviceProfile;

/**
 * Load complete device profile
 *
 * @param device_address Bluetooth MAC address of the device
 * @param profile Pointer to structure to fill with profile data
 * @return true if found and loaded, false if not found (defaults used)
 */
bool config_load_device_profile(const char *device_address, DeviceProfile *profile);

/**
 * Save complete device profile
 *
 * @param device_address Bluetooth MAC address of the device
 * @param profile Pointer to profile to save
 * @return true on success
 */
bool config_save_device_profile(const char *device_address, const DeviceProfile *profile);

/**
 * Get default device profile
 *
 * @param profile Pointer to structure to fill with defaults
 */
void config_get_default_profile(DeviceProfile *profile);

/* Copy a user-visible name into the fixed profile field without splitting a
 * UTF-8 code point at the byte boundary. Invalid input is made valid. */
void config_copy_display_name(
    char destination[DEVICE_PROFILE_DISPLAY_NAME_SIZE],
    const char *source);

#endif /* CONFIG_H */
