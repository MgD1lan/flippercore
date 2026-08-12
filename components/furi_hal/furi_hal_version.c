#include "furi_hal_version.h"

#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include <esp_mac.h>
#include <nvs_flash.h>
#include <nvs.h>

/* -------------------------------------------------------------------------
 * eFuse-derived device identity
 *
 * Rather than a static "ESP32" name, derive a stable, human-readable name
 * from the factory-burned eFuse MAC — the same approach the original
 * Flipper Zero firmware uses with its OTP memory.
 *
 * The name is selected from the table below using a mixing hash of all 6
 * MAC bytes.  Every device gets a consistent, unique-ish name at first boot
 * without any user interaction.  The user can still override it through the
 * Settings → Desktop → Name screen (stored in /dolphin/name.settings).
 * -------------------------------------------------------------------------*/

static const char* const s_device_name_pool[] = {
    "Axiom",  "Blip",   "Byte",   "Chirp",  "Cipher",
    "Core",   "Data",   "Drift",  "Echo",   "Edge",
    "Flux",   "Frame",  "Glint",  "Hover",  "Ion",
    "Jazz",   "Krypto", "Lumen",  "Motif",  "Nexus",
    "Orbit",  "Pixel",  "Pulse",  "Quark",  "Relic",
    "Ripple", "Spark",  "Trace",  "Vortex", "Warp",
    "Xenon",  "Zap",
};
#define DEVICE_NAME_POOL_SIZE \
    ((uint8_t)(sizeof(s_device_name_pool) / sizeof(s_device_name_pool[0])))

/* Mix 6 MAC bytes into a single byte index using FNV-like accumulation. */
static uint8_t derive_name_index(const uint8_t* mac) {
    uint8_t h = 0x5A; /* non-zero seed */
    for(int i = 0; i < 6; i++) {
        h = (uint8_t)(h * 31u + mac[i]);
    }
    return h % DEVICE_NAME_POOL_SIZE;
}

typedef struct {
    char name[FURI_HAL_VERSION_ARRAY_NAME_LENGTH];
    char device_name[FURI_HAL_VERSION_DEVICE_NAME_LENGTH];
    char ble_local_name[FURI_HAL_BT_ADV_NAME_LENGTH + 1];
    uint8_t ble_mac[6];
    uint8_t uid[6];
} FuriHalVersionState;

/* NVS key holding the user-spoofed device name (survives reboots regardless
 * of SD-card presence; the SD copy in /ext/dolphin/name.settings is kept for
 * compatibility with the stock namechanger service). */
#define FURI_HAL_VERSION_NVS_NAMESPACE "furi_hal"
#define FURI_HAL_VERSION_NVS_KEY       "dev_name"
#define FURI_HAL_VERSION_NVS_COLOR_KEY "shell_color"

static FuriHalVersionState furi_hal_version = {
    .name = "ESP32",
    .device_name = "Furi ESP32",
    .ble_local_name = "ESP32",
    .ble_mac = {0},
    .uid = {0},
};

static FuriHalVersionColor furi_hal_version_shell_color = FuriHalVersionColorUnknown;

static void furi_hal_version_nvs_load(char* out, size_t out_size) {
    nvs_handle_t handle;
    if(nvs_open(FURI_HAL_VERSION_NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) return;
    size_t len = out_size;
    if(nvs_get_str(handle, FURI_HAL_VERSION_NVS_KEY, out, &len) != ESP_OK) {
        out[0] = '\0';
    }
    nvs_close(handle);
}

static void furi_hal_version_nvs_store(const char* name) {
    nvs_handle_t handle;
    if(nvs_open(FURI_HAL_VERSION_NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) return;
    nvs_set_str(handle, FURI_HAL_VERSION_NVS_KEY, name);
    nvs_commit(handle);
    nvs_close(handle);
}

static void furi_hal_version_nvs_color_load(void) {
    nvs_handle_t handle;
    if(nvs_open(FURI_HAL_VERSION_NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) return;
    uint8_t value = FuriHalVersionColorUnknown;
    nvs_get_u8(handle, FURI_HAL_VERSION_NVS_COLOR_KEY, &value);
    nvs_close(handle);
    if(value <= FuriHalVersionColorTransparent) {
        furi_hal_version_shell_color = (FuriHalVersionColor)value;
    }
}

static void furi_hal_version_nvs_color_store(void) {
    nvs_handle_t handle;
    if(nvs_open(FURI_HAL_VERSION_NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) return;
    nvs_set_u8(handle, FURI_HAL_VERSION_NVS_COLOR_KEY, (uint8_t)furi_hal_version_shell_color);
    nvs_commit(handle);
    nvs_close(handle);
}

/* Refresh the BLE advertisement service UUID so qFlipper mobile picks up the
 * new shell color while scanning (no reconnect needed). Declared via extern to
 * keep furi_hal free of a ble_serial dependency — ble_serial itself links
 * against furi_hal. */
extern void ble_serial_refresh_advertising(void);

static void furi_hal_version_refresh_ble_advertisement(void) {
    ble_serial_refresh_advertising();
}

static void furi_hal_version_refresh_names(const char* name) {
    snprintf(furi_hal_version.name, sizeof(furi_hal_version.name), "%s", name);
    snprintf(
        furi_hal_version.device_name,
        sizeof(furi_hal_version.device_name),
        "Furi %s",
        furi_hal_version.name);
    snprintf(
        furi_hal_version.ble_local_name,
        sizeof(furi_hal_version.ble_local_name),
        "%s",
        furi_hal_version.name);

    version_set_custom_name((Version*)version_get(), furi_hal_version.name);
}

void furi_hal_version_init(void) {
    /* Read factory MAC from eFuse — used as hardware UID and BLE address. */
    esp_efuse_mac_get_default(furi_hal_version.uid);
    memcpy(furi_hal_version.ble_mac, furi_hal_version.uid, sizeof(furi_hal_version.uid));

    /* Effective name precedence:
     *   1. NVS user-spoofed name (set via Spoofing Options),
     *   2. custom name loaded by namechanger (SD /ext/dolphin/name.settings),
     *   3. stable name derived from the eFuse MAC. */
    char nvs_name[FURI_HAL_VERSION_ARRAY_NAME_LENGTH] = {0};
    furi_hal_version_nvs_load(nvs_name, sizeof(nvs_name));
    furi_hal_version_nvs_color_load();

    const char* custom = version_get_custom_name(NULL);
    const char* effective;
    if(nvs_name[0]) {
        effective = nvs_name;
    } else if(custom && custom[0]) {
        effective = custom;
    } else {
        effective = s_device_name_pool[derive_name_index(furi_hal_version.uid)];
    }
    furi_hal_version_refresh_names(effective);
}

bool furi_hal_version_do_i_belong_here(void) {
    return true;
}

const char* furi_hal_version_get_model_name(void) {
    return "Flipper Zero";
}

const char* furi_hal_version_get_model_code(void) {
    /* Report the actual silicon so host tools (qFlipper, esptool) see the
     * real chip rather than a hardcoded "ESP32-C6". */
#if defined(CONFIG_IDF_TARGET_ESP32S3)
    return "ESP32-S3";
#elif defined(CONFIG_IDF_TARGET_ESP32C6)
    return "ESP32-C6";
#else
    return "ESP32";
#endif
}

const char* furi_hal_version_get_fcc_id(void) {
    return "N/A";
}

const char* furi_hal_version_get_ic_id(void) {
    return "N/A";
}

const char* furi_hal_version_get_mic_id(void) {
    return "N/A";
}

const char* furi_hal_version_get_srrc_id(void) {
    return "N/A";
}

const char* furi_hal_version_get_ncc_id(void) {
    return "N/A";
}

FuriHalVersionOtpVersion furi_hal_version_get_otp_version(void) {
    return FuriHalVersionOtpVersionEmpty;
}

uint8_t furi_hal_version_get_hw_version(void) {
    return 1;
}

uint8_t furi_hal_version_get_hw_target(void) {
    return version_get_target(NULL);
}

uint8_t furi_hal_version_get_hw_body(void) {
    return 0;
}

FuriHalVersionColor furi_hal_version_get_hw_color(void) {
    return furi_hal_version_shell_color;
}

void furi_hal_version_set_hw_color(FuriHalVersionColor color) {
    if(color > FuriHalVersionColorTransparent) {
        color = FuriHalVersionColorUnknown;
    }
    furi_hal_version_shell_color = color;
    furi_hal_version_nvs_color_store();
    furi_hal_version_refresh_ble_advertisement();
}

uint8_t furi_hal_version_get_hw_connect(void) {
    return 0;
}

FuriHalVersionRegion furi_hal_version_get_hw_region(void) {
    return FuriHalVersionRegionWorld;
}

const char* furi_hal_version_get_hw_region_name(void) {
    return "WW";
}

FuriHalVersionRegion furi_hal_version_get_hw_region_otp(void) {
    return FuriHalVersionRegionWorld;
}

const char* furi_hal_version_get_hw_region_name_otp(void) {
    return "WW";
}

FuriHalVersionDisplay furi_hal_version_get_hw_display(void) {
    return FuriHalVersionDisplayUnknown;
}

uint32_t furi_hal_version_get_hw_timestamp(void) {
    return 0;
}

const char* furi_hal_version_get_name_ptr(void) {
    return furi_hal_version.name;
}

const char* furi_hal_version_get_device_name_ptr(void) {
    return furi_hal_version.device_name;
}

const char* furi_hal_version_get_ble_local_device_name_ptr(void) {
    return furi_hal_version.ble_local_name;
}

void furi_hal_version_set_name(const char* name) {
    /* Accept NULL or empty string — fall back to the eFuse-derived name. */
    if(name && name[0]) {
        furi_hal_version_refresh_names(name);
        furi_hal_version_nvs_store(furi_hal_version.name);
    } else {
        const char* derived = s_device_name_pool[derive_name_index(furi_hal_version.uid)];
        furi_hal_version_refresh_names(derived);
        furi_hal_version_nvs_store(furi_hal_version.name);
    }
}

const uint8_t* furi_hal_version_get_ble_mac(void) {
    return furi_hal_version.ble_mac;
}

const struct Version* furi_hal_version_get_firmware_version(void) {
    return version_get();
}

size_t furi_hal_version_uid_size(void) {
    return sizeof(furi_hal_version.uid);
}

const uint8_t* furi_hal_version_uid(void) {
    return furi_hal_version.uid;
}

const uint8_t* furi_hal_version_uid_default(void) {
    return furi_hal_version.uid;
}
