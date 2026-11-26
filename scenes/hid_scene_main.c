#include "../hid.h"
#include "../views.h"

void hid_scene_main_on_enter(void* context) {
    Hid* app = context;
    hid_mode_show(app, app->mode_current);
}

bool hid_scene_main_on_event(void* context, SceneManagerEvent event) {
    Hid* app = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        if(event.event == HidCustomEventModeToggle) {
            hid_mode_toggle(app);
            consumed = true;
        } else if(event.event == HidCustomEventModeNext) {
            hid_mode_next(app);
            consumed = true;
        } else if(event.event == HidCustomEventBackSingle) {
            hid_mode_back_single(app);
            consumed = true;
        } else if(event.event == HidCustomEventModeMenu) {
            scene_manager_previous_scene(app->scene_manager);
            consumed = true;
        }
    }

    return consumed;
}

void hid_scene_main_on_exit(void* context) {
    Hid* app = context;
    furi_timer_stop(app->back_tap_timer);
    app->back_tap_pending = false;
    app->back_long = false;
}
