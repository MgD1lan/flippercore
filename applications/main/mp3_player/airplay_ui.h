#ifndef AIRPLAY_UI_H
#define AIRPLAY_UI_H

/* Self-contained AirPlay setup UI for the MP3 player.
 *
 * Runs on the player's plain ViewPort (no ViewDispatcher): the player delegates
 * render/input to this module whenever airplay_ui_is_active() is true. It owns
 * its whole sub-flow and state:
 *
 *   Output select  ->  WiFi scan  ->  password keyboard  ->  connecting
 *                                                              -> device scan
 *                                                              -> connected
 *
 * Phase 0 (this file) only establishes the WiFi connection and discovers RAOP
 * receivers. Actually routing audio to the chosen device is Phase 1; the player
 * reads the result via airplay_ui_output_mode() / airplay_ui_target().
 */

#include <stdbool.h>
#include <stdint.h>
#include <gui/gui.h>
#include <input/input.h>

#include "airplay_mdns.h"

typedef enum {
    Mp3OutputSpeaker,
    Mp3OutputAirplay,
} Mp3OutputMode;

typedef struct AirplayUi AirplayUi;

AirplayUi* airplay_ui_alloc(void);
void airplay_ui_free(AirplayUi* ui);

/* Open the AirPlay setup flow (shows the output-select screen). */
void airplay_ui_enter(AirplayUi* ui);

/* Start connecting straight away (skips the output-select screen): scans WiFi
 * if not connected yet, otherwise jumps to the AirPlay device scan. Used by the
 * player's Settings menu ("AirPlay Connect"). */
void airplay_ui_start_connect(AirplayUi* ui);

/* Drop the chosen AirPlay target and switch output back to the speaker. Leaves
 * the WiFi connection up for a fast reconnect (torn down on app exit). */
void airplay_ui_disconnect_target(AirplayUi* ui);

/* True once a target has been chosen and AirPlay is the active output. */
bool airplay_ui_is_connected(AirplayUi* ui);

/* True while any AirPlay screen is on top and should own render/input. */
bool airplay_ui_is_active(AirplayUi* ui);

/* Render the current AirPlay screen. Only call when is_active(). */
void airplay_ui_render(Canvas* canvas, AirplayUi* ui);

/* Feed an input event. Returns true if the AirPlay UI stays active, false when
 * the user has left the flow (control returns to the player). */
bool airplay_ui_input(AirplayUi* ui, const InputEvent* event);

/* Periodic poll (drives async WiFi connect + kicks off scans). Call from the
 * player's UI tick. */
void airplay_ui_tick(AirplayUi* ui);

/* Result the player acts on. */
Mp3OutputMode airplay_ui_output_mode(AirplayUi* ui);
const AirplayDevice* airplay_ui_target(AirplayUi* ui); /* NULL if none chosen */

#endif /* AIRPLAY_UI_H */
