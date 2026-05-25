#include "raw_hid.h"
#include "usb_descriptor.h"
#include QMK_KEYBOARD_H

#define HEARTBEAT_TIMEOUT_MS 500

static void process_shared_keys_remote(uint32_t keys);
static void shared_key_event_local(uint8_t key, bool pressed);
static void shared_keys_send(void);
static void shared_key_event(uint8_t key, bool pressed);

// enum layers {
//     _BASE,
// };

enum custom_keycodes {
    _SK_LY_START = SAFE_RANGE,
    _SK_LY_END = _SK_LY_START + 4,
};

enum shared_keys {
    _SK_DRAG_SCROLL,
    _SK_NUM,
    _SK_FUN,
    _SK_SYM,
    _SK_NAV,
};

#define SK_LY(x) (_SK_LY_START + (x) - 1)

static uint32_t last_heartbeat_time = 0;
static uint8_t raw_hid_report[RAW_EPSIZE];

static uint32_t shared_keys_local = 0;
static uint32_t shared_keys_remote = 0;

// TODO -- Figure out how to factor this out
void raw_hid_receive(uint8_t *data, uint8_t length) {
    if (length == 0) {
        return;
    }
    if (data[0] == 0xC0) {
        last_heartbeat_time = timer_read32();
        shared_keys_send();
    } else if (data[0] == 0xC1) {
        if (length >= 5) {
            process_shared_keys_remote(data[1] | (data[2] << 8) | (data[3] << 16) | (data[4] << 24));
        }
    }
}

// TODO -- Figure out how to factor this out
void housekeeping_task_user() {
    if (timer_elapsed32(last_heartbeat_time) > HEARTBEAT_TIMEOUT_MS) {
        process_shared_keys_remote(0);
    }
}

// TODO -- Should be factored out to common module
static void process_shared_keys_remote(uint32_t keys) {
    if (keys != shared_keys_remote) {
        uint8_t i, b;
        for (i = 0; i < 32; i++) {
            b = 1 << i;
            if (keys & b) {
                if (!(shared_keys_remote & b)) {
                    shared_key_event(i, true);
                }
            } else {
                if (shared_keys_remote & b) {
                    shared_key_event(i, false);
                }
            }
        }
        shared_keys_remote = keys;
    }
}

// TODO -- Should be factored out to common module
static void shared_key_event_local(uint8_t key, bool pressed) {
    if (key >= 32) {
        return;
    }
    uint32_t keys = shared_keys_local;
    if (pressed) {
        keys |= (1 << key);
    } else {
        keys &= ~(1 << key);
    }
    if (keys != shared_keys_local) {
        if ((shared_keys_local | shared_keys_remote) != (keys | shared_keys_remote)) {
            shared_key_event(key, pressed);
        }
        shared_keys_local = keys;
        shared_keys_send();
    }
}

// TODO -- Should be factored out to common module
static void shared_keys_send(void) {
    memset(raw_hid_report, 0, sizeof(raw_hid_report));
    raw_hid_report[0] = 0xC0;
    raw_hid_report[1] = shared_keys_local & 0xFF;
    raw_hid_report[2] = (shared_keys_local >> 8) & 0xFF;
    raw_hid_report[3] = (shared_keys_local >> 16) & 0xFF;
    raw_hid_report[4] = (shared_keys_local >> 24) & 0xFF;
    raw_hid_send(raw_hid_report, RAW_EPSIZE);
}

// TODO -- This could be a weak override
extern bool is_drag_scroll;
static void shared_key_event(uint8_t key, bool pressed) {
    // action_t action = {};
    switch (key) {
        default:
            return;
        case _SK_DRAG_SCROLL:
            is_drag_scroll = pressed;
            return;
            // action.code = DRAG_SCROLL;
            // break;
    }
    // keyrecord_t record = { .event = MAKE_KEYEVENT(0, 0, pressed) };
    // process_action(&record, action);
}


enum {
    TD_OMNI,
};

void tap_dance_omni_finished(tap_dance_state_t *state, void *user_data) {
    if (state->count == 5) {
        bootloader_jump();
    }
}

void tap_dance_omni_each(tap_dance_state_t *state, void *user_data) {
    if (state->count == 1) {
        shared_key_event_local(_SK_NUM, true);
    }
}

void tap_dance_omni_each_release(tap_dance_state_t *state, void *user_data) {
    if (state->count == 1) {
        shared_key_event_local(_SK_NUM, false);
    }
}

tap_dance_action_t tap_dance_actions[] = {
    [TD_OMNI] = ACTION_TAP_DANCE_FN_ADVANCED_WITH_RELEASE(tap_dance_omni_each, tap_dance_omni_each_release, tap_dance_omni_finished, NULL),
};

uint16_t get_tapping_term(uint16_t keycode, keyrecord_t *record) {
    switch (keycode) {
        case TD(TD_OMNI):
            return 250;
        default:
            return TAPPING_TERM;
    }
}

bool process_record_user(uint16_t keycode, keyrecord_t *record) {
    if (keycode >= _SK_LY_START && keycode < _SK_LY_END) {
        shared_key_event_local(keycode - _SK_LY_START + 1, record->event.pressed);
        return false;
    }
    return true;
}

const uint16_t PROGMEM keymaps[][MATRIX_ROWS][MATRIX_COLS] = {
    [0] = LAYOUT( TD(TD_OMNI), SK_LY(_SK_NAV), SK_LY(_SK_SYM), SK_LY(_SK_FUN), MS_BTN1, MS_BTN2 )
};

