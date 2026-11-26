#include "hid_movie.h"
#include <furi.h>
#include <furi_hal_usb_hid.h>
#include <extra_profiles/hid_profile.h>
#include <gui/elements.h>
#include "../hid.h"

#include "hid_icons.h"

#define TAG "HidMovie"

// While Left/Right stays held, another speed step follows after this many repeat events
#define HID_MOVIE_SPEED_REPEATS   4
// Default keys of the "Video Speed Controller" browser extension
#define HID_MOVIE_SPEED_DOWN_KEY  HID_KEYBOARD_S // -0.1
#define HID_MOVIE_SPEED_UP_KEY    HID_KEYBOARD_D // +0.1
#define HID_MOVIE_SPEED_RESET_KEY HID_KEYBOARD_R // back to 1.0

struct HidMovie {
    View* view;
    Hid* hid;
    uint8_t repeats; // Repeat events since the last speed step
    bool ok_long; // OK was held: speed reset sent, no play/pause on release
};

typedef struct {
    bool left_pressed;
    bool up_pressed;
    bool right_pressed;
    bool down_pressed;
    bool ok_pressed;
    bool connected;
    bool back_pressed;
} HidMovieModel;

static void hid_movie_draw_arrow(Canvas* canvas, int32_t x, int32_t y, CanvasDirection dir) {
    canvas_draw_triangle(canvas, x, y, 5, 3, dir);
    if(dir == CanvasDirectionBottomToTop) {
        canvas_draw_dot(canvas, x, y - 1);
    } else if(dir == CanvasDirectionTopToBottom) {
        canvas_draw_dot(canvas, x, y + 1);
    } else if(dir == CanvasDirectionRightToLeft) {
        canvas_draw_dot(canvas, x - 1, y);
    } else if(dir == CanvasDirectionLeftToRight) {
        canvas_draw_dot(canvas, x + 1, y);
    }
}

static void hid_movie_draw(Canvas* canvas, HidMovieModel* model, bool vertical) {
    // Top left corner of the d-pad circle and of the Back button
    const int32_t kx = vertical ? 6 : 58;
    const int32_t ky = vertical ? 20 : 3;
    const int32_t bx = vertical ? 42 : 107;
    const int32_t by = vertical ? 76 : 33;

    // Header
#ifdef HID_TRANSPORT_BLE
    if(model->connected) {
        canvas_draw_icon(canvas, 0, 0, &I_Ble_connected_15x15);
    } else {
        canvas_draw_icon(canvas, 0, 0, &I_Ble_disconnected_15x15);
    }
#endif

    canvas_set_font(canvas, FontPrimary);
    elements_multiline_text_aligned(canvas, 17, 3, AlignLeft, AlignTop, "Movie");
    canvas_set_font(canvas, FontSecondary);

    // Keypad circles
    if(vertical) {
        canvas_draw_icon(canvas, kx, ky, &I_Circles_51x51);
        canvas_draw_icon(canvas, bx, by, &I_BackCircle_19x19);
    } else {
        canvas_draw_icon(canvas, kx, ky, &I_OutCircles_70x51);
    }

    // Up
    if(model->up_pressed) {
        canvas_set_bitmap_mode(canvas, true);
        canvas_draw_icon(canvas, kx + 10, ky + 3, &I_S_UP_31x15);
        canvas_set_bitmap_mode(canvas, false);
        canvas_set_color(canvas, ColorWhite);
    }
    canvas_draw_icon(canvas, kx + 21, ky + 6, &I_Volup_8x6);
    canvas_set_color(canvas, ColorBlack);

    // Down
    if(model->down_pressed) {
        canvas_set_bitmap_mode(canvas, true);
        canvas_draw_icon(canvas, kx + 10, ky + 33, &I_S_DOWN_31x15);
        canvas_set_bitmap_mode(canvas, false);
        canvas_set_color(canvas, ColorWhite);
    }
    canvas_draw_icon(canvas, kx + 22, ky + 38, &I_Voldwn_6x6);
    canvas_set_color(canvas, ColorBlack);

    // Left
    if(model->left_pressed) {
        canvas_set_bitmap_mode(canvas, true);
        canvas_draw_icon(canvas, kx + 3, ky + 10, &I_S_LEFT_15x31);
        canvas_set_bitmap_mode(canvas, false);
        canvas_set_color(canvas, ColorWhite);
    }
    hid_movie_draw_arrow(canvas, kx + 7, ky + 25, CanvasDirectionRightToLeft);
    hid_movie_draw_arrow(canvas, kx + 12, ky + 25, CanvasDirectionRightToLeft);
    canvas_set_color(canvas, ColorBlack);

    // Right
    if(model->right_pressed) {
        canvas_set_bitmap_mode(canvas, true);
        canvas_draw_icon(canvas, kx + 33, ky + 10, &I_S_RIGHT_15x31);
        canvas_set_bitmap_mode(canvas, false);
        canvas_set_color(canvas, ColorWhite);
    }
    hid_movie_draw_arrow(canvas, kx + 38, ky + 25, CanvasDirectionLeftToRight);
    hid_movie_draw_arrow(canvas, kx + 43, ky + 25, CanvasDirectionLeftToRight);
    canvas_set_color(canvas, ColorBlack);

    // Ok
    if(model->ok_pressed) {
        canvas_set_bitmap_mode(canvas, true);
        canvas_draw_icon(canvas, kx + 16, ky + 16, &I_Pressed_Button_19x19);
        canvas_set_bitmap_mode(canvas, false);
        canvas_set_color(canvas, ColorWhite);
    }
    hid_movie_draw_arrow(canvas, kx + 22, ky + 25, CanvasDirectionLeftToRight);
    canvas_draw_line(canvas, kx + 26, ky + 23, kx + 26, ky + 27);
    canvas_draw_line(canvas, kx + 28, ky + 23, kx + 28, ky + 27);
    canvas_set_color(canvas, ColorBlack);

    // Exit
    if(model->back_pressed) {
        canvas_set_bitmap_mode(canvas, true);
        canvas_draw_icon(canvas, bx, by, &I_Pressed_Button_19x19);
        canvas_set_bitmap_mode(canvas, false);
        canvas_set_color(canvas, ColorWhite);
    }
    canvas_draw_icon(canvas, bx + 4, by + 5, &I_Pin_back_arrow_10x10);
    canvas_set_color(canvas, ColorBlack);

    hid_mode_draw_hint(canvas, vertical);
}

static void hid_movie_draw_callback(Canvas* canvas, void* context) {
    furi_assert(context);
    hid_movie_draw(canvas, context, false);
}

static void hid_movie_draw_vertical_callback(Canvas* canvas, void* context) {
    furi_assert(context);
    hid_movie_draw(canvas, context, true);
}

void hid_movie_set_orientation(HidMovie* hid_movie, bool vertical) {
    furi_assert(hid_movie);
    view_set_draw_callback(
        hid_movie->view, vertical ? hid_movie_draw_vertical_callback : hid_movie_draw_callback);
    view_set_orientation(
        hid_movie->view, vertical ? ViewOrientationVerticalFlip : ViewOrientationHorizontal);
}

static void hid_movie_key_tap(HidMovie* hid_movie, uint16_t key) {
    hid_hal_keyboard_press(hid_movie->hid, key);
    furi_delay_ms(20);
    hid_hal_keyboard_release(hid_movie->hid, key);
}

static uint16_t hid_movie_seek_key(InputKey key) {
    return key == InputKeyLeft ? HID_KEYBOARD_LEFT_ARROW : HID_KEYBOARD_RIGHT_ARROW;
}

static void hid_movie_set_pressed(HidMovie* hid_movie, InputKey key, bool pressed) {
    with_view_model(
        hid_movie->view,
        HidMovieModel * model,
        {
            if(key == InputKeyUp) {
                model->up_pressed = pressed;
            } else if(key == InputKeyDown) {
                model->down_pressed = pressed;
            } else if(key == InputKeyLeft) {
                model->left_pressed = pressed;
            } else if(key == InputKeyRight) {
                model->right_pressed = pressed;
            } else if(key == InputKeyOk) {
                model->ok_pressed = pressed;
            } else if(key == InputKeyBack) {
                model->back_pressed = pressed;
            }
        },
        true);
}

// Left/Right: tap = seek, hold = slower/faster (one more step every half second or so)
static void hid_movie_process_left_right(HidMovie* hid_movie, InputEvent* event) {
    uint16_t speed_key = event->key == InputKeyLeft ? HID_MOVIE_SPEED_DOWN_KEY :
                                                      HID_MOVIE_SPEED_UP_KEY;

    if(event->type == InputTypeShort) {
        hid_movie_key_tap(hid_movie, hid_movie_seek_key(event->key));
    } else if(event->type == InputTypeLong) {
        hid_movie->repeats = 0;
        hid_movie_key_tap(hid_movie, speed_key);
    } else if(event->type == InputTypeRepeat) {
        if(++hid_movie->repeats >= HID_MOVIE_SPEED_REPEATS) {
            hid_movie->repeats = 0;
            hid_movie_key_tap(hid_movie, speed_key);
        }
    }
}

// OK: tap = play/pause, hold = playback speed back to 1.0
static void hid_movie_process_ok(HidMovie* hid_movie, InputEvent* event) {
    if(event->type == InputTypePress) {
        hid_movie->ok_long = false;
    } else if(event->type == InputTypeLong) {
        hid_movie->ok_long = true;
        hid_movie_key_tap(hid_movie, HID_MOVIE_SPEED_RESET_KEY);
    } else if(event->type == InputTypeShort && !hid_movie->ok_long) {
        hid_hal_consumer_key_press(hid_movie->hid, HID_CONSUMER_PLAY_PAUSE);
        furi_delay_ms(20);
        hid_hal_consumer_key_release(hid_movie->hid, HID_CONSUMER_PLAY_PAUSE);
    }
}

// Up/Down: volume while held, as before
static void hid_movie_process_up_down(HidMovie* hid_movie, InputEvent* event) {
    uint16_t key = event->key == InputKeyUp ? HID_CONSUMER_VOLUME_INCREMENT :
                                              HID_CONSUMER_VOLUME_DECREMENT;
    if(event->type == InputTypePress) {
        hid_hal_consumer_key_press(hid_movie->hid, key);
    } else if(event->type == InputTypeRelease) {
        hid_hal_consumer_key_release(hid_movie->hid, key);
    }
}

static bool hid_movie_input_callback(InputEvent* event, void* context) {
    furi_assert(context);
    HidMovie* hid_movie = context;

    if(hid_mode_back_event(hid_movie->hid, event)) return true;

    if(event->type == InputTypePress) {
        hid_movie_set_pressed(hid_movie, event->key, true);
    } else if(event->type == InputTypeRelease) {
        hid_movie_set_pressed(hid_movie, event->key, false);
    }

    if(event->type == InputTypeLong && event->key == InputKeyBack) {
        hid_hal_keyboard_release_all(hid_movie->hid);
        hid_mode_back_long(hid_movie->hid);
    } else if(event->key == InputKeyLeft || event->key == InputKeyRight) {
        hid_movie_process_left_right(hid_movie, event);
    } else if(event->key == InputKeyOk) {
        hid_movie_process_ok(hid_movie, event);
    } else if(event->key == InputKeyUp || event->key == InputKeyDown) {
        hid_movie_process_up_down(hid_movie, event);
    }

    return true;
}

HidMovie* hid_movie_alloc(Hid* hid) {
    HidMovie* hid_movie = malloc(sizeof(HidMovie));
    hid_movie->view = view_alloc();
    hid_movie->hid = hid;
    hid_movie->repeats = 0;
    hid_movie->ok_long = false;
    view_set_context(hid_movie->view, hid_movie);
    view_allocate_model(hid_movie->view, ViewModelTypeLocking, sizeof(HidMovieModel));
    view_set_draw_callback(hid_movie->view, hid_movie_draw_callback);
    view_set_input_callback(hid_movie->view, hid_movie_input_callback);

    return hid_movie;
}

void hid_movie_free(HidMovie* hid_movie) {
    furi_assert(hid_movie);
    view_free(hid_movie->view);
    free(hid_movie);
}

View* hid_movie_get_view(HidMovie* hid_movie) {
    furi_assert(hid_movie);
    return hid_movie->view;
}

void hid_movie_set_connected_status(HidMovie* hid_movie, bool connected) {
    furi_assert(hid_movie);
    with_view_model(
        hid_movie->view, HidMovieModel * model, { model->connected = connected; }, true);
}
