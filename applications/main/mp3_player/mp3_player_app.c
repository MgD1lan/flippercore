#include "mp3_player.h"
#include "mp3_storage.h"
#include "mp3_sink.h"
#include "mp3_decoder.h"

#include <stdio.h>
#include <string.h>

#include <furi.h>
#include <furi_hal.h>
#include <gui/gui.h>
#include <gui/elements.h>
#include <storage/storage.h>

#define TAG MP3_PLAYER_TAG

/* T-Embed mono canvas resolution (the GUI service scales 2x to RGB565). */
#define BROWSER_VISIBLE_ROWS 4
#define BROWSER_ROW_H        13
#define BROWSER_ROW_X_PAD    4

/* ---------- decoder callback ---------- */

static void on_track_ended(void* ctx) {
    Mp3App* app = ctx;
    Mp3Event ev = {.type = Mp3EventTypeTrackEnded};
    furi_message_queue_put(app->event_queue, &ev, 0);
}

/* ---------- input ---------- */

static void mp3_input_callback(InputEvent* input_event, void* ctx) {
    Mp3App* app = ctx;
    Mp3Event ev = {.type = Mp3EventTypeKey, .input = *input_event};
    furi_message_queue_put(app->event_queue, &ev, FuriWaitForever);
}

static void mp3_tick_callback(void* ctx) {
    Mp3App* app = ctx;
    Mp3Event ev = {.type = Mp3EventTypeTick};
    furi_message_queue_put(app->event_queue, &ev, 0);
}

/* ---------- helpers ---------- */

static void format_mmss(uint32_t ms, char* out, size_t out_size) {
    uint32_t total = ms / 1000;
    snprintf(out, out_size, "%02lu:%02lu", (unsigned long)(total / 60), (unsigned long)(total % 60));
}

/* Strip the .mp3 extension for display, copy into out. */
static void track_display_name(const Mp3Track* t, char* out, size_t out_size) {
    strncpy(out, t->name, out_size - 1);
    out[out_size - 1] = '\0';
    size_t len = strlen(out);
    if(len >= 4) {
        const char* tail = out + len - 4;
        if((tail[0] == '.') &&
           (tail[1] == 'm' || tail[1] == 'M') &&
           (tail[2] == 'p' || tail[2] == 'P') &&
           (tail[3] == '3')) {
            out[len - 4] = '\0';
        }
    }
}

/* ---------- render: browser ---------- */

static void render_browser(Canvas* canvas, Mp3App* app) {
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 2, 10, "MP3 Player");
    canvas_draw_line(canvas, 0, 12, 127, 12);

    canvas_set_font(canvas, FontSecondary);
    if(app->playlist.count == 0) {
        canvas_draw_str_aligned(canvas, 64, 32, AlignCenter, AlignCenter, "No tracks");
        canvas_draw_str_aligned(canvas, 64, 44, AlignCenter, AlignCenter, MP3_PLAYER_DATA_DIR);
        return;
    }

    /* Scroll the visible window so the cursor stays inside it. */
    int32_t top = app->selected - BROWSER_VISIBLE_ROWS / 2;
    if(top < 0) top = 0;
    int32_t max_top = (int32_t)app->playlist.count - BROWSER_VISIBLE_ROWS;
    if(max_top < 0) max_top = 0;
    if(top > max_top) top = max_top;

    int32_t end = top + BROWSER_VISIBLE_ROWS;
    if((size_t)end > app->playlist.count) end = (int32_t)app->playlist.count;

    char buf[MP3_PLAYER_NAME_MAX];
    for(int32_t i = top; i < end; i++) {
        int32_t row = i - top;
        int32_t y = 14 + row * BROWSER_ROW_H;
        bool selected = (i == app->selected);
        if(selected) {
            canvas_draw_box(canvas, 0, y, 128, BROWSER_ROW_H);
            canvas_invert_color(canvas);
        }
        track_display_name(&app->playlist.tracks[i], buf, sizeof(buf));
        canvas_draw_str(canvas, BROWSER_ROW_X_PAD, y + 10, buf);
        if(selected) canvas_invert_color(canvas);
    }
}

/* ---------- render: now-playing ---------- */

static void render_now_playing(Canvas* canvas, Mp3App* app) {
    canvas_set_font(canvas, FontPrimary);

    char title[MP3_PLAYER_NAME_MAX];
    if(app->playing_index >= 0 && (size_t)app->playing_index < app->playlist.count) {
        track_display_name(&app->playlist.tracks[app->playing_index], title, sizeof(title));
    } else {
        snprintf(title, sizeof(title), "—");
    }
    canvas_draw_str(canvas, 2, 10, title);
    canvas_draw_line(canvas, 0, 12, 127, 12);

    /* Time */
    canvas_set_font(canvas, FontSecondary);
    char elapsed[8], total[8];
    format_mmss(app->elapsed_ms, elapsed, sizeof(elapsed));
    if(app->duration_ms > 0) {
        format_mmss(app->duration_ms, total, sizeof(total));
    } else {
        strncpy(total, "--:--", sizeof(total));
    }
    canvas_draw_str(canvas, 2, 26, elapsed);
    canvas_draw_str_aligned(canvas, 126, 26, AlignRight, AlignBottom, total);

    /* Progress bar */
    int32_t bar_y = 30;
    int32_t bar_w = 124;
    canvas_draw_frame(canvas, 2, bar_y, bar_w, 6);
    if(app->duration_ms > 0) {
        uint32_t fill = (app->elapsed_ms * (uint32_t)(bar_w - 2)) / app->duration_ms;
        if(fill > (uint32_t)(bar_w - 2)) fill = bar_w - 2;
        canvas_draw_box(canvas, 3, bar_y + 1, fill, 4);
    }

    /* Status & volume */
    const char* status =
        (app->playback == Mp3StatePlaying) ? "Playing" :
        (app->playback == Mp3StatePaused)  ? "Paused"  : "Stopped";
    canvas_draw_str(canvas, 2, 50, status);
    char vol[16];
    snprintf(vol, sizeof(vol), "Vol %u%%", (unsigned)app->volume);
    canvas_draw_str_aligned(canvas, 126, 50, AlignRight, AlignBottom, vol);

    elements_button_left(canvas, "Config");
    elements_button_center(canvas, app->playback == Mp3StatePlaying ? "Pause" : "Play");
}

/* ---------- render: config (settings) ---------- */

static void render_config(Canvas* canvas, Mp3App* app) {
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 2, 10, "Settings");
    canvas_draw_line(canvas, 0, 12, 127, 12);

    canvas_set_font(canvas, FontSecondary);

    bool airplay_on = airplay_ui_is_connected(app->airplay);
    char volbuf[8];
    snprintf(volbuf, sizeof(volbuf), "%u%%", (unsigned)app->volume);

    const char* labels[6] = {"Audio Output", "Repeat", "Shuffle", "Volume", "Play MP3", "Exit"};
    const char* values[6] = {
        airplay_on ? "AirPlay" : "Speaker",
        app->repeat ? "On" : "Off",
        app->shuffle ? "On" : "Off",
        volbuf,
        NULL, /* Play MP3 — action row */
        NULL, /* Exit — action row */
    };

    /* Scroll window so the cursor stays visible (5 rows fit under the title). */
    const int total = 6;
    const int visible = 5;
    int top = app->config_sel - visible / 2;
    if(top < 0) top = 0;
    if(top > total - visible) top = total - visible;
    if(top < 0) top = 0;

    for(int idx = top; idx < top + visible && idx < total; idx++) {
        int y = 13 + (idx - top) * 10;
        bool sel = (app->config_sel == idx);
        if(sel) {
            canvas_draw_box(canvas, 0, y, 128, 10);
            canvas_invert_color(canvas);
        }
        canvas_draw_str(canvas, 4, y + 8, labels[idx]);
        if(values[idx]) {
            char vb[16];
            /* Volume (index 3) in edit mode: wrap in < > to show rotation adjusts it. */
            if(idx == 3 && sel && app->config_editing) {
                snprintf(vb, sizeof(vb), "<%s>", values[idx]);
            } else {
                snprintf(vb, sizeof(vb), "%s", values[idx]);
            }
            canvas_draw_str_aligned(canvas, 124, y + 8, AlignRight, AlignBottom, vb);
        }
        if(sel) canvas_invert_color(canvas);
    }
    /* simple scroll indicator: a down-arrow hint when more rows are below */
    if(top + visible < total) {
        canvas_draw_str_aligned(canvas, 123, 63, AlignRight, AlignBottom, "v");
    }
    /* No bottom action buttons — the settings list mirrors the SubGhz
     * VariableItemList look (rotate = navigate, OK = select, Back = leave). */
}

static void mp3_render_callback(Canvas* canvas, void* ctx) {
    Mp3App* app = ctx;
    if(furi_mutex_acquire(app->mutex, 100) != FuriStatusOk) return;

    canvas_clear(canvas);
    if(airplay_ui_is_active(app->airplay)) {
        airplay_ui_render(canvas, app->airplay);
    } else if(app->view == Mp3ViewBrowser) {
        render_browser(canvas, app);
    } else if(app->view == Mp3ViewConfig) {
        render_config(canvas, app);
    } else {
        render_now_playing(canvas, app);
    }

    furi_mutex_release(app->mutex);
}

/* ---------- actions ---------- */

static bool start_track(Mp3App* app, int32_t index) {
    if(index < 0 || (size_t)index >= app->playlist.count) return false;
    char path[256];
    if(!mp3_storage_track_path(&app->playlist, index, path, sizeof(path))) return false;

    if(!mp3_decoder_play(path)) {
        FURI_LOG_E(TAG, "decoder_play failed for %s", path);
        return false;
    }
    app->playing_index = index;
    app->playback      = Mp3StatePlaying;
    app->elapsed_ms    = 0;
    app->duration_ms   = 0;
    app->view          = Mp3ViewNowPlaying;
    return true;
}

static void stop_track(Mp3App* app) {
    mp3_decoder_stop();
    app->playback = Mp3StateIdle;
}

static void toggle_pause(Mp3App* app) {
    if(app->playback == Mp3StatePlaying) {
        mp3_decoder_pause();
        app->playback = Mp3StatePaused;
    } else if(app->playback == Mp3StatePaused) {
        mp3_decoder_resume();
        app->playback = Mp3StatePlaying;
    }
}

static void volume_step(Mp3App* app, int delta) {
    int v = (int)app->volume + delta;
    if(v < 0) v = 0;
    if(v > 100) v = 100;
    app->volume = (uint8_t)v;
    mp3_sink_set_volume(app->volume);
}

/* Index of the next track to play. With shuffle on, a random track (never the
 * current one); otherwise the following track, or -1 at the end of the list. */
static int32_t mp3_next_index(Mp3App* app) {
    int32_t count = (int32_t)app->playlist.count;
    if(count <= 0) return -1;
    if(app->shuffle && count > 1) {
        int32_t n;
        do {
            n = (int32_t)(furi_hal_random_get() % (uint32_t)count);
        } while(n == app->playing_index);
        return n;
    }
    int32_t n = app->playing_index + 1;
    return (n < count) ? n : -1;
}

/* ---------- input handling ---------- */

static bool handle_browser_input(Mp3App* app, const InputEvent* in) {
    if(in->type != InputTypeShort && in->type != InputTypeRepeat) return true;

    switch(in->key) {
    case InputKeyUp:
        if(app->playlist.count > 0) {
            app->selected = (app->selected - 1 + (int32_t)app->playlist.count) %
                            (int32_t)app->playlist.count;
        }
        break;
    case InputKeyDown:
        if(app->playlist.count > 0) {
            app->selected = (app->selected + 1) % (int32_t)app->playlist.count;
        }
        break;
    case InputKeyOk:
        if(app->playlist.count > 0) start_track(app, app->selected);
        break;
    case InputKeyBack:
        /* Back from the list returns to the Config (the app's root screen). */
        app->config_sel = 0;
        app->view = Mp3ViewConfig;
        break;
    default:
        break;
    }
    return true;
}

static bool handle_now_playing_input(Mp3App* app, const InputEvent* in) {
    if(in->type != InputTypeShort && in->type != InputTypeRepeat) {
        if(in->key == InputKeyBack && in->type == InputTypeLong) {
            /* Long-back from Now-Playing: back to the playlist (keep playing). */
            app->view = Mp3ViewBrowser;
            return true;
        }
        return true;
    }

    switch(in->key) {
    case InputKeyUp:
        /* rotate left (CCW) → open the Config (settings) menu */
        app->config_sel = 0;
        app->config_editing = false;
        app->view = Mp3ViewConfig;
        break;
    case InputKeyDown: {
        /* rotate right (CW) → next track (shuffle-aware) */
        int32_t n = mp3_next_index(app);
        if(n >= 0) start_track(app, n);
        break;
    }
    case InputKeyLeft:
        /* encoder held + rotate left → previous track */
        if(app->playing_index > 0) start_track(app, app->playing_index - 1);
        break;
    case InputKeyRight: {
        /* encoder held + rotate right → next track */
        int32_t n = mp3_next_index(app);
        if(n >= 0) start_track(app, n);
        break;
    }
    case InputKeyOk:
        if(app->playback == Mp3StateIdle && app->playing_index >= 0) {
            /* Track finished → OK replays it from the start. */
            start_track(app, app->playing_index);
        } else {
            toggle_pause(app);
        }
        break;
    case InputKeyBack:
        /* Back → return to the playlist, keep playback going. */
        app->view = Mp3ViewBrowser;
        break;
    default:
        break;
    }
    return true;
}

#define MP3_CONFIG_ITEMS 5 /* Audio Output, Shuffle, Volume, Repeat, Play MP3 */

static bool handle_config_input(Mp3App* app, const InputEvent* in) {
    if(in->type != InputTypeShort && in->type != InputTypeRepeat) return true;

    /* Volume edit mode: rotation adjusts the value; OK/Back/Left exits. */
    if(app->config_editing) {
        switch(in->key) {
        case InputKeyUp:
            volume_step(app, +5);
            break;
        case InputKeyDown:
            volume_step(app, -5);
            break;
        case InputKeyOk:
        case InputKeyBack:
        case InputKeyLeft:
            app->config_editing = false;
            break;
        default:
            break;
        }
        return true;
    }

    switch(in->key) {
    case InputKeyUp:
        app->config_sel = (app->config_sel - 1 + MP3_CONFIG_ITEMS) % MP3_CONFIG_ITEMS;
        break;
    case InputKeyDown:
        app->config_sel = (app->config_sel + 1) % MP3_CONFIG_ITEMS;
        break;
    case InputKeyOk:
        if(app->config_sel == 0) {
            /* Audio Output → Speaker/AirPlay select screen (airplay UI takes over) */
            airplay_ui_enter(app->airplay);
        } else if(app->config_sel == 1) {
            app->repeat = !app->repeat;
        } else if(app->config_sel == 2) {
            app->shuffle = !app->shuffle;
        } else if(app->config_sel == 3) {
            app->config_editing = true; /* enter volume edit */
        } else if(app->config_sel == 4) {
            app->view = Mp3ViewBrowser; /* Play MP3 → track list */
        } else {
            return false; /* Exit → leave the app */
        }
        break;
    case InputKeyBack:
        /* Config is the root screen: go to Now-Playing if a track is loaded,
         * otherwise leave the app. */
        if(app->playback != Mp3StateIdle && app->playing_index >= 0) {
            app->view = Mp3ViewNowPlaying;
        } else {
            return false; /* exit app */
        }
        break;
    default:
        break;
    }
    return true;
}

/* ---------- entry point ---------- */

int32_t mp3_player_app(void* p) {
    UNUSED(p);
    FURI_LOG_I(TAG, "starting");

    Mp3App* app = malloc(sizeof(Mp3App));
    app->mutex       = furi_mutex_alloc(FuriMutexTypeNormal);
    app->event_queue = furi_message_queue_alloc(16, sizeof(Mp3Event));
    app->view        = Mp3ViewConfig; /* app opens on the config/settings screen */
    app->selected    = 0;
    app->playback    = Mp3StateIdle;
    app->playing_index = -1;
    app->volume      = 80;
    app->elapsed_ms  = 0;
    app->duration_ms = 0;
    app->shuffle     = false;
    app->repeat      = false;
    app->config_sel  = 0;
    app->config_editing = false;
    app->airplay     = airplay_ui_alloc();

    if(!mp3_storage_playlist_alloc(&app->playlist)) {
        FURI_LOG_E(TAG, "playlist alloc failed");
        goto cleanup_pre;
    }

    /* Tear down the standard speaker HAL — it owns I2S_NUM_0 by default and
     * registers a channel even when nobody is playing tones. We need exclusive
     * ownership of I2S_NUM_0 for our own writer thread. Restored on exit. */
    furi_hal_speaker_deinit();
    if(!mp3_sink_init_speaker(44100)) {
        FURI_LOG_E(TAG, "i2s init failed");
        furi_hal_speaker_init();   /* restore on failure */
        goto cleanup_playlist;
    }
    mp3_sink_set_volume(app->volume);

    if(!mp3_decoder_init()) {
        FURI_LOG_E(TAG, "decoder init failed");
        goto cleanup_i2s;
    }
    mp3_decoder_set_ended_callback(on_track_ended, app);

    /* Scan for tracks. */
    Storage* storage = furi_record_open(RECORD_STORAGE);
    mp3_storage_scan(storage, &app->playlist);
    furi_record_close(RECORD_STORAGE);

    /* GUI setup. */
    ViewPort* view_port = view_port_alloc();
    view_port_draw_callback_set(view_port, mp3_render_callback, app);
    view_port_input_callback_set(view_port, mp3_input_callback, app);

    Gui* gui = furi_record_open(RECORD_GUI);
    gui_add_view_port(gui, view_port, GuiLayerFullscreen);

    FuriTimer* timer = furi_timer_alloc(mp3_tick_callback, FuriTimerTypePeriodic, app);
    furi_timer_start(timer, furi_kernel_get_tick_frequency() / 4); /* 4 Hz UI tick */

    /* Main event loop. */
    Mp3Event ev;
    bool running = true;
    while(running) {
        if(furi_message_queue_get(app->event_queue, &ev, 200) != FuriStatusOk) continue;

        furi_mutex_acquire(app->mutex, FuriWaitForever);

        switch(ev.type) {
        case Mp3EventTypeKey:
            if(airplay_ui_is_active(app->airplay)) {
                airplay_ui_input(app->airplay, &ev.input);
            } else if(app->view == Mp3ViewBrowser) {
                running = handle_browser_input(app, &ev.input);
            } else if(app->view == Mp3ViewConfig) {
                running = handle_config_input(app, &ev.input);
            } else {
                running = handle_now_playing_input(app, &ev.input);
            }
            break;
        case Mp3EventTypeTick: {
            airplay_ui_tick(app->airplay);
            /* Don't refresh progress once a track has finished (Idle) — that
             * would pull elapsed back to the track duration and undo the 0:00
             * reset done in TrackEnded. */
            if(mp3_decoder_is_playing() || mp3_decoder_is_paused()) {
                mp3_decoder_get_progress(&app->elapsed_ms, &app->duration_ms);
            }
            if(mp3_decoder_is_paused())       app->playback = Mp3StatePaused;
            else if(mp3_decoder_is_playing()) app->playback = Mp3StatePlaying;

            /* Push title + position to the AirPlay receiver ~once per second. */
            static uint8_t prog_div = 0;
            if(mp3_sink_is_airplay() && app->playback != Mp3StateIdle &&
               app->playing_index >= 0 && ++prog_div >= 4) {
                prog_div = 0;
                char title[MP3_PLAYER_NAME_MAX];
                track_display_name(
                    &app->playlist.tracks[app->playing_index], title, sizeof(title));
                mp3_sink_set_metadata(title, app->duration_ms);
                mp3_sink_set_progress(app->elapsed_ms, app->duration_ms);
            }
            break;
        }
        case Mp3EventTypeTrackEnded:
            FURI_LOG_I(TAG, "track ended");
            if(app->repeat && app->playing_index >= 0) {
                /* Repeat: loop the current track forever. */
                start_track(app, app->playing_index);
            } else {
                int32_t n = mp3_next_index(app);
                if(n >= 0) {
                    start_track(app, n);
                } else {
                    /* End of the list, no repeat → reset to 0:00 and show the
                     * Play button so the user can replay manually. */
                    app->playback = Mp3StateIdle;
                    app->elapsed_ms = 0;
                }
            }
            break;
        }

        furi_mutex_release(app->mutex);
        view_port_update(view_port);
    }

    /* Teardown. */
    furi_timer_stop(timer);
    furi_timer_free(timer);
    view_port_enabled_set(view_port, false);
    gui_remove_view_port(gui, view_port);
    view_port_free(view_port);
    furi_record_close(RECORD_GUI);

    mp3_decoder_stop();
    mp3_decoder_deinit();
    mp3_sink_deinit();
    furi_hal_speaker_init();   /* restore standard speaker HAL */
    airplay_ui_free(app->airplay);   /* stops WiFi + restores BT if started */
    mp3_storage_playlist_free(&app->playlist);
    furi_message_queue_free(app->event_queue);
    furi_mutex_free(app->mutex);
    free(app);
    FURI_LOG_I(TAG, "exit");
    return 0;

cleanup_i2s:
    mp3_sink_deinit();
    furi_hal_speaker_init();
cleanup_playlist:
    mp3_storage_playlist_free(&app->playlist);
cleanup_pre:
    furi_message_queue_free(app->event_queue);
    furi_mutex_free(app->mutex);
    free(app);
    return 255;
}
