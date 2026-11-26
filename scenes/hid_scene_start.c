#include "../hid.h"
#include "../views.h"

// Menu indexes below HidModeCount are modes
enum HidSubmenuIndex {
    HidSubmenuIndexOrientation = HidModeCount,
    HidSubmenuIndexMouseSettings,
    HidSubmenuIndexRemovePairing,
};

static void hid_scene_start_submenu_callback(void* context, uint32_t index) {
    furi_assert(context);
    Hid* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, index);
}

void hid_scene_start_on_enter(void* context) {
    Hid* app = context;

    submenu_set_header(app->submenu, "2x Back: next mode");
    for(size_t i = 0; i < hid_mode_count(); i++) {
        const HidMode mode = hid_mode_at(i);
        submenu_add_item(
            app->submenu, hid_mode_name(mode), mode, hid_scene_start_submenu_callback, app);
    }
    submenu_add_item(
        app->submenu,
        app->vertical ? "Layout: Upright" : "Layout: Horizontal",
        HidSubmenuIndexOrientation,
        hid_scene_start_submenu_callback,
        app);
    submenu_add_item(
        app->submenu,
        "Mouse settings",
        HidSubmenuIndexMouseSettings,
        hid_scene_start_submenu_callback,
        app);
#ifdef HID_TRANSPORT_BLE
    submenu_add_item(
        app->submenu,
        "Bluetooth Unpairing",
        HidSubmenuIndexRemovePairing,
        hid_scene_start_submenu_callback,
        app);
#endif

    submenu_set_selected_item(app->submenu, app->mode_current);
    view_dispatcher_switch_to_view(app->view_dispatcher, HidViewSubmenu);
}

bool hid_scene_start_on_event(void* context, SceneManagerEvent event) {
    Hid* app = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        if(event.event == HidSubmenuIndexRemovePairing) {
            scene_manager_next_scene(app->scene_manager, HidSceneUnpair);
            consumed = true;
        } else if(event.event == HidSubmenuIndexMouseSettings) {
            scene_manager_next_scene(app->scene_manager, HidSceneMouseSettings);
            consumed = true;
        } else if(event.event == HidSubmenuIndexOrientation) {
            hid_mode_set_vertical(app, !app->vertical);
            submenu_change_item_label(
                app->submenu,
                HidSubmenuIndexOrientation,
                app->vertical ? "Layout: Upright" : "Layout: Horizontal");
            consumed = true;
        } else if(event.event < HidModeCount) {
            // Picking another mode keeps the one just left as the swap partner
            if(event.event != app->mode_current) {
                app->mode_previous = app->mode_current;
                app->mode_current = event.event;
            }
            scene_manager_next_scene(app->scene_manager, HidSceneMain);
            consumed = true;
        }
    }

    return consumed;
}

void hid_scene_start_on_exit(void* context) {
    Hid* app = context;
    submenu_reset(app->submenu);
}
