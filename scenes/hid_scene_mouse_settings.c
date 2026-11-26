#include "../hid.h"
#include "../views.h"

#include <stddef.h>

/* Settings of the Mouse mode, taken from Better Mouse by timon (GPL-3.0).
   Every numeric setting is a range, so adding one is a table row. */
typedef struct {
    const char* label;
    const BmRange* range;
    const char* unit;
    size_t offset; /* into BmSettings */
    bool wide; /* field is uint16_t rather than uint8_t */
} HidMouseTunable;

#define HID_MOUSE_TUNABLE(lbl, rng, un, field, is_wide) \
    {.label = (lbl),                                    \
     .range = (rng),                                    \
     .unit = (un),                                      \
     .offset = offsetof(BmSettings, field),             \
     .wide = (is_wide)}

static const HidMouseTunable hid_mouse_tunables[] = {
    HID_MOUSE_TUNABLE("Max speed", &bm_range_max_speed, "/s", max_speed, true),
    HID_MOUSE_TUNABLE("Pre-ramp", &bm_range_min_speed, "/s", min_speed, true),
    HID_MOUSE_TUNABLE("Accel ramp", &bm_range_accel, "ms", accel_ms, true),
    HID_MOUSE_TUNABLE("Scroll speed", &bm_range_scroll, "/s", scroll_speed, false),
    HID_MOUSE_TUNABLE("Report rate", &bm_range_tick, "Hz", tick_hz, true),
    HID_MOUSE_TUNABLE("Ice glide", &bm_range_ice_glide, "ms", ice_glide_ms, true),
};

static uint16_t hid_mouse_tunable_get(const Hid* app, const HidMouseTunable* t) {
    const uint8_t* base = (const uint8_t*)&app->mouse_settings;
    if(t->wide) {
        uint16_t v;
        memcpy(&v, base + t->offset, sizeof(v));
        return v;
    }
    return *(base + t->offset);
}

static void hid_mouse_tunable_set(Hid* app, const HidMouseTunable* t, uint16_t value) {
    uint8_t* base = (uint8_t*)&app->mouse_settings;
    if(t->wide) {
        memcpy(base + t->offset, &value, sizeof(value));
    } else {
        *(base + t->offset) = (uint8_t)value;
    }
}

static void hid_mouse_tunable_changed(VariableItem* item) {
    Hid* app = variable_item_get_context(item);
    uint8_t list_index = variable_item_list_get_selected_item_index(app->variable_item_list);
    if(list_index >= COUNT_OF(hid_mouse_tunables)) return;

    const HidMouseTunable* t = &hid_mouse_tunables[list_index];
    uint16_t value = bm_range_value(t->range, variable_item_get_current_value_index(item));
    hid_mouse_tunable_set(app, t, value);
    snprintf(app->value_text, sizeof(app->value_text), "%u%s", (unsigned)value, t->unit);
    variable_item_set_current_value_text(item, app->value_text);
}

static void hid_mouse_toggle_changed(VariableItem* item, uint8_t* field) {
    uint8_t index = variable_item_get_current_value_index(item);
    *field = index;
    variable_item_set_current_value_text(item, bm_toggle_names[index]);
}

static void hid_mouse_setting_ice(VariableItem* item) {
    Hid* app = variable_item_get_context(item);
    hid_mouse_toggle_changed(item, &app->mouse_settings.ice);
}

static void hid_mouse_setting_invert_y(VariableItem* item) {
    Hid* app = variable_item_get_context(item);
    hid_mouse_toggle_changed(item, &app->mouse_settings.invert_y);
}

static void hid_mouse_setting_invert_scroll(VariableItem* item) {
    Hid* app = variable_item_get_context(item);
    hid_mouse_toggle_changed(item, &app->mouse_settings.invert_scroll);
}

static void hid_mouse_setting_haptic(VariableItem* item) {
    Hid* app = variable_item_get_context(item);
    hid_mouse_toggle_changed(item, &app->mouse_settings.haptic);
}

static void hid_mouse_add_toggle(
    Hid* app,
    const char* label,
    uint8_t value,
    VariableItemChangeCallback callback) {
    VariableItem* item = variable_item_list_add(app->variable_item_list, label, 2, callback, app);
    variable_item_set_current_value_index(item, value);
    variable_item_set_current_value_text(item, bm_toggle_names[value]);
}

void hid_scene_mouse_settings_on_enter(void* context) {
    Hid* app = context;

    for(size_t i = 0; i < COUNT_OF(hid_mouse_tunables); i++) {
        const HidMouseTunable* t = &hid_mouse_tunables[i];
        uint16_t value = hid_mouse_tunable_get(app, t);
        VariableItem* item = variable_item_list_add(
            app->variable_item_list, t->label, t->range->count, hid_mouse_tunable_changed, app);
        variable_item_set_current_value_index(item, bm_range_index(t->range, value));
        snprintf(app->value_text, sizeof(app->value_text), "%u%s", (unsigned)value, t->unit);
        variable_item_set_current_value_text(item, app->value_text);
    }

    hid_mouse_add_toggle(app, "Ice mode", app->mouse_settings.ice, hid_mouse_setting_ice);
    hid_mouse_add_toggle(
        app, "Invert Y", app->mouse_settings.invert_y, hid_mouse_setting_invert_y);
    hid_mouse_add_toggle(
        app, "Invert scroll", app->mouse_settings.invert_scroll, hid_mouse_setting_invert_scroll);
    hid_mouse_add_toggle(app, "Click vibro", app->mouse_settings.haptic, hid_mouse_setting_haptic);

    view_dispatcher_switch_to_view(app->view_dispatcher, HidViewMouseSettings);
}

bool hid_scene_mouse_settings_on_event(void* context, SceneManagerEvent event) {
    UNUSED(context);
    UNUSED(event);
    return false;
}

void hid_scene_mouse_settings_on_exit(void* context) {
    Hid* app = context;
    bm_settings_save(&app->mouse_settings);
    variable_item_list_reset(app->variable_item_list);
}
