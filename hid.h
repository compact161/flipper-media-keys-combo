#pragma once

#include <furi.h>
#include <furi_hal_bt.h>
#include <furi_hal_usb.h>
#include <furi_hal_usb_hid.h>

#include <extra_profiles/hid_profile.h>

#include <bt/bt_service/bt.h>
#include <gui/gui.h>
#include <gui/view.h>
#include <gui/view_dispatcher.h>
#include <gui/scene_manager.h>
#include <notification/notification.h>
#include <storage/storage.h>

#include <gui/modules/submenu.h>
#include <gui/modules/variable_item_list.h>
#include <gui/modules/dialog_ex.h>
#include <gui/modules/popup.h>
#include "views/hid_keynote.h"
#include "views/hid_numpad.h"
#include "views/hid_movie.h"
#include "views/hid_mouse.h"
#include "views/hid_keyboard.h"

#include "scenes/hid_scene.h"

#define HID_BT_KEYS_STORAGE_NAME ".bt_hid.keys"
#define HID_SETTINGS_PATH        APP_DATA_PATH("orientation")
// Data folder of the stock Remote app (appid hid_ble)
#define HID_BT_KEYS_STORAGE_DIR  EXT_PATH("apps_data/hid_ble")

// Only Bluetooth has a link to wait for; over USB a view always counts as connected
#ifdef HID_TRANSPORT_BLE
#define hid_model_connected(model) ((model)->connected)
#else
#define hid_model_connected(model) (true)
#endif

typedef struct Hid Hid;

// Modes the app can show
typedef enum {
    HidModeMovie,
    HidModeMouse,
    HidModeKeynote,
    HidModeKeyboard,
    HidModeNumpad,
    HidModeCount,
} HidMode;

// Which modes this app carries, in the order of the mode ring
#if defined(HID_APP_MEDIA)
#define HID_APP_MODES {HidModeMovie, HidModeMouse}
#elif defined(HID_APP_KEYS)
#define HID_APP_MODES {HidModeMouse, HidModeKeynote, HidModeKeyboard, HidModeNumpad}
#else
#error "Define HID_APP_MEDIA or HID_APP_KEYS"
#endif

typedef enum {
    HidCustomEventModeToggle = 100,
    HidCustomEventModeMenu,
    HidCustomEventModeNext,
    HidCustomEventBackSingle,
} HidCustomEvent;

// Back repeats (about 150 ms each) after the long press that open the mode menu
#define HID_MODE_MENU_HOLD_REPEATS 7
// A second short Back press within this time makes a double tap
#define HID_MODE_DOUBLE_TAP_MS     350

struct Hid {
    FuriHalBleProfileBase* ble_hid_profile;
    Bt* bt;
    Gui* gui;
    NotificationApp* notifications;
    ViewDispatcher* view_dispatcher;
    SceneManager* scene_manager;
    Submenu* submenu;
    VariableItemList* variable_item_list;
    DialogEx* dialog;
    Popup* popup;
    HidKeynote* hid_keynote;
    HidNumpad* hid_numpad;
    HidMovie* hid_movie;
    HidMouse* hid_mouse;
    HidKeyboard* hid_keyboard;
    BmSettings mouse_settings;
    // Scratch for formatting setting values; the list copies the string
    char value_text[16];
    HidMode mode_current;
    HidMode mode_previous;
    // Flipper held upright. Numpad has an upright layout only.
    bool vertical;
    FuriTimer* back_tap_timer;
    bool back_tap_pending;
    bool back_long;
    bool back_menu_sent;
    uint8_t back_repeats;
};

void bt_hid_remove_pairing(Hid* app);

void hid_hal_keyboard_press(Hid* instance, uint16_t event);
void hid_hal_keyboard_release(Hid* instance, uint16_t event);
void hid_hal_keyboard_release_all(Hid* instance);

void hid_hal_consumer_key_press(Hid* instance, uint16_t event);
void hid_hal_consumer_key_release(Hid* instance, uint16_t event);
void hid_hal_consumer_key_release_all(Hid* instance);

void hid_hal_mouse_move(Hid* instance, int8_t dx, int8_t dy);
void hid_hal_mouse_scroll(Hid* instance, int8_t delta);
void hid_hal_mouse_press(Hid* instance, uint16_t event);
void hid_hal_mouse_release(Hid* instance, uint16_t event);
void hid_hal_mouse_release_all(Hid* instance);

const char* hid_mode_name(HidMode mode);

// Number of modes in this app and the mode at a position of the ring
size_t hid_mode_count(void);

HidMode hid_mode_at(size_t index);
// Show a mode. Remembers the mode it replaces, so a long Back press can return to it.
void hid_mode_show(Hid* app, HidMode mode);
// Change between the horizontal and the upright layouts and remember the choice
void hid_mode_set_vertical(Hid* app, bool vertical);
// Back button hint shared by the views
void hid_mode_draw_hint(Canvas* canvas, bool vertical);
// Swap the current and the previous mode
void hid_mode_toggle(Hid* app);
// Go to the next mode in menu order
void hid_mode_next(Hid* app);
// Do what a single short Back press means in the current mode
void hid_mode_back_single(Hid* app);
// Called by a view on a long Back press, after it released its keys
void hid_mode_back_long(Hid* app);
// Called by a view first for every input event; true if the event is used up
bool hid_mode_back_event(Hid* app, InputEvent* event);
