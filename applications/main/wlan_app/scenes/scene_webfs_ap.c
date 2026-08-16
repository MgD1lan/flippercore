/* Dedicated-AP settings: SSID / Password (TextInput) + Start Filesystem.
 * Values live in app->webfs_ssid/pw (loaded from /ext/webfs/config.txt on first
 * enter); Start hands over to the info scene in AP mode. */

#include "../wlan_app.h"

enum {
    WebFsApItemSsid,
    WebFsApItemPassword,
    WebFsApItemStart,
};

static void webfs_ap_submenu_cb(void* context, uint32_t index) {
    WlanApp* app = context;
    uint32_t ev;
    switch(index) {
    case WebFsApItemSsid:
        ev = WlanAppCustomEventWebFsApSsid;
        break;
    case WebFsApItemPassword:
        ev = WlanAppCustomEventWebFsApPassword;
        break;
    default:
        ev = WlanAppCustomEventWebFsApStart;
        break;
    }
    view_dispatcher_send_custom_event(app->view_dispatcher, ev);
}

void wlan_app_scene_webfs_ap_on_enter(void* context) {
    WlanApp* app = context;
    if(app->webfs_ssid[0] == '\0') {
        wlan_webfs_config_load(app->webfs_ssid, app->webfs_pw);
    }

    submenu_reset(app->submenu);
    submenu_set_header_centered(app->submenu, "Dedicated AP");

    char line[80];
    snprintf(line, sizeof(line), "SSID: %s", app->webfs_ssid);
    submenu_add_item(app->submenu, line, WebFsApItemSsid, webfs_ap_submenu_cb, app);
    snprintf(line, sizeof(line), "Password: %s", app->webfs_pw[0] ? app->webfs_pw : "(open)");
    submenu_add_item(app->submenu, line, WebFsApItemPassword, webfs_ap_submenu_cb, app);
    submenu_add_item(app->submenu, "Start Filesystem", WebFsApItemStart, webfs_ap_submenu_cb, app);

    submenu_set_selected_item(
        app->submenu, scene_manager_get_scene_state(app->scene_manager, WlanAppSceneWebFsAp));
    view_dispatcher_switch_to_view(app->view_dispatcher, WlanAppViewSubmenu);
}

bool wlan_app_scene_webfs_ap_on_event(void* context, SceneManagerEvent event) {
    WlanApp* app = context;
    SceneManager* sm = app->scene_manager;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        if(event.event == WlanAppCustomEventWebFsApSsid) {
            scene_manager_set_scene_state(sm, WlanAppSceneWebFsAp, WebFsApItemSsid);
            scene_manager_set_scene_state(sm, WlanAppSceneWebFsInput, 0 /* SSID */);
            scene_manager_next_scene(sm, WlanAppSceneWebFsInput);
            consumed = true;
        } else if(event.event == WlanAppCustomEventWebFsApPassword) {
            scene_manager_set_scene_state(sm, WlanAppSceneWebFsAp, WebFsApItemPassword);
            scene_manager_set_scene_state(sm, WlanAppSceneWebFsInput, 1 /* password */);
            scene_manager_next_scene(sm, WlanAppSceneWebFsInput);
            consumed = true;
        } else if(event.event == WlanAppCustomEventWebFsApStart) {
            scene_manager_set_scene_state(sm, WlanAppSceneWebFsAp, WebFsApItemStart);
            scene_manager_set_scene_state(sm, WlanAppSceneWebFsInfo, 1 /* AP */);
            scene_manager_next_scene(sm, WlanAppSceneWebFsInfo);
            consumed = true;
        }
    }
    return consumed;
}

void wlan_app_scene_webfs_ap_on_exit(void* context) {
    WlanApp* app = context;
    submenu_reset(app->submenu);
}
