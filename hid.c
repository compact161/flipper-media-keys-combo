#include "hid.h"
#include <extra_profiles/hid_profile.h>
#include <profiles/serial_profile.h>
#include "views.h"
#include <notification/notification_messages.h>
#include <dolphin/dolphin.h>
#include <gui/elements.h>
#include "hid_icons.h"

#define TAG "HidApp"

bool hid_custom_event_callback(void* context, uint32_t event) {
    furi_assert(context);
    Hid* app = context;
    return scene_manager_handle_custom_event(app->scene_manager, event);
}

bool hid_back_event_callback(void* context) {
    furi_assert(context);
    Hid* app = context;
    return scene_manager_handle_back_event(app->scene_manager);
}

void bt_hid_remove_pairing(Hid* app) {
    Bt* bt = app->bt;
    bt_disconnect(bt);

    // Wait 2nd core to update nvm storage
    furi_delay_ms(200);

    furi_hal_bt_stop_advertising();

    bt_forget_bonded_devices(bt);

    furi_hal_bt_start_advertising();
}

static void bt_hid_connection_status_changed_callback(BtStatus status, void* context) {
    furi_assert(context);
    Hid* hid = context;
    const bool connected = (status == BtStatusConnected);
    notification_internal_message(
        hid->notifications, connected ? &sequence_set_blue_255 : &sequence_reset_blue);
    hid_keynote_set_connected_status(hid->hid_keynote, connected);
    hid_numpad_set_connected_status(hid->hid_numpad, connected);
    hid_movie_set_connected_status(hid->hid_movie, connected);
    hid_mouse_set_connected_status(hid->hid_mouse, connected);
    hid_keyboard_set_connected_status(hid->hid_keyboard, connected);
}

const char* hid_mode_name(HidMode mode) {
    static const char* const names[HidModeCount] = {
        [HidModeMovie] = "Movie",
        [HidModeMouse] = "Mouse",
        [HidModeKeynote] = "Keynote",
        [HidModeKeyboard] = "Keyboard",
        [HidModeNumpad] = "Numpad",
    };
    furi_check(mode < HidModeCount);
    return names[mode];
}

static const HidMode hid_app_modes[] = HID_APP_MODES;

size_t hid_mode_count(void) {
    return COUNT_OF(hid_app_modes);
}

HidMode hid_mode_at(size_t index) {
    furi_check(index < COUNT_OF(hid_app_modes));
    return hid_app_modes[index];
}

static void hid_mode_apply(Hid* app) {
    HidView view_id = HidViewMovie;

    switch(app->mode_current) {
    case HidModeMovie:
        view_id = HidViewMovie;
        hid_movie_set_orientation(app->hid_movie, app->vertical);
        break;
    case HidModeMouse:
        view_id = HidViewMouse;
        hid_mouse_set_orientation(app->hid_mouse, app->vertical);
        break;
    case HidModeKeynote:
        view_id = HidViewKeynote;
        hid_keynote_set_orientation(app->hid_keynote, app->vertical);
        break;
    case HidModeKeyboard:
        // Horizontal only, the key grid needs the full width
        view_id = HidViewKeyboard;
        break;
    case HidModeNumpad:
        view_id = HidViewNumpad;
        break;
    default:
        furi_crash();
    }

    view_dispatcher_switch_to_view(app->view_dispatcher, view_id);
}

static bool hid_mode_load_vertical(void) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    File* file = storage_file_alloc(storage);
    // Upright unless the user chose Horizontal in the menu
    uint8_t value = 1;
    if(storage_file_open(file, HID_SETTINGS_PATH, FSAM_READ, FSOM_OPEN_EXISTING)) {
        if(storage_file_read(file, &value, sizeof(value)) != sizeof(value)) value = 1;
    }
    storage_file_close(file);
    storage_file_free(file);
    furi_record_close(RECORD_STORAGE);
    return value == 1;
}

void hid_mode_set_vertical(Hid* app, bool vertical) {
    app->vertical = vertical;

    Storage* storage = furi_record_open(RECORD_STORAGE);
    File* file = storage_file_alloc(storage);
    uint8_t value = vertical ? 1 : 0;
    if(storage_file_open(file, HID_SETTINGS_PATH, FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
        storage_file_write(file, &value, sizeof(value));
    }
    storage_file_close(file);
    storage_file_free(file);
    furi_record_close(RECORD_STORAGE);
}

void hid_mode_draw_hint(Canvas* canvas, bool vertical) {
    canvas_set_font(canvas, FontSecondary);
    if(vertical) {
        canvas_draw_icon(canvas, 0, 104, &I_Pin_back_arrow_10x8);
        elements_multiline_text_aligned(canvas, 13, 112, AlignLeft, AlignBottom, "2x: next");
        elements_multiline_text_aligned(canvas, 13, 124, AlignLeft, AlignBottom, "Hold: back");
    } else {
        canvas_draw_icon(canvas, 0, 54, &I_Pin_back_arrow_10x8);
        elements_multiline_text_aligned(canvas, 13, 62, AlignLeft, AlignBottom, "2x: next");
    }
}

void hid_mode_show(Hid* app, HidMode mode) {
    furi_check(mode < HidModeCount);
    if(mode != app->mode_current) {
        app->mode_previous = app->mode_current;
        app->mode_current = mode;
    }
    hid_mode_apply(app);
}

void hid_mode_toggle(Hid* app) {
    HidMode mode = app->mode_previous;
    app->mode_previous = app->mode_current;
    app->mode_current = mode;
    hid_mode_apply(app);
    notification_message(app->notifications, &sequence_single_vibro);
}

void hid_mode_next(Hid* app) {
    size_t index = 0;
    while(index < hid_mode_count() && hid_mode_at(index) != app->mode_current)
        index++;
    app->mode_previous = app->mode_current;
    app->mode_current = hid_mode_at((index + 1) % hid_mode_count());
    hid_mode_apply(app);
    notification_message(app->notifications, &sequence_single_vibro);
}

void hid_mode_back_single(Hid* app) {
    // Late timer event after the second tap already made it a double tap
    if(!app->back_tap_pending) return;
    app->back_tap_pending = false;

    switch(app->mode_current) {
    case HidModeMouse:
        hid_mouse_right_click(app->hid_mouse);
        break;
    case HidModeKeyboard:
    case HidModeNumpad:
        hid_hal_keyboard_press(app, HID_KEYBOARD_DELETE);
        hid_hal_keyboard_release(app, HID_KEYBOARD_DELETE);
        break;
    case HidModeKeynote:
        hid_hal_keyboard_press(app, HID_KEYBOARD_DELETE);
        hid_hal_keyboard_release(app, HID_KEYBOARD_DELETE);
        hid_hal_consumer_key_press(app, HID_CONSUMER_AC_BACK);
        hid_hal_consumer_key_release(app, HID_CONSUMER_AC_BACK);
        break;
    default:
        break;
    }
}

static void hid_mode_back_tap_timer_callback(void* context) {
    Hid* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, HidCustomEventBackSingle);
}

void hid_mode_back_long(Hid* app) {
    app->back_long = true;
    app->back_menu_sent = false;
    app->back_repeats = 0;
}

bool hid_mode_back_event(Hid* app, InputEvent* event) {
    if(event->key != InputKeyBack) return false;

    if(event->type == InputTypeShort) {
        // The single-press action waits until it is clear that no second tap follows
        if(app->back_tap_pending) {
            app->back_tap_pending = false;
            furi_timer_stop(app->back_tap_timer);
            view_dispatcher_send_custom_event(app->view_dispatcher, HidCustomEventModeNext);
        } else {
            app->back_tap_pending = true;
            furi_timer_start(app->back_tap_timer, furi_ms_to_ticks(HID_MODE_DOUBLE_TAP_MS));
        }
        return true;
    }

    if(!app->back_long) return false;

    if(event->type == InputTypeRepeat) {
        // Still held after the long press: open the mode menu once
        if(!app->back_menu_sent && ++app->back_repeats >= HID_MODE_MENU_HOLD_REPEATS) {
            app->back_menu_sent = true;
            view_dispatcher_send_custom_event(app->view_dispatcher, HidCustomEventModeMenu);
        }
        return true;
    }

    if(event->type == InputTypeRelease) {
        // Released before the menu opened: swap to the other mode
        if(!app->back_menu_sent) {
            view_dispatcher_send_custom_event(app->view_dispatcher, HidCustomEventModeToggle);
        }
        app->back_long = false;
    }

    // The view still gets the release, to clear its pressed state
    return false;
}

Hid* hid_alloc() {
    Hid* app = malloc(sizeof(Hid));

    // Gui
    app->gui = furi_record_open(RECORD_GUI);

    // Bt
    app->bt = furi_record_open(RECORD_BT);

    // Notifications
    app->notifications = furi_record_open(RECORD_NOTIFICATION);

    // View dispatcher
    app->view_dispatcher = view_dispatcher_alloc();
    view_dispatcher_set_event_callback_context(app->view_dispatcher, app);
    view_dispatcher_set_custom_event_callback(app->view_dispatcher, hid_custom_event_callback);
    view_dispatcher_set_navigation_event_callback(app->view_dispatcher, hid_back_event_callback);
    view_dispatcher_attach_to_gui(app->view_dispatcher, app->gui, ViewDispatcherTypeFullscreen);

    // Scene Manager
    app->scene_manager = scene_manager_alloc(&hid_scene_handlers, app);

    // Device Type Submenu view
    app->submenu = submenu_alloc();

    view_dispatcher_add_view(app->view_dispatcher, HidViewSubmenu, submenu_get_view(app->submenu));

    app->variable_item_list = variable_item_list_alloc();
    view_dispatcher_add_view(
        app->view_dispatcher,
        HidViewMouseSettings,
        variable_item_list_get_view(app->variable_item_list));

    // Dialog view
    app->dialog = dialog_ex_alloc();
    view_dispatcher_add_view(app->view_dispatcher, HidViewDialog, dialog_ex_get_view(app->dialog));

    // Popup view
    app->popup = popup_alloc();
    view_dispatcher_add_view(app->view_dispatcher, HidViewPopup, popup_get_view(app->popup));

    // Keynote view
    app->hid_keynote = hid_keynote_alloc(app);
    view_dispatcher_add_view(
        app->view_dispatcher, HidViewKeynote, hid_keynote_get_view(app->hid_keynote));

    // Numpad view
    app->hid_numpad = hid_numpad_alloc(app);
    view_dispatcher_add_view(
        app->view_dispatcher, HidViewNumpad, hid_numpad_get_view(app->hid_numpad));

    // Movie view
    app->hid_movie = hid_movie_alloc(app);
    view_dispatcher_add_view(
        app->view_dispatcher, HidViewMovie, hid_movie_get_view(app->hid_movie));

    // Mouse view
    bm_settings_load(&app->mouse_settings);
    app->hid_mouse = hid_mouse_alloc(app, &app->mouse_settings);
    view_dispatcher_add_view(
        app->view_dispatcher, HidViewMouse, hid_mouse_get_view(app->hid_mouse));

    // Keyboard view
    app->hid_keyboard = hid_keyboard_alloc(app);
    view_dispatcher_add_view(
        app->view_dispatcher, HidViewKeyboard, hid_keyboard_get_view(app->hid_keyboard));

    app->back_tap_timer =
        furi_timer_alloc(hid_mode_back_tap_timer_callback, FuriTimerTypeOnce, app);

    // Long Back swaps between these two until another mode is picked in the menu
    app->mode_current = hid_mode_at(0);
    app->mode_previous = hid_mode_at(1);
    app->vertical = hid_mode_load_vertical();

    return app;
}

void hid_free(Hid* app) {
    furi_assert(app);

    // Reset notification
#ifdef HID_TRANSPORT_BLE
    notification_internal_message(app->notifications, &sequence_reset_blue);
#endif
    // Free views
    view_dispatcher_remove_view(app->view_dispatcher, HidViewSubmenu);
    submenu_free(app->submenu);
    view_dispatcher_remove_view(app->view_dispatcher, HidViewMouseSettings);
    variable_item_list_free(app->variable_item_list);
    view_dispatcher_remove_view(app->view_dispatcher, HidViewDialog);
    dialog_ex_free(app->dialog);
    view_dispatcher_remove_view(app->view_dispatcher, HidViewPopup);
    popup_free(app->popup);
    view_dispatcher_remove_view(app->view_dispatcher, HidViewKeynote);
    hid_keynote_free(app->hid_keynote);
    view_dispatcher_remove_view(app->view_dispatcher, HidViewNumpad);
    hid_numpad_free(app->hid_numpad);
    view_dispatcher_remove_view(app->view_dispatcher, HidViewMovie);
    hid_movie_free(app->hid_movie);
    view_dispatcher_remove_view(app->view_dispatcher, HidViewMouse);
    hid_mouse_free(app->hid_mouse);
    view_dispatcher_remove_view(app->view_dispatcher, HidViewKeyboard);
    hid_keyboard_free(app->hid_keyboard);
    furi_timer_free(app->back_tap_timer);
    scene_manager_free(app->scene_manager);
    view_dispatcher_free(app->view_dispatcher);

    // Close records
    furi_record_close(RECORD_GUI);
    app->gui = NULL;
    furi_record_close(RECORD_NOTIFICATION);
    app->notifications = NULL;
    furi_record_close(RECORD_BT);
    app->bt = NULL;

    // Free rest
    free(app);
}

int32_t hid_usb_app(void* p) {
    UNUSED(p);
    Hid* app = hid_alloc();

    FURI_LOG_D("HID", "Starting as USB app");

    FuriHalUsbInterface* usb_mode_prev = furi_hal_usb_get_config();
    furi_hal_usb_unlock();
    furi_check(furi_hal_usb_set_config(&usb_hid, NULL) == true);

    dolphin_deed(DolphinDeedPluginStart);

    scene_manager_next_scene(app->scene_manager, HidSceneStart);
    scene_manager_next_scene(app->scene_manager, HidSceneMain);

    view_dispatcher_run(app->view_dispatcher);

    // The mouse motion thread sends reports; stop it while USB HID is still active
    hid_mouse_stop(app->hid_mouse);

    furi_hal_usb_set_config(usb_mode_prev, NULL);

    hid_free(app);

    return 0;
}

int32_t hid_ble_app(void* p) {
    UNUSED(p);
    Hid* app = hid_alloc();

    FURI_LOG_D("HID", "Starting as BLE app");

    bt_disconnect(app->bt);

    // Wait 2nd core to update nvm storage
    furi_delay_ms(200);

    // Use the pairing keys of the stock Remote app. Both apps appear to the PC as the
    // same Bluetooth device, so one pairing serves both.
    Storage* storage = furi_record_open(RECORD_STORAGE);

    storage_common_mkdir(storage, HID_BT_KEYS_STORAGE_DIR);

    bt_keys_storage_set_storage_path(
        app->bt, HID_BT_KEYS_STORAGE_DIR "/" HID_BT_KEYS_STORAGE_NAME);

    furi_record_close(RECORD_STORAGE);

    app->ble_hid_profile = bt_profile_start(app->bt, ble_profile_hid, NULL);

    furi_check(app->ble_hid_profile);

    furi_hal_bt_start_advertising();
    bt_set_status_changed_callback(app->bt, bt_hid_connection_status_changed_callback, app);

    dolphin_deed(DolphinDeedPluginStart);

    scene_manager_next_scene(app->scene_manager, HidSceneStart);
    scene_manager_next_scene(app->scene_manager, HidSceneMain);

    view_dispatcher_run(app->view_dispatcher);

    // The mouse motion thread sends reports; stop it while the profile still exists
    hid_mouse_stop(app->hid_mouse);

    bt_set_status_changed_callback(app->bt, NULL, NULL);

    bt_disconnect(app->bt);

    // Wait 2nd core to update nvm storage
    furi_delay_ms(200);

    bt_keys_storage_set_default_path(app->bt);

    furi_check(bt_profile_restore_default(app->bt));

    hid_free(app);

    return 0;
}
