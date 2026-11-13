#pragma once

/* Project identity shown in About. This is the upstream project this port is
 * based on, not the device: report the project and its author first, then the
 * device and the build version, like the stock firmware does. */
#define FURI_ESP32_PROJECT_NAME   "Flipper-Zero-ESP32-Port"
#define FURI_ESP32_PROJECT_AUTHOR "by Sor3nt"

/* Device name shown in About and used by the board layer. Keep this in sync
 * with BOARD_NAME in components/furi_hal/boards/board_fatalcore.h. */
#define FURI_ESP32_DEVICE_NAME  "Fatalcore"

#define FURI_ESP32_VERSION      "2.0.12"
#define FURI_ESP32_VERSION_NAME "Fatalcore"
#define FURI_ESP32_VERSION_FULL FURI_ESP32_VERSION " - " FURI_ESP32_VERSION_NAME