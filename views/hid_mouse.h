#pragma once

#include <gui/view.h>

#include "../bm_settings.h"

typedef struct Hid Hid;
typedef struct HidMouse HidMouse;

HidMouse* hid_mouse_alloc(Hid* bt_hid, const BmSettings* settings);

void hid_mouse_free(HidMouse* hid_mouse);

View* hid_mouse_get_view(HidMouse* hid_mouse);

void hid_mouse_set_connected_status(HidMouse* hid_mouse, bool connected);

void hid_mouse_set_orientation(HidMouse* hid_mouse, bool vertical);

/** Right click, sent by the motion thread on its next tick */
void hid_mouse_right_click(HidMouse* hid_mouse);

/** Stop the motion thread and release the buttons. Call before the HID link
    goes away; the view exit later does nothing more. */
void hid_mouse_stop(HidMouse* hid_mouse);
