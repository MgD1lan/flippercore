/* Web-Filesystem info scene (Widget): starts the file server (AP or STA per
 * scene state: 1 = AP, 0 = STA) and shows SSID + URL + client count. Back stops
 * the server and returns to the entry menu. */

#include "../wlan_app.h"

static int s_last_clients = -1;

static void webfs_info_render(WlanApp* app, bool ok) {
    Widget* w = app->widget;
    widget_reset(w);
    widget_add_string_element(w, 64, 2, AlignCenter, AlignTop, FontPrimary, "Web-Filesystem");

    if(!ok) {
        widget_add_string_element(
            w, 64, 30, AlignCenter, AlignCenter, FontSecondary, "Start failed (RAM/WiFi?)");
        widget_add_string_element(w, 64, 54, AlignCenter, AlignTop, FontSecondary, "Back = exit");
        return;
    }

    bool ap = wlan_webfs_is_ap();
    char line[64];
    char ip[16] = {0};
    wlan_webfs_get_ip(ip, sizeof(ip));

    const char* ssid = ap ? app->webfs_ssid : app->connected_ap.ssid;
    snprintf(line, sizeof(line), "SSID: %s", ssid);
    widget_add_string_element(w, 4, 18, AlignLeft, AlignTop, FontSecondary, line);

    snprintf(line, sizeof(line), "http://%s", ip);
    widget_add_string_element(w, 4, 30, AlignLeft, AlignTop, FontPrimary, line);

    if(ap) {
        snprintf(line, sizeof(line), "Clients: %u", (unsigned)wlan_webfs_get_client_count());
        widget_add_string_element(w, 4, 46, AlignLeft, AlignTop, FontSecondary, line);
    }

    widget_add_string_element(w, 64, 56, AlignCenter, AlignTop, FontSecondary, "Back = stop");
}

void wlan_app_scene_webfs_info_on_enter(void* context) {
    WlanApp* app = context;
    uint32_t mode = scene_manager_get_scene_state(app->scene_manager, WlanAppSceneWebFsInfo);

    bool ok = (mode == 1) ? wlan_webfs_start_ap(app->webfs_ssid, app->webfs_pw) :
                            wlan_webfs_start_sta();

    s_last_clients = -1;
    webfs_info_render(app, ok);
    view_dispatcher_switch_to_view(app->view_dispatcher, WlanAppViewWidget);
}

bool wlan_app_scene_webfs_info_on_event(void* context, SceneManagerEvent event) {
    WlanApp* app = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeTick) {
        // Refresh the client count in AP mode when it changes.
        if(wlan_webfs_is_running() && wlan_webfs_is_ap()) {
            int now = wlan_webfs_get_client_count();
            if(now != s_last_clients) {
                s_last_clients = now;
                webfs_info_render(app, true);
            }
        }
        consumed = true;
    } else if(event.type == SceneManagerEventTypeBack) {
        // on_exit stops the server; return to the entry scene.
        if(!scene_manager_search_and_switch_to_previous_scene(
               app->scene_manager, WlanAppSceneWebFsMenu)) {
            if(!scene_manager_search_and_switch_to_previous_scene(
                   app->scene_manager, WlanAppSceneMain)) {
                scene_manager_previous_scene(app->scene_manager);
            }
        }
        consumed = true;
    }
    return consumed;
}

void wlan_app_scene_webfs_info_on_exit(void* context) {
    UNUSED(context);
    wlan_webfs_stop();
}
