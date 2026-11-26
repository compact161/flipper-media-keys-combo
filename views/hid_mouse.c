#include "hid_mouse.h"
#include <gui/elements.h>
#include <input/input.h>
#include <furi_hal_rtc.h>
#include <notification/notification_messages.h>
#include <math.h>
#include "../hid.h"

#include "hid_icons.h"

#define TAG "HidMouse"

/* Mouse mode with the motion engine of Better Mouse by timon
   (https://github.com/Timon-sys/Bettermouse, GPL-3.0), adapted to this app:
   reports go through hid_hal_*, so it works over Bluetooth and USB, the keys
   follow the upright layout, and Back keeps the mode gestures of this app.

   Movement is driven by a fixed-rate motion loop, not by the 150 ms key repeat
   of the GUI. Key state comes straight off the input service pubsub: the GUI
   thread dispatches input and redraws, so a release queued behind a redraw
   would arrive late and the pointer would overshoot. */

#define BM_DIAG       0.70710678f /* keep diagonals the same speed as axes */
#define BM_REPORT_MAX 127 /* HID reports are int8 */
#define BM_ICE_STOP   8.0f /* px/s below which a glide is over */
#define BM_BAR_MIN_MS 250u /* min gap between speed bar repaints */

#define BM_FLAG_STOP (1u << 0)

/* Only what the screen shows. Written by the motion thread, read by the draw
   callback. */
typedef struct {
    bool up;
    bool down;
    bool left;
    bool right;
    bool back_down;
    bool scroll_mode;
    bool drag_lock;
    bool btn_left;
    bool connected;
    bool ice;
    bool gliding;
    uint8_t speed_pct;
} HidMouseModel;

/* Live key state. Bytes and counters, written on the input service thread (and
   click_right on the GUI thread), read by the motion thread; no lock. */
typedef struct {
    volatile bool up;
    volatile bool down;
    volatile bool left;
    volatile bool right;

    volatile bool back_held;
    volatile bool ok_held;
    volatile bool ok_swallowed; /* this OK press already did something else */
    volatile bool drag_lock; /* owned by the motion thread, read here */

    /* Requests handed to the motion thread, which sends all reports. */
    volatile uint8_t click_left;
    volatile uint8_t click_right;
    volatile uint8_t cycle_mode;
    volatile uint8_t drag_release;
} HidMouseInput;

struct HidMouse {
    View* view;
    Hid* hid;
    const BmSettings* settings;

    FuriThread* thread;
    FuriPubSub* input_events;
    FuriPubSubSubscription* input_subscription;

    HidMouseInput input;
    volatile bool vertical;
    volatile bool connected;
    /* Motion thread started and not yet stopped */
    bool running;

    /* Owned by the motion thread. */
    float vx; /* px/s, only meaningful in ice mode */
    float vy;
    float acc_x; /* sub-pixel remainders */
    float acc_y;
    float acc_scroll;
    uint32_t hold_ms;
    bool scroll_mode;
};

/* ------------------------------------------------------------------ drawing */

static void hid_mouse_draw(Canvas* canvas, HidMouseModel* model, bool vertical) {
    // Top left corner of the d-pad circle and of the Back button
    const int32_t kx = vertical ? 6 : 58;
    const int32_t ky = vertical ? 20 : 3;
    const int32_t bx = vertical ? 42 : 107;
    const int32_t by = vertical ? 76 : 33;
    // Status text baseline and speed bar
    const int32_t sy = vertical ? 84 : 28;
    const int32_t bar_w = vertical ? 38 : 52;

    // Header
#ifdef HID_TRANSPORT_BLE
    if(model->connected) {
        canvas_draw_icon(canvas, 0, 0, &I_Ble_connected_15x15);
    } else {
        canvas_draw_icon(canvas, 0, 0, &I_Ble_disconnected_15x15);
    }
#endif

    canvas_set_font(canvas, FontPrimary);
    elements_multiline_text_aligned(canvas, 17, 3, AlignLeft, AlignTop, "Mouse");
    canvas_set_font(canvas, FontSecondary);

    const char* status = NULL;
    if(model->drag_lock) {
        status = "DRAG";
    } else if(model->scroll_mode) {
        status = "Scroll";
    } else if(model->ice) {
        status = model->gliding ? "Ice: glide" : "Ice";
    }
    if(status) canvas_draw_str(canvas, 0, sy, status);

    // Speed bar: where you are on the ramp
    canvas_draw_frame(canvas, 0, sy + 3, bar_w, 5);
    if(model->speed_pct > 0) {
        const int32_t w = ((bar_w - 2) * model->speed_pct) / 100;
        if(w > 0) canvas_draw_box(canvas, 1, sy + 4, w, 3);
    }

    hid_mode_draw_hint(canvas, vertical);

    // Keypad circles
    if(vertical) {
        canvas_draw_icon(canvas, kx, ky, &I_Circles_51x51);
        canvas_draw_icon(canvas, bx, by, &I_BackCircle_19x19);
    } else {
        canvas_draw_icon(canvas, kx, ky, &I_OutCircles_70x51);
    }

    // Up
    if(model->up) {
        canvas_set_bitmap_mode(canvas, true);
        canvas_draw_icon(canvas, kx + 10, ky + 3, &I_S_UP_31x15);
        canvas_set_bitmap_mode(canvas, false);
        canvas_set_color(canvas, ColorWhite);
    }
    canvas_draw_icon(canvas, kx + 22, ky + 5, &I_Pin_arrow_up_7x9);
    canvas_set_color(canvas, ColorBlack);

    // Down
    if(model->down) {
        canvas_set_bitmap_mode(canvas, true);
        canvas_draw_icon(canvas, kx + 10, ky + 33, &I_S_DOWN_31x15);
        canvas_set_bitmap_mode(canvas, false);
        canvas_set_color(canvas, ColorWhite);
    }
    canvas_draw_icon(canvas, kx + 22, ky + 37, &I_Pin_arrow_down_7x9);
    canvas_set_color(canvas, ColorBlack);

    // Left
    if(model->left) {
        canvas_set_bitmap_mode(canvas, true);
        canvas_draw_icon(canvas, kx + 3, ky + 10, &I_S_LEFT_15x31);
        canvas_set_bitmap_mode(canvas, false);
        canvas_set_color(canvas, ColorWhite);
    }
    canvas_draw_icon(canvas, kx + 5, ky + 22, &I_Pin_arrow_left_9x7);
    canvas_set_color(canvas, ColorBlack);

    // Right
    if(model->right) {
        canvas_set_bitmap_mode(canvas, true);
        canvas_draw_icon(canvas, kx + 33, ky + 10, &I_S_RIGHT_15x31);
        canvas_set_bitmap_mode(canvas, false);
        canvas_set_color(canvas, ColorWhite);
    }
    canvas_draw_icon(canvas, kx + 37, ky + 22, &I_Pin_arrow_right_9x7);
    canvas_set_color(canvas, ColorBlack);

    // Ok
    if(model->btn_left) {
        canvas_set_bitmap_mode(canvas, true);
        canvas_draw_icon(canvas, kx + 16, ky + 16, &I_Pressed_Button_19x19);
        canvas_set_bitmap_mode(canvas, false);
        canvas_set_color(canvas, ColorWhite);
    }
    canvas_draw_icon(canvas, kx + 21, ky + 21, &I_Left_mouse_icon_9x9);
    canvas_set_color(canvas, ColorBlack);

    // Back
    if(model->back_down) {
        canvas_set_bitmap_mode(canvas, true);
        canvas_draw_icon(canvas, bx, by, &I_Pressed_Button_19x19);
        canvas_set_bitmap_mode(canvas, false);
        canvas_set_color(canvas, ColorWhite);
    }
    canvas_draw_icon(canvas, bx + 5, by + 5, &I_Right_mouse_icon_9x9);
    canvas_set_color(canvas, ColorBlack);
}

static void hid_mouse_draw_callback(Canvas* canvas, void* context) {
    furi_assert(context);
    hid_mouse_draw(canvas, context, false);
}

static void hid_mouse_draw_vertical_callback(Canvas* canvas, void* context) {
    furi_assert(context);
    hid_mouse_draw(canvas, context, true);
}

void hid_mouse_set_orientation(HidMouse* hid_mouse, bool vertical) {
    furi_assert(hid_mouse);
    hid_mouse->vertical = vertical;
    view_set_draw_callback(
        hid_mouse->view, vertical ? hid_mouse_draw_vertical_callback : hid_mouse_draw_callback);
    view_set_orientation(
        hid_mouse->view, vertical ? ViewOrientationVerticalFlip : ViewOrientationHorizontal);
}

/* ---------------------------------------------------------------- input tap */

/* The pubsub delivers physical keys. Turn them the way the GUI does for the
   layout of this view (see view_port_map_input in the firmware). */
static InputKey hid_mouse_map_key(const HidMouse* hid_mouse, const InputEvent* event) {
    static const InputKey vertical_flip[InputKeyMAX] = {
        InputKeyLeft, InputKeyRight, InputKeyUp, InputKeyDown, InputKeyOk, InputKeyBack};
    static const InputKey left_hand[InputKeyMAX] = {
        InputKeyDown, InputKeyUp, InputKeyLeft, InputKeyRight, InputKeyOk, InputKeyBack};

    InputKey key = event->key;
    if(key >= InputKeyMAX || event->sequence_source != INPUT_SEQUENCE_SOURCE_HARDWARE) return key;

    if(hid_mouse->vertical) {
        key = vertical_flip[key];
    } else if(furi_hal_rtc_is_flag_set(FuriHalRtcFlagHandOrient)) {
        key = left_hand[key];
    }
    return key;
}

static void hid_mouse_input_event(const void* value, void* context) {
    const InputEvent* event = value;
    HidMouse* hid_mouse = context;
    HidMouseInput* in = &hid_mouse->input;

    switch(hid_mouse_map_key(hid_mouse, event)) {
    case InputKeyUp:
        if(event->type == InputTypePress) in->up = true;
        if(event->type == InputTypeRelease) in->up = false;
        break;
    case InputKeyDown:
        if(event->type == InputTypePress) in->down = true;
        if(event->type == InputTypeRelease) in->down = false;
        break;
    case InputKeyLeft:
        if(event->type == InputTypePress) in->left = true;
        if(event->type == InputTypeRelease) in->left = false;
        break;
    case InputKeyRight:
        if(event->type == InputTypePress) in->right = true;
        if(event->type == InputTypeRelease) in->right = false;
        break;

    case InputKeyOk:
        if(event->type == InputTypePress) {
            in->ok_held = true;
            in->ok_swallowed = false;
        } else if(event->type == InputTypeLong) {
            // Holding OK steps pointer -> scroll -> drag -> pointer
            if(!in->ok_swallowed) {
                in->ok_swallowed = true;
                in->cycle_mode++;
            }
        } else if(event->type == InputTypeShort) {
            /* A short OK while dragging drops the drag. Short arrives before
               Release, so the Release below does not also fire a click. */
            if(in->drag_lock && !in->ok_swallowed) {
                in->drag_release++;
                in->ok_swallowed = true;
            }
        } else if(event->type == InputTypeRelease) {
            if(in->ok_held && !in->ok_swallowed && !in->drag_lock) in->click_left++;
            in->ok_held = false;
        }
        break;

    case InputKeyBack:
        /* Only the held state; the Back gestures run in the view callback. */
        if(event->type == InputTypePress) in->back_held = true;
        if(event->type == InputTypeRelease) in->back_held = false;
        break;

    default:
        break;
    }
}

/* ------------------------------------------------------------ motion engine */

static int8_t hid_mouse_take_whole(float* accumulator) {
    /* Truncate toward zero and keep the remainder, so speeds below one pixel
       per tick still move the pointer instead of rounding away to nothing. */
    float whole = (*accumulator < 0.0f) ? ceilf(*accumulator) : floorf(*accumulator);

    /* Clamp to the int8 report range, leaving anything over in the accumulator
       so a delayed tick spreads over the next few instead of being dropped. */
    if(whole > (float)BM_REPORT_MAX) whole = (float)BM_REPORT_MAX;
    if(whole < (float)-BM_REPORT_MAX) whole = (float)-BM_REPORT_MAX;

    *accumulator -= whole;
    return (int8_t)whole;
}

static void hid_mouse_vibro(HidMouse* hid_mouse) {
    if(hid_mouse->settings->haptic) {
        notification_message(hid_mouse->hid->notifications, &sequence_single_vibro);
    }
}

static bool hid_mouse_is_connected(const HidMouse* hid_mouse) {
#ifdef HID_TRANSPORT_BLE
    return hid_mouse->connected;
#else
    UNUSED(hid_mouse);
    return furi_hal_hid_is_connected();
#endif
}

/* Act on everything the input side asked for. All reports are sent here. */
static void hid_mouse_service_requests(HidMouse* hid_mouse, bool connected) {
    HidMouseInput* in = &hid_mouse->input;
    Hid* hid = hid_mouse->hid;

    while(in->click_left > 0) {
        in->click_left--;
        if(connected) {
            hid_hal_mouse_press(hid, HID_MOUSE_BTN_LEFT);
            hid_hal_mouse_release(hid, HID_MOUSE_BTN_LEFT);
        }
        hid_mouse_vibro(hid_mouse);
    }

    while(in->click_right > 0) {
        in->click_right--;
        if(connected) {
            hid_hal_mouse_press(hid, HID_MOUSE_BTN_RIGHT);
            hid_hal_mouse_release(hid, HID_MOUSE_BTN_RIGHT);
        }
        hid_mouse_vibro(hid_mouse);
    }

    while(in->drag_release > 0) {
        in->drag_release--;
        if(connected) hid_hal_mouse_release(hid, HID_MOUSE_BTN_LEFT);
        in->drag_lock = false;
    }

    /* Pointer -> scroll -> drag (left button held) -> pointer */
    while(in->cycle_mode > 0) {
        in->cycle_mode--;
        if(hid_mouse->scroll_mode) {
            hid_mouse->scroll_mode = false;
            if(connected) hid_hal_mouse_press(hid, HID_MOUSE_BTN_LEFT);
            in->drag_lock = true;
        } else if(in->drag_lock) {
            if(connected) hid_hal_mouse_release(hid, HID_MOUSE_BTN_LEFT);
            in->drag_lock = false;
        } else {
            hid_mouse->scroll_mode = true;
        }
        hid_mouse_vibro(hid_mouse);
    }
}

static int32_t hid_mouse_thread(void* context) {
    HidMouse* hid_mouse = context;
    HidMouseInput* in = &hid_mouse->input;

    /* Redraws are throttled: the GUI service redraws on the same thread that
       dispatches input. */
    uint8_t last_drawn_pct = 255;
    uint32_t last_bar_tick = 0;

    /* Absolute deadlines plus a measured dt: a relative wait would let the
       period drift with the work time and the 1 ms tick phase, which shows as
       a slow speed wobble. */
    uint32_t next_deadline = furi_get_tick();
    uint32_t last_tick = next_deadline;

    while(true) {
        const BmSettings* settings = hid_mouse->settings;

        uint16_t tick_hz = settings->tick_hz;
        if(tick_hz < 10) tick_hz = 10;
        uint32_t tick_ms = 1000u / tick_hz;
        if(tick_ms < 1) tick_ms = 1;

        next_deadline += tick_ms;
        uint32_t now = furi_get_tick();
        int32_t remaining = (int32_t)(next_deadline - now);

        /* Far behind, or the rate just changed: resync instead of a burst. */
        if(remaining < 0 || remaining > (int32_t)(tick_ms * 4)) {
            next_deadline = now + tick_ms;
            remaining = (int32_t)tick_ms;
        }

        uint32_t flags =
            furi_thread_flags_wait(BM_FLAG_STOP, FuriFlagWaitAny, (uint32_t)remaining);
        if((flags & FuriFlagError) == 0 && (flags & BM_FLAG_STOP) != 0) break;

        now = furi_get_tick();
        uint32_t elapsed_ms = now - last_tick;
        last_tick = now;
        if(elapsed_ms == 0) elapsed_ms = 1;
        if(elapsed_ms > 200) elapsed_ms = 200; /* do not teleport after a stall */
        float dt = (float)elapsed_ms / 1000.0f;

        bool connected = hid_mouse_is_connected(hid_mouse);
        hid_mouse_service_requests(hid_mouse, connected);

        /* Sample the keys once per tick. */
        int8_t dir_x = (int8_t)((in->right ? 1 : 0) - (in->left ? 1 : 0));
        int8_t dir_y = 0;
        int8_t scroll_dir = 0;

        if(hid_mouse->scroll_mode) {
            scroll_dir = (int8_t)((in->up ? 1 : 0) - (in->down ? 1 : 0));
        } else {
            dir_y = (int8_t)((in->down ? 1 : 0) - (in->up ? 1 : 0));
        }

        if(settings->invert_y) dir_y = (int8_t)-dir_y;
        if(settings->invert_scroll) scroll_dir = (int8_t)-scroll_dir;

        bool moving = (dir_x != 0 || dir_y != 0);

        float max_speed = (float)settings->max_speed;
        float min_speed = (float)settings->min_speed;
        /* Ramp: eases in from min_speed to max_speed over accel_ms. */
        if(moving) {
            hid_mouse->hold_ms += elapsed_ms;
        } else {
            hid_mouse->hold_ms = 0;
        }

        float ramp = 1.0f;
        if(settings->accel_ms > 0) {
            ramp = (float)hid_mouse->hold_ms / (float)settings->accel_ms;
            if(ramp > 1.0f) ramp = 1.0f;
            ramp = ramp * ramp; /* gentle at first, quick once moving */
        }

        float speed = min_speed + (max_speed - min_speed) * ramp;
        if(speed < 0.0f) speed = 0.0f;

        float target_vx = (float)dir_x * speed;
        float target_vy = (float)dir_y * speed;

        /* Without this, diagonals are 1.41x faster than the axes. */
        if(dir_x != 0 && dir_y != 0) {
            target_vx *= BM_DIAG;
            target_vy *= BM_DIAG;
        }

        bool gliding = false;

        if(settings->ice) {
            /* Ice mode: velocity has momentum. Keys push it toward the target,
               and letting go coasts to a stop instead of cutting out. */
            float accel_ms = (float)settings->accel_ms;
            if(accel_ms < 1.0f) accel_ms = 1.0f;
            float max_delta = max_speed * (dt * 1000.0f / accel_ms);

            float ddx = target_vx - hid_mouse->vx;
            float ddy = target_vy - hid_mouse->vy;

            if(moving) {
                if(ddx > max_delta) ddx = max_delta;
                if(ddx < -max_delta) ddx = -max_delta;
                if(ddy > max_delta) ddy = max_delta;
                if(ddy < -max_delta) ddy = -max_delta;
                hid_mouse->vx += ddx;
                hid_mouse->vy += ddy;
            } else {
                /* Exponential coast: halves every ice_glide_ms. */
                float glide_ms = (float)settings->ice_glide_ms;
                if(glide_ms < 1.0f) glide_ms = 1.0f;
                float decay = powf(0.5f, (dt * 1000.0f) / glide_ms);
                hid_mouse->vx *= decay;
                hid_mouse->vy *= decay;

                if(fabsf(hid_mouse->vx) < BM_ICE_STOP) hid_mouse->vx = 0.0f;
                if(fabsf(hid_mouse->vy) < BM_ICE_STOP) hid_mouse->vy = 0.0f;
                gliding = (hid_mouse->vx != 0.0f || hid_mouse->vy != 0.0f);
            }
        } else {
            /* Default: velocity follows the keys exactly, so releasing stops
               dead on the very next tick. */
            hid_mouse->vx = target_vx;
            hid_mouse->vy = target_vy;

            if(!moving) {
                hid_mouse->acc_x = 0.0f;
                hid_mouse->acc_y = 0.0f;
            }
        }

        hid_mouse->acc_x += hid_mouse->vx * dt;
        hid_mouse->acc_y += hid_mouse->vy * dt;

        int8_t dx = hid_mouse_take_whole(&hid_mouse->acc_x);
        int8_t dy = hid_mouse_take_whole(&hid_mouse->acc_y);

        /* One combined report per tick, so diagonals are real diagonals. */
        if(connected && (dx != 0 || dy != 0)) hid_hal_mouse_move(hid_mouse->hid, dx, dy);

        if(scroll_dir != 0) {
            float scroll_speed = (float)settings->scroll_speed;
            hid_mouse->acc_scroll += (float)scroll_dir * scroll_speed * dt;
            int8_t notches = hid_mouse_take_whole(&hid_mouse->acc_scroll);
            if(connected && notches != 0) hid_hal_mouse_scroll(hid_mouse->hid, notches);
        } else {
            hid_mouse->acc_scroll = 0.0f;
        }

        /* Mirror state to the screen, the bar quantised to 20% and rate
           limited. Button and direction changes repaint at once. */
        float shown = sqrtf(hid_mouse->vx * hid_mouse->vx + hid_mouse->vy * hid_mouse->vy);
        uint8_t speed_pct = 0;
        if(max_speed > 0.0f) {
            float pct = (shown * 100.0f) / max_speed;
            if(pct > 100.0f) pct = 100.0f;
            speed_pct = (uint8_t)(((uint8_t)pct / 20u) * 20u);
        }

        bool bar_due = (speed_pct != last_drawn_pct) &&
                       ((uint32_t)(now - last_bar_tick) >= BM_BAR_MIN_MS || speed_pct == 0 ||
                        speed_pct == 100);
        if(bar_due) last_bar_tick = now;

        bool redraw = false;
        with_view_model(
            hid_mouse->view,
            HidMouseModel * model,
            {
                /* One declarator per line: a comma here would split the macro
                   argument. */
                bool up = in->up;
                bool down = in->down;
                bool left = in->left;
                bool right = in->right;
                bool back_down = in->back_held;
                bool drag = in->drag_lock;

                if(model->up != up || model->down != down || model->left != left ||
                   model->right != right || model->back_down != back_down ||
                   model->drag_lock != drag || model->btn_left != (in->ok_held || drag) ||
                   model->scroll_mode != hid_mouse->scroll_mode || model->connected != connected ||
                   model->gliding != gliding || model->ice != (bool)settings->ice || bar_due) {
                    model->up = up;
                    model->down = down;
                    model->left = left;
                    model->right = right;
                    model->back_down = back_down;
                    model->drag_lock = drag;
                    model->btn_left = (in->ok_held || drag);
                    model->scroll_mode = hid_mouse->scroll_mode;
                    model->connected = connected;
                    model->gliding = gliding;
                    model->ice = settings->ice;
                    model->speed_pct = speed_pct;
                    last_drawn_pct = speed_pct;
                    redraw = true;
                }
            },
            redraw);
    }

    return 0;
}

/* --------------------------------------------------------------------- view */

static bool hid_mouse_input_callback(InputEvent* event, void* context) {
    furi_assert(context);
    HidMouse* hid_mouse = context;

    /* Arrows and OK are taken from the input service. Only Back is handled
       here, for the mode gestures of the app. */
    if(event->key != InputKeyBack) return true;

    if(hid_mode_back_event(hid_mouse->hid, event)) return true;

    if(event->type == InputTypeLong) hid_mode_back_long(hid_mouse->hid);

    return true;
}

static void hid_mouse_enter_callback(void* context) {
    HidMouse* hid_mouse = context;

    memset(&hid_mouse->input, 0, sizeof(HidMouseInput));
    hid_mouse->vx = 0.0f;
    hid_mouse->vy = 0.0f;
    hid_mouse->acc_x = 0.0f;
    hid_mouse->acc_y = 0.0f;
    hid_mouse->acc_scroll = 0.0f;
    hid_mouse->hold_ms = 0;
    hid_mouse->scroll_mode = false;

    with_view_model(
        hid_mouse->view,
        HidMouseModel * model,
        {
            memset(model, 0, sizeof(HidMouseModel));
            model->connected = hid_mouse_is_connected(hid_mouse);
            model->ice = hid_mouse->settings->ice;
        },
        true);

    hid_mouse->input_subscription =
        furi_pubsub_subscribe(hid_mouse->input_events, hid_mouse_input_event, hid_mouse);

    hid_mouse->thread = furi_thread_alloc_ex("HidMouseMotion", 2048, hid_mouse_thread, hid_mouse);
    /* Above the GUI service, so a tick lands on time even mid-redraw. The loop
       sleeps between ticks and never spins. */
    furi_thread_set_priority(hid_mouse->thread, FuriThreadPriorityHigh);
    hid_mouse->running = true;
    furi_thread_start(hid_mouse->thread);
}

void hid_mouse_stop(HidMouse* hid_mouse) {
    furi_assert(hid_mouse);

    if(hid_mouse->input_subscription) {
        furi_pubsub_unsubscribe(hid_mouse->input_events, hid_mouse->input_subscription);
        hid_mouse->input_subscription = NULL;
    }

    if(hid_mouse->thread) {
        furi_thread_flags_set(furi_thread_get_id(hid_mouse->thread), BM_FLAG_STOP);
        furi_thread_join(hid_mouse->thread);
        furi_thread_free(hid_mouse->thread);
        hid_mouse->thread = NULL;
    }

    /* Never leave a button stuck down on the host. Only while the motion
       thread ran: after hid_mouse_stop() at app exit the link may be gone. */
    if(hid_mouse->running && hid_mouse_is_connected(hid_mouse)) {
        hid_hal_mouse_release_all(hid_mouse->hid);
    }
    hid_mouse->running = false;
}

static void hid_mouse_exit_callback(void* context) {
    hid_mouse_stop(context);
}

HidMouse* hid_mouse_alloc(Hid* hid, const BmSettings* settings) {
    HidMouse* hid_mouse = malloc(sizeof(HidMouse));
    memset(hid_mouse, 0, sizeof(HidMouse));
    hid_mouse->hid = hid;
    hid_mouse->settings = settings;
    hid_mouse->input_events = furi_record_open(RECORD_INPUT_EVENTS);

    hid_mouse->view = view_alloc();
    view_set_context(hid_mouse->view, hid_mouse);
    /* Lock free: view_draw() holds the model mutex for the whole draw
       callback, which would stall the motion thread once per tick. Every field
       is a byte and only the motion thread writes it. */
    view_allocate_model(hid_mouse->view, ViewModelTypeLockFree, sizeof(HidMouseModel));
    view_set_draw_callback(hid_mouse->view, hid_mouse_draw_callback);
    view_set_input_callback(hid_mouse->view, hid_mouse_input_callback);
    view_set_enter_callback(hid_mouse->view, hid_mouse_enter_callback);
    view_set_exit_callback(hid_mouse->view, hid_mouse_exit_callback);
    return hid_mouse;
}

void hid_mouse_free(HidMouse* hid_mouse) {
    furi_assert(hid_mouse);
    furi_record_close(RECORD_INPUT_EVENTS);
    view_free(hid_mouse->view);
    free(hid_mouse);
}

View* hid_mouse_get_view(HidMouse* hid_mouse) {
    furi_assert(hid_mouse);
    return hid_mouse->view;
}

void hid_mouse_set_connected_status(HidMouse* hid_mouse, bool connected) {
    furi_assert(hid_mouse);
    hid_mouse->connected = connected;
    with_view_model(
        hid_mouse->view, HidMouseModel * model, { model->connected = connected; }, true);
}

void hid_mouse_right_click(HidMouse* hid_mouse) {
    furi_assert(hid_mouse);
    hid_mouse->input.click_right++;
}
