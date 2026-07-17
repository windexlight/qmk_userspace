#include "action.h"
#include "action_util.h"
#include "keyboard.h"
#include "progmem.h"
#include "raw_hid.h"
#include "timer.h"
#include "usb_descriptor.h"
#include "host.h"
#include <assert.h>
#include QMK_KEYBOARD_H

void send_keyboard_user(report_keyboard_t* report);
void send_nkro_user(report_nkro_t* report);
void send_extra_user(report_extra_t* report);
uint8_t USAGE2KEYCODE(uint16_t usage);

static void send_raw_hid_report(void);
static void process_shared_keys_remote(uint32_t keys);
static void shared_key_event_local(uint8_t key, bool pressed);
static void shared_keys_send(void);
static void shared_key_event(uint8_t key, bool pressed);

enum layers {
    _MAGIC_STURDY,
    _QWERTY_NVIM,
    _NAV_LAYER,
    _NAV_L_LAYER,
    _SYM_L_LAYER,
    _SYM_R_LAYER,
    _NUM_LAYER,
    _NUM_NVIM_LAYER,
    _FUN_LAYER,
};

enum shared_keys {
    _SK_DRAG_SCROLL,
    _SK_NUM,
    _SK_FUN,
    _SK_SYM,
    _SK_NAV,
    _SK_NVIM = 30,
    _SK_NVIM_NORMAL,
};

#define MAGIC QK_AREP

#define _BAK LALT(KC_LEFT)
#define _FND LCTL(KC_F)
#define _FWD LALT(KC_RGHT)

#define _UNDO LCTL(KC_Z)
#define _REDO LCTL(KC_Y)
#define _REDO2 LSFT(LCTL(KC_Z))
#define _CUT LCTL(KC_X)
#define _COPY LCTL(KC_C)
#define _WIN KC_LGUI
#define _PSTE LCTL(KC_V)
#define _BTAB LSFT(KC_TAB)
#define _SAVE LCTL(KC_S)
#define _ALL LCTL(KC_A)
#define _ATAB LALT(KC_TAB)
#define _GTAB LGUI(KC_TAB)
#define _EXIT LALT(KC_F4)
#define _NTAB LCTL(KC_T)
#define _CLSE LCTL(KC_W)
#define _NEW LCTL(KC_N)
#define _EXPL LGUI(KC_E)

#define _CTL(x) LCTL_T(x)
#define _SFT(x) LSFT_T(x)
#define _GUI(x) LGUI_T(x)
#define _ALT(x) LALT_T(x)

#define _NAV(x) LT(_NAV_LAYER, x)
#define _SYM_L(x) LT(_SYM_L_LAYER, x)
#define _SYM_R(x) LT(_SYM_R_LAYER, x)
#define _NUM(x) LT(_NUM_LAYER, x)
#define _FUN(x) LT(_FUN_LAYER, x)

#define HEARTBEAT_TIMEOUT_MS 2000

enum custom_keycodes {
    TSL_NUM = SAFE_RANGE,
    // https://github.com/getreuer/qmk-keymap/blob/main/getreuer.c
    // Macros invoked through the Magic key.
    OSL_NUM,
    UPDIR,
    M_N,
    M_DOCSTR,
    M_EQEQ,
    M_INCLUDE,
    M_ION,
    M_MENT,
    M_MKGRVS,
    M_QUEN,
    M_THE,
    M_CAP_THE,
    M_TMENT,
    M_UPDIR,
    M_NBSP,
    M_BEFORE,
    M_JUST,
    M_NION,
    M_VER,
    M_WHICH,
    M_XES,
    M_BUT,
    M_LL,
    M_VE,
    M_NOOP,
    _SK_START,
    _SK_END = _SK_START + 32,
};

#define _SK(x) (_SK_START + (x))
#define SK_DS _SK(_SK_DRAG_SCROLL)

typedef struct {
    keypos_t pos;
    uint16_t keycode;
} key_needing_release_t;

#define MAX_KEYS_NEEDING_RELEASE 10
static key_needing_release_t keys_needing_release[MAX_KEYS_NEEDING_RELEASE];
static uint8_t keys_needing_release_count = 0;
static uint8_t tsl_count = 0;
static uint32_t last_heartbeat_time = 0;
static uint8_t raw_hid_report[RAW_EPSIZE];
static bool suppress_real_reports = false;
static bool send_raw_hid_reports = false;
static bool nvim_active = false;

// static bool host_connection = false;
static uint32_t shared_keys_local = 0;
static uint32_t shared_keys_remote = 0;

static report_nkro_t nkro_report_user = {
    .report_id = REPORT_ID_NKRO,
    .mods = 0,
    .bits = {0},
};

void (*send_keyboard_real)(report_keyboard_t *) = NULL;
void (*send_nkro_real)(report_nkro_t *) = NULL;
void (*send_extra_real)(report_extra_t *) = NULL;

extern matrix_row_t matrix[MATRIX_ROWS];

extern host_driver_t chibios_driver;


// https://github.com/getreuer/qmk-keymap/blob/main/getreuer.c
// An enhanced version of SEND_STRING: if Caps Word is active, the Shift key is
// held while sending the string. Additionally, the last key is set such that if
// the Repeat Key is pressed next, it produces `repeat_keycode`. This helper is
// used for several macros below in my process_record_user() function.
// windexlight modifications - clear non-shift mods. If shft_str is provided,
// clear shift as well and send shft_str if shift was active, else send str.
#define MAGIC_STRING(str, shft_str, repeat_keycode) \
    magic_send_string_P(PSTR(str), PSTR(shft_str), (repeat_keycode))
static void magic_send_string_P(const char* str, const char* shft_str, uint16_t repeat_keycode) {
    uint8_t saved_mods = 0;
    // If Caps Word is on, save the mods and hold Shift.
    if (is_caps_word_on()) {
        saved_mods = get_mods();
        register_mods(MOD_BIT_LSHIFT);
    }

    uint8_t mods = get_mods();
    uint8_t weak_mods = get_weak_mods();
    uint8_t oneshot_mods = get_oneshot_mods();
    if (shft_str != NULL) {
        clear_mods();
        clear_weak_mods();
        clear_oneshot_mods();
        if (((mods | weak_mods | oneshot_mods) & MOD_MASK_SHIFT) != 0) {
            send_string_P(shft_str); // Send the shifted string.
        } else {
            send_string_P(str); // Send un-shifted string.
        }
    } else {
        set_mods(mods & MOD_MASK_SHIFT);
        set_weak_mods(weak_mods & MOD_MASK_SHIFT);
        set_oneshot_mods(oneshot_mods & MOD_MASK_SHIFT);
        send_string_P(str); // Send the string.
    }
    set_last_keycode(repeat_keycode);
    set_last_mods(get_mods());
    set_mods(mods);
    set_weak_mods(weak_mods);
    set_oneshot_mods(oneshot_mods);

    // If Caps Word is on, restore the mods.
    if (is_caps_word_on()) {
        set_mods(saved_mods);
    }
}

// Can somehow get stuck on a layer, like right symbol layer, for example... how?
bool process_record_user(uint16_t keycode, keyrecord_t *record) {
#ifdef CONSOLE_ENABLE
    uprintf("KL: kc: 0x%04X, col: %2u, row: %2u, pressed: %u, time: %5u, int: %u, count: %u\n", keycode, record->event.key.col, record->event.key.row, record->event.pressed, record->event.time, record->tap.interrupted, record->tap.count);
#endif
    bool ret = true;
    if (keycode >= _SK_START && keycode < _SK_END) {
        shared_key_event_local(keycode - _SK_START, record->event.pressed);
        return false;
    }
    if (keycode >= TSL_NUM && keycode <= OSL_NUM) {
        if (record->event.pressed) {
            if (tsl_count > 0) {
                tsl_count = 0;
                layer_off(_NUM_NVIM_LAYER);
            } else {
                tsl_count = 1 + (TSL_NUM - keycode);
                layer_move(_NUM_NVIM_LAYER);
            }
        }
        return false;
    }
    // if (keycode == _NAV(KC_T)) {
    //     if (record->event.pressed) {
    //         if (!host_keyboard_led_state().scroll_lock) {
    //             tap_scrl_no_mods();
    //         }
    //     } else {
    //         if (host_keyboard_led_state().scroll_lock) {
    //             tap_scrl_no_mods();
    //         }
    //     }
    // }
    // https://github.com/getreuer/qmk-keymap/blob/main/getreuer.c
    // If alt repeating key A, E, I, O, U, Y with no mods other than Shift, set
    // the last key to KC_N. Above, alternate repeat of KC_N is defined to be
    // again KC_N. This way, either tapping alt repeat and then repeat (or
    // equivalently double tapping alt repeat) is useful to type certain patterns
    // without SFBs:
    //
    //   D <altrep> <rep> -> DYN (as in "dynamic")
    //   O <altrep> <rep> -> OAN (as in "loan")
    int8_t rep_count = get_repeat_key_count();
    if (rep_count < 0) {
        if (((get_mods() | get_weak_mods() | get_oneshot_mods()) & ~MOD_MASK_SHIFT) == 0) {
            if (keycode == KC_A || keycode == KC_E || keycode == KC_I ||
                keycode == KC_O || keycode == KC_U || keycode == KC_Y) {
                set_last_keycode(M_N);
                set_last_mods(get_mods());
            } else if (keycode == KC_QUOT) {
                set_last_keycode(M_LL);
                set_last_mods(get_mods());
            }
        }
    }
    if (record->event.pressed) {
        if (tsl_count > 0) {
            if (keycode < KC_1 || keycode > KC_0) {
                tsl_count = 0;
                layer_off(_NUM_NVIM_LAYER);
            } else if (--tsl_count == 0) {
                layer_off(_NUM_NVIM_LAYER);
                if (keys_needing_release_count < MAX_KEYS_NEEDING_RELEASE) {
                    keys_needing_release[keys_needing_release_count++] = (key_needing_release_t){ .pos = record->event.key, .keycode = keycode };
                }
            }
        }
        switch (keycode) {
            // https://github.com/getreuer/qmk-keymap/blob/main/getreuer.c
            // Macros invoked through the MAGIC key.
            case M_THE:     MAGIC_STRING(/* */"the", "The", M_N); break;
            case M_CAP_THE: MAGIC_STRING(/* */"The", "The", M_N); break;
            case M_ION:     MAGIC_STRING(/*i*/"on", NULL, KC_S); break;
            case M_MENT:    MAGIC_STRING(/*m*/"ent", NULL, KC_S); break;
            case M_QUEN:    MAGIC_STRING(/*q*/"uen", NULL, KC_C); break;
            case M_TMENT:   MAGIC_STRING(/*t*/"ment", NULL, KC_S); break;
            case M_UPDIR:   MAGIC_STRING(/*.*/"./", NULL, UPDIR); break;
            case M_INCLUDE: SEND_STRING_DELAY(/*#*/"include ", TAP_CODE_DELAY); break;
            case M_EQEQ:    SEND_STRING_DELAY(/*=*/"==", TAP_CODE_DELAY); break;
            case M_NBSP:    SEND_STRING_DELAY(/*&*/"nbsp;", TAP_CODE_DELAY); break;
            case M_BEFORE:  MAGIC_STRING(/*b*/"efore", NULL, M_NOOP); break;
            case M_JUST:    MAGIC_STRING(/*j*/"ust", NULL, M_NOOP); break;
            case M_NION:    MAGIC_STRING(/*n*/"ion", NULL, KC_S); break;
            case M_VER:     MAGIC_STRING(/*v*/"er", NULL, KC_S); break;
            case M_WHICH:   MAGIC_STRING(/*w*/"hich", NULL, M_NOOP); break;
            case M_XES:     MAGIC_STRING(/*x*/"es", NULL, M_NOOP); break;
            case M_BUT:     MAGIC_STRING(/*,*/" but", NULL, M_NOOP); break;
            case M_LL:      MAGIC_STRING(/*I'*/"ll", NULL, M_NOOP); break;
            case M_VE:      MAGIC_STRING(/*I'*/"ve", NULL, M_NOOP); break;

            case M_DOCSTR:
                SEND_STRING_DELAY(/*"*/"\"\"\"\"\""
                                  SS_TAP(X_LEFT) SS_TAP(X_LEFT) SS_TAP(X_LEFT), TAP_CODE_DELAY);
                break;
            case M_MKGRVS:
                SEND_STRING_DELAY(/*`*/"``\n\n```" SS_TAP(X_UP), TAP_CODE_DELAY);
                break;
            case UPDIR:
                SEND_STRING_DELAY("../", TAP_CODE_DELAY);
                break;
            // case KC_SPC:
            //     // https://github.com/getreuer/qmk-keymap/blob/main/getreuer.c
            //     // When the Repeat key follows Space, it behaves as one-shot shift.
            //     // TODO: This isn't working very well. Think it probably conflicts
            //     // with my alternate one-shot mods, and also it's still sending a space,
            //     // need to set ret = false, I think.
            //     if (get_repeat_key_count() > 0) {
            //         if (record->event.pressed) {
            //             add_oneshot_mods(MOD_LSFT);
            //             register_mods(MOD_LSFT);
            //         } else {
            //             unregister_mods(MOD_LSFT);
            //         }
            //     } else {
            //         magic = false;
            //     }
            //     break;
        }
        // Repeat key overrides
        if (rep_count > 0) {
            ret = false;
            switch (keycode) {
                case M_N: MAGIC_STRING(/*n*/"n", /*n*/"n", M_NOOP); break;
                default:
                    switch (keycode & 0xFF) {
                        case KC_A: MAGIC_STRING(/*a*/"nd", /*a*/"nd", M_NOOP); break;
                        case KC_I: MAGIC_STRING(/*i*/"ng", /*i*/"ng", KC_S); break;
                        case KC_Y: MAGIC_STRING(/*y*/"ou", /*y*/"ou", M_NOOP); break;
                        case KC_N: MAGIC_STRING(/*n*/"f", /*n*/"f", M_NOOP); break;
                        case KC_B: MAGIC_STRING(/*b*/"ecause", /*b*/"ecause", M_NOOP); break;
                        case KC_W: MAGIC_STRING(/*w*/"ould", /*w*/"ould", M_NOOP); break;
                        case KC_COMM: MAGIC_STRING(/*,*/" and", NULL, M_NOOP); break;
                        case KC_SPC: MAGIC_STRING(/* */"for", "For", M_NOOP); break;
                        case KC_QUOT: MAGIC_STRING(/*'*/"ll", NULL, M_NOOP); break;
                        default: ret = true; break;
                    }
                    break;
            }
        }
        if (keycode == KC_DEL) {
            if (((get_mods() | get_weak_mods() | get_oneshot_mods()) & MOD_MASK_SHIFT) != 0) {
                tap_code(KC_MINS);
                ret = false;
            }
        }
    } else { // Release
        for (uint8_t i = 0; i < keys_needing_release_count; i++) {
            if (keys_needing_release[i].pos.col == record->event.key.col &&
                keys_needing_release[i].pos.row == record->event.key.row) {
                unregister_code(keys_needing_release[i].keycode);
                for (uint8_t j = i; j < keys_needing_release_count - 1; j++) {
                    keys_needing_release[j] = keys_needing_release[j+1];
                }
                keys_needing_release_count--;
                ret = false;
                break;
            }
        }
    }
    return ret;
}

void keyboard_post_init_user(void) {
    // TODO - This is probably brittle to use chibios_driver, check here if breaks with future changes
    host_driver_t *driver = &chibios_driver; //host_get_driver(); <- can't use here, too early
    send_keyboard_real = driver->send_keyboard;
    send_nkro_real = driver->send_nkro;
    send_extra_real = driver->send_extra;
    driver->send_keyboard = send_keyboard_user;
    driver->send_nkro = send_nkro_user;
    driver->send_extra = send_extra_user;
}

void send_keyboard_user(report_keyboard_t* report) {
    if (!suppress_real_reports) {
        (*send_keyboard_real)(report);
    }
    if (send_raw_hid_reports) {
        nkro_report_user.mods = report->mods;
        memset(&nkro_report_user.bits, 0, sizeof(nkro_report_user.bits));
        uint8_t code;
        for (int i=0; i<KEYBOARD_REPORT_KEYS; i++) {
            code = report->keys[i];
            if (code != 0) {
                if ((code >> 3) < NKRO_REPORT_BITS) {
                    nkro_report_user.bits[code >> 3] |= 1 << (code & 7);
                }
            }
        }
        send_raw_hid_report();
    }
}

void send_nkro_user(report_nkro_t* report) {
    if (!suppress_real_reports) {
        (*send_nkro_real)(report);
    }
    if (send_raw_hid_reports) {
        memcpy(&nkro_report_user, report, sizeof(report_nkro_t));
        send_raw_hid_report();
    }
}

void send_extra_user(report_extra_t* report) {
    if (!suppress_real_reports) {
        (*send_extra_real)(report);
    }
    if (send_raw_hid_reports) {
        if (report->usage == 0) {
            for (int code = KC_SYSTEM_POWER; code<=KC_SYSTEM_WAKE; code++) {
                if ((code >> 3) < NKRO_REPORT_BITS) {
                    nkro_report_user.bits[code >> 3] &= ~(1 << (code & 7));
                }
            }
            for (int code = KC_AUDIO_MUTE; code<=KC_LAUNCHPAD; code++) {
                if ((code >> 3) < NKRO_REPORT_BITS) {
                    nkro_report_user.bits[code >> 3] &= ~(1 << (code & 7));
                }
            }
        } else {
            int code = USAGE2KEYCODE(report->usage);
            if ((code >> 3) < NKRO_REPORT_BITS) {
                nkro_report_user.bits[code >> 3] |= 1 << (code & 7);
            }
        }
        send_raw_hid_report();
    }
}

static void send_raw_hid_report() {
    static_assert(sizeof(report_nkro_t) == RAW_EPSIZE, "report_nkro_t does not match raw HID report size");
    raw_hid_send((uint8_t*)&nkro_report_user, RAW_EPSIZE);

    // static_assert(REPORT_ID_NKRO == 6, "REPORT_ID_NKRO unexpected value");
    // static_assert(sizeof(matrix) <= RAW_EPSIZE-4, "Matrix too big for raw HID report size");
    // static_assert(MATRIX_ROWS <= 0xFF, "Too many matrix rows for raw HID report");
    // static_assert(MATRIX_COLS <= 0xFF, "Too many matrix cols for raw HID report");
    // raw_hid_report[0] = 1; // non-standard report ID, we only need to stay away from REPORT_ID_NKRO
    // raw_hid_report[1] = 0; //active_layer;
    // raw_hid_report[2] = MATRIX_ROWS;
    // raw_hid_report[3] = MATRIX_COLS;
    // memcpy(raw_hid_report+4, matrix, sizeof(matrix));
    // raw_hid_send(raw_hid_report, RAW_EPSIZE);
}

void raw_hid_receive(uint8_t *data, uint8_t length) {
    if (length == 0) {
        return;
    }
    if (data[0] == 0xBE) {
        send_raw_hid_reports = true;
        suppress_real_reports = true;
    } else if (data[0] == 0xBF) {
        send_raw_hid_reports = false;
        suppress_real_reports = false;
    } else if (data[0] == 0xC0) {
        last_heartbeat_time = timer_read32();
        // host_connection = true;
        // shared_keys_send();
    } else if (data[0] == 0xC1) {
        if (length >= 5) {
            process_shared_keys_remote(data[1] | (data[2] << 8) | (data[3] << 16) | (data[4] << 24));
        }
    }
}

void housekeeping_task_user() {
    if (timer_elapsed32(last_heartbeat_time) > HEARTBEAT_TIMEOUT_MS) {
        send_raw_hid_reports = false;
        suppress_real_reports = false;
        // host_connection = false;
        process_shared_keys_remote(0);
    }
}

static void process_shared_keys_remote(uint32_t keys) {
    if (keys != shared_keys_remote) {
        uint8_t i;
        uint32_t b;
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

static void shared_keys_send(void) {
    memset(raw_hid_report, 0, sizeof(raw_hid_report));
    raw_hid_report[0] = 0xC0;
    raw_hid_report[1] = shared_keys_local & 0xFF;
    raw_hid_report[2] = (shared_keys_local >> 8) & 0xFF;
    raw_hid_report[3] = (shared_keys_local >> 16) & 0xFF;
    raw_hid_report[4] = (shared_keys_local >> 24) & 0xFF;
    raw_hid_send(raw_hid_report, RAW_EPSIZE);
}

static void shared_key_event(uint8_t key, bool pressed) {
    action_t action = {};
    switch (key) {
        default:
            return;
        case _SK_NUM:
            action.code = ACTION_LAYER_MOMENTARY(_NUM_LAYER);
            break;
        case _SK_FUN:
            action.code = ACTION_LAYER_MOMENTARY(_FUN_LAYER);
            break;
        case _SK_SYM:
            action.code = ACTION_LAYER_MOMENTARY(_SYM_L_LAYER);
            break;
        case _SK_NAV:
            action.code = ACTION_LAYER_MOMENTARY(_NAV_L_LAYER);
            break;
        case _SK_NVIM:
            nvim_active = pressed;
            return;
        case _SK_NVIM_NORMAL:
            if (pressed) {
                set_single_default_layer(_QWERTY_NVIM);
            } else {
                set_single_default_layer(_MAGIC_STURDY);
            }
            return;

    }
    keyrecord_t record = { .event = MAKE_KEYEVENT(0, 0, pressed) };
    process_action(&record, action);
}

uint8_t USAGE2KEYCODE(uint16_t usage) {
    switch (usage) {
        case SYSTEM_POWER_DOWN:
            return KC_SYSTEM_POWER;
        case SYSTEM_SLEEP:
            return KC_SYSTEM_SLEEP;
        case SYSTEM_WAKE_UP:
            return KC_SYSTEM_WAKE;
        case AUDIO_MUTE:
            return KC_AUDIO_MUTE;
        case AUDIO_VOL_UP:
            return KC_AUDIO_VOL_UP;
        case AUDIO_VOL_DOWN:
            return KC_AUDIO_VOL_DOWN;
        case TRANSPORT_NEXT_TRACK:
            return KC_MEDIA_NEXT_TRACK;
        case TRANSPORT_PREV_TRACK:
            return KC_MEDIA_PREV_TRACK;
        case TRANSPORT_FAST_FORWARD:
            return KC_MEDIA_FAST_FORWARD;
        case TRANSPORT_REWIND:
            return KC_MEDIA_REWIND;
        case TRANSPORT_STOP:
            return KC_MEDIA_STOP;
        case TRANSPORT_STOP_EJECT:
            return KC_MEDIA_EJECT;
        case TRANSPORT_PLAY_PAUSE:
            return KC_MEDIA_PLAY_PAUSE;
        case AL_CC_CONFIG:
            return KC_MEDIA_SELECT;
        case AL_EMAIL:
            return KC_MAIL;
        case AL_CALCULATOR:
            return KC_CALCULATOR;
        case AL_LOCAL_BROWSER:
            return KC_MY_COMPUTER;
        case AL_CONTROL_PANEL:
            return KC_CONTROL_PANEL;
        case AL_ASSISTANT:
            return KC_ASSISTANT;
        case AC_SEARCH:
            return KC_WWW_SEARCH;
        case AC_HOME:
            return KC_WWW_HOME;
        case AC_BACK:
            return KC_WWW_BACK;
        case AC_FORWARD:
            return KC_WWW_FORWARD;
        case AC_STOP:
            return KC_WWW_STOP;
        case AC_REFRESH:
            return KC_WWW_REFRESH;
        case BRIGHTNESS_UP:
            return KC_BRIGHTNESS_UP;
        case BRIGHTNESS_DOWN:
            return KC_BRIGHTNESS_DOWN;
        case AC_BOOKMARKS:
            return KC_WWW_FAVORITES;
        case AC_DESKTOP_SHOW_ALL_WINDOWS:
            return KC_MISSION_CONTROL;
        case AC_SOFT_KEY_LEFT:
            return KC_LAUNCHPAD;
        default:
            return 0;
    }
}

enum {
    TD_BOOT,
    TD_CAPS,
};

void tap_dance_boot_finished(tap_dance_state_t *state, void *user_data) {
    if (state->count == 3) {
        bootloader_jump();
    }
}

void tap_dance_caps_finished(tap_dance_state_t *state, void *user_data) {
    if (state->count == 1) {
        caps_word_toggle();
    } else if (state->count == 2) {
        tap_code(KC_CAPS);
    } else if (state->count == 3) {
        tap_code(KC_PSCR);
    }
}


tap_dance_action_t tap_dance_actions[] = {
    [TD_BOOT] = ACTION_TAP_DANCE_FN(tap_dance_boot_finished),
    [TD_CAPS] = ACTION_TAP_DANCE_FN(tap_dance_caps_finished),
};


uint16_t get_tapping_term(uint16_t keycode, keyrecord_t *record) {
    switch (keycode) {
        case TD(TD_BOOT):
        case TD(TD_CAPS):
            return 250;
        default:
            return TAPPING_TERM;
    }
}

bool is_flow_tap_key(uint16_t keycode) {
    if ((get_mods() & (MOD_MASK_CG | MOD_BIT_LALT)) != 0) {
        return false; // Disable Flow Tap on hotkeys.
    }
    switch (get_tap_keycode(keycode)) {
        case KC_A ... KC_Z:
        case KC_DOT:
        case KC_COMM:
        case KC_SCLN:
        case KC_SLSH:
            return true;
    }
    return false;
}

uint16_t get_flow_tap_term(uint16_t keycode, keyrecord_t* record, uint16_t prev_keycode) {
    if (nvim_active || !is_flow_tap_key(keycode) || !is_flow_tap_key(prev_keycode)) {
        return 0;
    } else {
        return FLOW_TAP_TERM;
    }
}

uint16_t get_quick_tap_term(uint16_t keycode, keyrecord_t *record) {
    if (nvim_active) {
        return 0;
    } else {
        return QUICK_TAP_TERM;
    }
}

const key_override_t comma_key_override = ko_make_basic(MOD_MASK_SHIFT, KC_COMM, KC_QUES);
const key_override_t dot_key_override = ko_make_basic(MOD_MASK_SHIFT, KC_DOT, KC_EXLM);
const key_override_t unds_key_override = ko_make_basic(MOD_MASK_SHIFT, KC_UNDS, KC_MINS);
// const key_override_t coln_key_override = ko_make_basic(MOD_MASK_SHIFT, KC_COLN, KC_SCLN);

const key_override_t *key_overrides[] = {
	&comma_key_override,
	&dot_key_override,
	&unds_key_override,
	// &coln_key_override,
};


/*    0   1   2   3   4   5           0   1   2   3   4   5
*  ┌───┬───┬───┬───┬───┬───┐       ┌───┬───┬───┬───┬───┬───┐
* 0│   │   │   │   │   │   │      4│   │   │   │   │   │   │
*  ├───┼───┼───┼───┼───┼───┤       ├───┼───┼───┼───┼───┼───┤
* 1│   │   │   │   │   │   │      5│   │   │   │   │   │   │
*  ├───┼───┼───┼───┼───┼───┤       ├───┼───┼───┼───┼───┼───┤
* 2│   │   │   │   │   │   │      6│   │   │   │   │   │   │
*  └───┴───┴───┴───┴───┴───┘       └───┴───┴───┴───┴───┴───┘
*                  0                       2
*                ┌───┐ 1               1 ┌───┐
*               3│   ├───┐ 2       0 ┌───┤   │
*                └───┤   ├───┐   ┌───┤   ├───┘
*                    └───┤   │  7│   ├───┘
*                        └───┘   └───┘
*/

// To execute a fake key event at a given matrix position:
// action_exec(MAKE_KEYEVENT(row, col, key_pressed));
// key_pressed is bool
// rows and cols of matrix are marked above
// should be able to make the matrix bigger to define virtual keys if needed
//
// can also construct an action using one of the macros you can see in
// action_for_keycode, construct an event like above (think matrix position
// could be ignored for this), and then call process_action(record, action)
//
const uint16_t PROGMEM keymaps[][MATRIX_ROWS][MATRIX_COLS] = {
    [_MAGIC_STURDY] = LAYOUT_split_3x6_3(
        SK_DS,        KC_V,       KC_M,       KC_L,         KC_C,       KC_P,         KC_B,        MAGIC,       KC_U,       KC_O,       KC_Q,  TD(TD_CAPS),
        KC_ENT,  _ALT(KC_S), _CTL(KC_T), _SFT(KC_R), _SYM_R(KC_D), _NAV(KC_Y),   _NUM(KC_F), _SYM_L(KC_N), _SFT(KC_E), _CTL(KC_A), _ALT(KC_I),    KC_BSPC,
        KC_TAB,  _GUI(KC_X),      KC_K,       KC_J,         KC_G,       KC_W,         KC_Z,    _FUN(KC_H),    KC_COMM,     KC_DOT, _GUI(KC_QUOT), QK_LEAD,
                                                        KC_DEL, KC_SPC, KC_ESC,      QK_REP, KC_UNDS, KC_MINS
    ),
    [_QWERTY_NVIM] = LAYOUT_split_3x6_3(
        SK_DS,        KC_Q,       KC_W,       KC_E,         KC_R,       KC_T,         KC_Y,         KC_U,       KC_I,       KC_O,       KC_P,  TD(TD_CAPS),
        KC_ENT,  _ALT(KC_A), _CTL(KC_S), _SFT(KC_D), _SYM_R(KC_F), _NAV(KC_G),   _NUM(KC_H), _SYM_L(KC_J), _SFT(KC_K), _CTL(KC_L), _ALT(KC_SCLN), KC_BSPC,
        KC_TAB,  _GUI(KC_Z),      KC_X,       KC_C,         KC_V,       KC_B,         KC_N,    _FUN(KC_M),    KC_COMM,     KC_DOT, _GUI(KC_SLSH), QK_LEAD,
                                                       KC_LBRC, KC_SPC, KC_ESC,      TSL_NUM, OSL_NUM, KC_RBRC
    ),
    [_NAV_LAYER] = LAYOUT_split_3x6_3(
        KC_TRNS, KC_NO,   KC_NO,   KC_NO,   KC_NO, KC_NO,   KC_PGUP, KC_HOME, KC_UP,   KC_END,  KC_NO, KC_TRNS,
        KC_TRNS, KC_LALT, KC_LCTL, KC_LSFT, KC_NO, KC_NO,   KC_PGDN, KC_LEFT, KC_DOWN, KC_RGHT, KC_NO, KC_TRNS,
        KC_TRNS, KC_LGUI, KC_NO,   KC_NO,   KC_NO, KC_NO,   KC_NO,   _BAK,    KC_NO,   _FWD,    KC_NO, KC_TRNS,
                               KC_TRNS, KC_TRNS, KC_TRNS,   KC_TRNS, KC_TRNS, KC_TRNS
    ),
    [_NAV_L_LAYER] = LAYOUT_split_3x6_3(
           _NEW, _PSTE, _ALL,    KC_UP,   _COPY,   KC_PSCR,    KC_NO, KC_NO, KC_NO, KC_NO, KC_NO, KC_NO,
        KC_LGUI, _UNDO, KC_LEFT, KC_DOWN, KC_RGHT, _REDO,      KC_NO, KC_NO, KC_NO, KC_NO, KC_NO, KC_NO,
          _BTAB, _CUT,  _BAK,    _SAVE,   _FWD,    _REDO2,     KC_NO, KC_NO, KC_NO, KC_NO, KC_NO, KC_NO,
                                       _EXIT, _NTAB, _CLSE,    KC_NO, KC_NO, KC_NO
    ),
    [_SYM_L_LAYER] = LAYOUT_split_3x6_3(
        KC_TRNS, KC_GRV,  KC_LABK, KC_RABK, KC_MINS, KC_PIPE,   KC_NO, KC_NO, KC_NO,   KC_NO,   KC_NO,   KC_TRNS,
        KC_TRNS, KC_EXLM, KC_ASTR, KC_SLSH, KC_EQL,  KC_AMPR,   KC_NO, KC_NO, KC_LSFT, KC_LCTL, KC_LALT, KC_TRNS,
        KC_TRNS, KC_TILD, KC_PLUS, KC_LBRC, KC_RBRC, KC_PERC,   KC_NO, KC_NO, KC_NO,   KC_NO,   KC_LGUI, KC_TRNS,
                                   KC_TRNS, KC_TRNS, KC_TRNS,   KC_TRNS, KC_TRNS, KC_TRNS
    ),
    [_SYM_R_LAYER] = LAYOUT_split_3x6_3(
        KC_TRNS, KC_NO,   KC_NO,   KC_NO,   KC_NO, KC_NO,   KC_CIRC, KC_LCBR, KC_RCBR, KC_DLR,  KC_BSLS, KC_TRNS,
        KC_TRNS, KC_LALT, KC_LCTL, KC_LSFT, KC_NO, KC_NO,   KC_HASH, KC_LPRN, KC_RPRN, KC_SCLN, KC_DQUO, KC_TRNS,
        KC_TRNS, KC_LGUI, KC_NO,   KC_NO,   KC_NO, KC_NO,   KC_AT,   KC_COLN, KC_TRNS, KC_TRNS, KC_QUOT, KC_TRNS,
                               KC_TRNS, KC_TRNS, KC_TRNS,   KC_TRNS, KC_TRNS, KC_TRNS
    ),
    [_NUM_LAYER] = LAYOUT_split_3x6_3(
        TD(TD_BOOT), KC_PLUS, KC_9, KC_8, KC_7, KC_ASTR,   KC_NO, KC_NO, KC_NO,   KC_NO,   KC_NO,   KC_TRNS,
        _EXPL,       KC_DOT,  KC_3, KC_2, KC_1, KC_NO,     KC_NO, KC_NO, KC_LSFT, KC_LCTL, KC_LALT, KC_TRNS,
        _ATAB,       KC_MINS, KC_6, KC_5, KC_4, KC_SLSH,   KC_NO, KC_NO, KC_NO,   KC_NO,   KC_LGUI, KC_TRNS,
                                 KC_TRNS, KC_0, KC_TRNS,   KC_TRNS, KC_TRNS, KC_TRNS
    ),
    [_FUN_LAYER] = LAYOUT_split_3x6_3(
        KC_TRNS, KC_NO, KC_F9, KC_F8, KC_F7, KC_F10,   KC_NO, KC_NO, KC_NO,   KC_NO,   KC_NO,   KC_TRNS,
        KC_TRNS, KC_NO, KC_F3, KC_F2, KC_F1, KC_F11,   KC_NO, KC_NO, KC_LSFT, KC_LCTL, KC_LALT, KC_TRNS,
        _GTAB,   KC_NO, KC_F6, KC_F5, KC_F4, KC_F12,   KC_NO, KC_NO, KC_NO,   KC_NO,   KC_LGUI, KC_TRNS,
                          KC_TRNS, KC_TRNS, KC_TRNS,   KC_TRNS, KC_TRNS, KC_TRNS
    ),
    // Note that the logic for this layer explicitly relies on every base keycode that is NOT a digit
    // being an exact match for _QWERTY_NVIM (or else needs to be KC_NO), otherwise keys may not be
    // unregistered properly.
    [_NUM_NVIM_LAYER] = LAYOUT_split_3x6_3(
        KC_NO, KC_NO, KC_9, KC_8, KC_7, KC_NO,   KC_Y, KC_U, KC_I,    KC_O,   KC_P,    KC_NO,
        KC_NO, KC_NO, KC_3, KC_2, KC_1, KC_NO,   KC_H, KC_J, KC_K,    KC_L,   KC_SCLN, KC_NO,
        KC_NO, KC_NO, KC_6, KC_5, KC_4, KC_NO,   KC_N, KC_M, KC_COMM, KC_DOT, KC_SLSH, KC_NO,
                         KC_NO, KC_0, KC_TRNS,   TSL_NUM, OSL_NUM, KC_NO
    ),
};

void leader_end_user(void) {
    if (leader_sequence_two_keys(KC_W, KC_I)) {
        SEND_STRING("windexlight");
    }
}

bool caps_word_press_user(uint16_t keycode) {
    switch (keycode) {
        // Keycodes that continue Caps Word, with shift applied.
        case KC_A ... KC_Z:
            add_weak_mods(MOD_BIT(KC_LSFT));  // Apply shift to next key.
            return true;

        // Keycodes that continue Caps Word, without shifting.
        case KC_1 ... KC_0:
        case KC_BSPC:
        case KC_DEL:
        case KC_UNDS:
        // https://github.com/getreuer/qmk-keymap/blob/main/getreuer.c
        case M_THE:
        case M_ION:
        case M_MENT:
        case M_QUEN:
        case M_TMENT:
            return true;

        default:
            return false;  // Deactivate Caps Word.
    }
}


// https://github.com/getreuer/qmk-keymap/blob/main/getreuer.c
// The following describes the magic key functionality, where * represents the
// magic key and @ the repeat key. For example, tapping A and then the magic key
// types "ao". Most of this is coded in my `get_alt_repeat_key_keycode_user()`
// definition below.
//
// SFB removal and common n-grams:
//
//     A * -> AO     L * -> LK      S * -> SK
//     C * -> CY     M * -> MENT    T * -> TMENT
//     D * -> DY     O * -> OA      U * -> UE
//     E * -> EU     P * -> PY      Y * -> YP
//     G * -> GY     Q * -> QUEN    spc * -> THE
//     I * -> ION    R * -> RL
//
// When the magic key types a letter, following it with the repeat key produces
// "n". This is useful to type certain patterns without SFBs.
//
//     A * @ -> AON             (like "kaon")
//     D * @ -> DYN             (like "dynamic")
//     E * @ -> EUN             (like "reunite")
//     O * @ -> OAN             (like "loan")
//
// Other patterns:
//
//     spc * @ -> THEN
//     I * @ -> IONS            (like "nations")
//     M * @ -> MENTS           (like "moments")
//     Q * @ -> QUENC           (like "frequency")
//     T * @ -> TMENTS          (like "adjustments")
//     = *   -> ===             (JS code)
//     ! *   -> !==             (JS code)
//     " *   -> """<cursor>"""  (Python code)
//     ` *   -> ```<cursor>```  (Markdown code)
//     # *   -> #include        (C code)
//     & *   -> &nbsp;          (HTML code)
//     . *   -> ../             (shell)
//     . * @ -> ../../
uint16_t get_alt_repeat_key_keycode_user(uint16_t keycode, uint8_t mods) {
    keycode = get_tap_keycode(keycode);

    if (mods == MOD_BIT_LALT) {
        switch (keycode) {
        case KC_U: return A(KC_O);
        case KC_O: return A(KC_U);
        case KC_N: return A(KC_I);
        case KC_I: return A(KC_N);
        }
    } else if ((mods & ~MOD_MASK_SHIFT) == 0) {
        // This is where most of the "magic" for the MAGIC key is implemented.
        switch (keycode) {
        case KC_SPC:  // spc -> THE
        case KC_ENT:
        case KC_TAB:
            if ((mods & MOD_MASK_SHIFT) == 0) {
                return M_THE;
            } else {
                return M_CAP_THE;
            }

        // For navigating next/previous search results in Vim:
        // N -> Shift + N, Shift + N -> N.
        // case KC_N:
        //     if ((mods & MOD_MASK_SHIFT) == 0) {
        //         return S(KC_N);
        //     }
        //     return KC_N;

        // Fix SFBs and awkward strokes.
        case KC_A: return KC_O;         // A -> O
        case KC_O: return KC_A;         // O -> A
        case KC_E: return KC_U;         // E -> U
        case KC_U: return KC_E;         // U -> E
        case KC_I:
            if ((mods & MOD_MASK_SHIFT) == 0) {
                return M_ION;  // I -> ON
            } else {
                return KC_QUOT;  // Shift I -> '
            }
        case KC_M: return M_MENT;       // M -> ENT
        case KC_Q: return M_QUEN;       // Q -> UEN
        case KC_T: return M_TMENT;      // T -> TMENT

        case KC_C: return KC_Y;         // C -> Y
        case KC_D: return KC_Y;         // D -> Y
        case KC_G: return KC_Y;         // G -> Y
        case KC_P: return KC_Y;         // P -> Y
        case KC_Y: return KC_P;         // Y -> P

        case KC_L: return KC_K;         // L -> K
        case KC_S: return KC_K;         // S -> K

        case KC_R: return KC_L;         // R -> L
        case KC_DOT:
            if ((mods & MOD_MASK_SHIFT) == 0) {
                return M_UPDIR;  // . -> ./
            }
            return M_NOOP;
        case KC_HASH: return M_INCLUDE;  // # -> include
        case KC_AMPR: return M_NBSP;     // & -> nbsp;
        case KC_EQL: return M_EQEQ;      // = -> ==
        case KC_RBRC: return KC_SCLN;    // ] -> ;
        //   case KC_AT: return USRNAME;      // @ -> <username>

        // Additions from https://github.com/Ikcelaks/keyboard_layouts/blob/main/magic_sturdy/magic_sturdy.md
        case KC_B:    return M_BEFORE;
        case KC_K:    return KC_S;
        case KC_J:    return M_JUST;
        case KC_N:    return M_NION;
        case KC_V:    return M_VER;
        case KC_W:    return M_WHICH;
        case KC_X:    return M_XES;
        case KC_COMM: return M_BUT;

        case KC_QUOT:
            if ((mods & MOD_MASK_SHIFT) != 0) {
                return M_DOCSTR;  // " -> ""<cursor>"""
            }
            return M_VE;
        case M_LL:
            return M_VE;
        case KC_GRV:  // ` -> ``<cursor>``` (for Markdown code)
            return M_MKGRVS;
        // case KC_LABK:  // < -> - (for Haskell)
        //     return KC_MINS;
        // case KC_SLSH:
        //     return KC_SLSH;  // / -> / (easier reach than Repeat)

        case KC_PLUS:
        case KC_MINS:
        case KC_ASTR:
        case KC_PERC:
        case KC_PIPE:
        case KC_CIRC:
        case KC_TILD:
        case KC_EXLM:
        case KC_DLR:
        case KC_LABK:
        case KC_RABK:
        case KC_LPRN:
        case KC_RPRN:
        case KC_UNDS:
        case KC_COLN:
        case KC_SLSH:
            return KC_EQL;

        case KC_F:
        case KC_Z:
        case KC_H:
        case KC_SCLN:
        case KC_1 ... KC_0:
            return M_NOOP;
        }
    }

    //   switch (keycode) {
    //     case MS_WHLU: return MS_WHLD;
    //     case MS_WHLD: return MS_WHLU;
    //     case SELWBAK: return SELWORD;
    //     case SELWORD: return SELWBAK;
    //   }
    return KC_TRNS;
}

bool get_speculative_hold(uint16_t keycode, keyrecord_t* record) {
    switch (keycode) { // These keys may be speculatively held.
        case _SFT(KC_D):
        case _CTL(KC_S):
        case _ALT(KC_A):
        case _GUI(KC_Z):
        case _SFT(KC_K):
        case _CTL(KC_L):
        case _ALT(KC_COLN):
        case _GUI(KC_SLSH):
        case _SFT(KC_R):
        case _CTL(KC_T):
        case _ALT(KC_S):
        case _GUI(KC_X):
        case _SFT(KC_E):
        case _CTL(KC_A):
        case _ALT(KC_I):
        case _GUI(KC_QUOT):
            return true;
    }
    return false; // Disable otherwise.
}

