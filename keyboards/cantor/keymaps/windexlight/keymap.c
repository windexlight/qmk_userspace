// Copyright 2022 Diego Palacios (@diepala)
// SPDX-License-Identifier: GPL-2.0

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

void clear_osm_mods(void);
void send_raw_hid_report(void);
void send_keyboard_user(report_keyboard_t* report);
void send_nkro_user(report_nkro_t* report);
void send_extra_user(report_extra_t* report);
uint8_t USAGE2KEYCODE(uint16_t usage);


enum layers {
  _MAGIC_STURDY,
  _NUM,
  _SYM,
  _FNC,
  _EXT,
  _MSE
};

#define _BAK LALT(KC_LEFT)
#define _FND LCTL(KC_F)
#define _FWD LALT(KC_RGHT)

#define _UNDO LCTL(KC_Z)
#define _CUT LCTL(KC_X)
#define _COPY LCTL(KC_C)
#define _WIN KC_LGUI
#define _PSTE LCTL(KC_V)

#define EX_NUM_MODS 8
#define EX_NUM_OSM_KEYS 32
#define EX_MOD_HOLD_OSM_CLEAR_MS 500
#define EX_OSM_TIMEOUT_MS 5000
#define HEARTBEAT_TIMEOUT_MS 1500

enum custom_keycodes {
    EX_LAYER = SAFE_RANGE,
    EX_LAYER_MAX = EX_LAYER + 10,
    EX_ONE_SHOT_MOD,
    EX_ONE_SHOT_MOD_MAX = EX_ONE_SHOT_MOD + EX_NUM_MODS - 1,
    CLR_OSM,
    // https://github.com/getreuer/qmk-keymap/blob/main/getreuer.c
    // Macros invoked through the Magic key.
    UPDIR,
    M_DOCSTR,
    M_EQEQ,
    M_INCLUDE,
    M_ION,
    M_MENT,
    M_MKGRVS,
    M_QUEN,
    M_THE,
    M_TMENT,
    M_UPDIR,
    M_NBSP,
    M_NOOP,
};

enum keycode_aliases {
    // https://github.com/getreuer/qmk-keymap/blob/main/getreuer.c
    // The "magic" key is the Alternate Repeat Key.
    MAGIC = QK_AREP,
    // Short aliases for home row mods and other tap-hold keys.
    // HRM_S = LALT_T(KC_S),
    // HRM_T = LT(SYM, KC_T),
    HRM_R = LSFT_T(KC_R),
    // HRM_D = LT(NAV, KC_D),
    // HRM_G = LCTL_T(KC_G),
    // HRM_X = LGUI_T(KC_X),

    // HRM_N = LT(NUM, KC_N),
    HRM_E = RSFT_T(KC_E),
    // HRM_A = LT(SYM, KC_A),
    // HRM_I = LALT_T(KC_I),
    // HRM_H = RCTL_T(KC_H),
    // HRM_DOT = LT(WIN, KC_DOT),
    // HRM_QUO = RGUI_T(KC_QUOT),

    // EXT_COL = LT(EXT, KC_SCLN),
    // NAV_SLS = LSFT_T(KC_SLSH),
    // NAV_EQL = LT(0, KC_EQL),
};

#define EX_MO(n) (EX_LAYER + (n))
#define EX_OSM(n) (EX_ONE_SHOT_MOD + ((n) & 0x07)) // Usage: EX_OSM(KC_LSFT)
#define EX_MOD(n) (KC_LEFT_CTRL + (n))

#define _LALT EX_OSM(KC_LALT)
#define _LGUI EX_OSM(KC_LGUI)
#define _LSFT EX_OSM(KC_LSFT)
#define _LCTL EX_OSM(KC_LCTL)
#define _RALT EX_OSM(KC_RALT)

static uint8_t layer_stack[6];
static uint8_t stack_size = 0;
static uint8_t ex_osm_bits = 0;
static uint8_t ex_mod_bits = 0;
static uint32_t last_mod_time[EX_NUM_MODS];
static uint32_t last_heartbeat_time = 0;
static uint16_t ex_osm_keys[EX_NUM_OSM_KEYS];
static uint8_t ex_osm_key_count = 0;
static uint8_t raw_hid_report[RAW_EPSIZE];
static uint8_t active_layer = 0;
static bool suppress_real_reports = false;
static bool send_raw_hid_reports = false;

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
#define MAGIC_STRING(str, repeat_keycode) \
    magic_send_string_P(PSTR(str), (repeat_keycode))
static void magic_send_string_P(const char* str, uint16_t repeat_keycode) {
    uint8_t saved_mods = 0;
    // If Caps Word is on, save the mods and hold Shift.
    if (is_caps_word_on()) {
        saved_mods = get_mods();
        register_mods(MOD_BIT_LSHIFT);
    }

    send_string_P(str);  // Send the string.
    set_last_keycode(repeat_keycode);

    // If Caps Word is on, restore the mods.
    if (is_caps_word_on()) {
        set_mods(saved_mods);
    }
}


bool process_record_user(uint16_t keycode, keyrecord_t *record) {
#ifdef CONSOLE_ENABLE
    uprintf("KL: kc: 0x%04X, col: %2u, row: %2u, pressed: %u, time: %5u, int: %u, count: %u\n", keycode, record->event.key.col, record->event.key.row, record->event.pressed, record->event.time, record->tap.interrupted, record->tap.count);
#endif
    bool ret = true;
    if (keycode >= EX_LAYER && keycode <= EX_LAYER_MAX) {
        uint8_t layer = keycode - EX_LAYER;
        if (record->event.pressed) {
            if (layer == _EXT) {
                if (!host_keyboard_led_state().scroll_lock) {
                    tap_code(KC_SCRL);
                }
            }
            if (stack_size < sizeof(layer_stack)) {
                layer_stack[stack_size++] = layer;
            }
            active_layer = layer;
        } else {
            if (layer == _EXT) {
                if (host_keyboard_led_state().scroll_lock) {
                    tap_code(KC_SCRL);
                }
            }
            for (uint8_t i = 0; i < stack_size; i++) {
                if (layer_stack[i] == layer) {
                    for (uint8_t j = i; j < stack_size - 1; j++) {
                        layer_stack[j] = layer_stack[j+1];
                    }
                    stack_size--;
                    break;
                }
            }
            if (stack_size > 0) {
                active_layer = layer_stack[stack_size-1];
            } else {
                active_layer = 0;
            }
        }
        layer_move(active_layer);
        ret = false;
    } else if (keycode >= EX_ONE_SHOT_MOD && keycode <= EX_ONE_SHOT_MOD_MAX) {
        uint8_t mod = keycode - EX_ONE_SHOT_MOD;
        uint8_t code = EX_MOD(mod);
        uint8_t bit = MOD_BIT(code);
        if (record->event.pressed) {
            register_code(code);
            ex_mod_bits |= bit;
            ex_osm_bits |= bit;
            last_mod_time[mod] = timer_read32();
        } else {
            if ((ex_osm_bits & bit) == 0) {
                unregister_code(code);
            }
            ex_mod_bits &= ~bit;
        }
        ret = false;
    } else if (keycode == CLR_OSM) {
        if (record->event.pressed) {
            ex_osm_key_count = 0;
            clear_osm_mods();
        }
        ret = false;
    } else {
        // https://github.com/getreuer/qmk-keymap/blob/main/getreuer.c
        // If alt repeating key A, E, I, O, U, Y with no mods other than Shift, set
        // the last key to KC_N. Above, alternate repeat of KC_N is defined to be
        // again KC_N. This way, either tapping alt repeat and then repeat (or
        // equivalently double tapping alt repeat) is useful to type certain patterns
        // without SFBs:
        //
        //   D <altrep> <rep> -> DYN (as in "dynamic")
        //   O <altrep> <rep> -> OAN (as in "loan")
        if (get_repeat_key_count() < 0 &&
            ((get_mods() | get_weak_mods() | get_oneshot_mods()) & ~MOD_MASK_SHIFT) == 0 &&
            (keycode == KC_A || keycode == KC_E || keycode == KC_I ||
            keycode == KC_O || keycode == KC_U || keycode == KC_Y)) {
            set_last_keycode(KC_N);
            set_last_mods(0);
        }
        bool magic = true;
        if (!record->event.pressed) {
            magic = false;
        } else {
            switch (keycode) {
            // https://github.com/getreuer/qmk-keymap/blob/main/getreuer.c
            // Macros invoked through the MAGIC key.
            case M_THE:     MAGIC_STRING(/* */"the", KC_N); break;
            case M_ION:     MAGIC_STRING(/*i*/"on", KC_S); break;
            case M_MENT:    MAGIC_STRING(/*m*/"ent", KC_S); break;
            case M_QUEN:    MAGIC_STRING(/*q*/"uen", KC_C); break;
            case M_TMENT:   MAGIC_STRING(/*t*/"ment", KC_S); break;
            case M_UPDIR:   MAGIC_STRING(/*.*/"./", UPDIR); break;
            case M_INCLUDE: SEND_STRING_DELAY(/*#*/"include ", TAP_CODE_DELAY); break;
            case M_EQEQ:    SEND_STRING_DELAY(/*=*/"==", TAP_CODE_DELAY); break;
            case M_NBSP:    SEND_STRING_DELAY(/*&*/"nbsp;", TAP_CODE_DELAY); break;

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
            case KC_SPC:
                // https://github.com/getreuer/qmk-keymap/blob/main/getreuer.c
                // When the Repeat key follows Space, it behaves as one-shot shift.
                // TODO: This isn't working very well. Think it probably conflicts
                // with my alternate one-shot mods, and also it's still sending a space,
                // need to set ret = false, I think.
                if (get_repeat_key_count() > 0) {
                    if (record->event.pressed) {
                        add_oneshot_mods(MOD_LSFT);
                        register_mods(MOD_LSFT);
                    } else {
                        unregister_mods(MOD_LSFT);
                    }
                } else {
                    magic = false;
                }
                break;
            default:
                magic = false;
                break;
            }
        }
        if (!magic) {
            if (ex_osm_bits > 0) {
                if (record->event.pressed) {
                    if (ex_osm_key_count < sizeof(ex_osm_keys)) {
                        ex_osm_keys[ex_osm_key_count++] = keycode;
                    }
                } else if (ex_osm_key_count > 0) {
                    for (uint8_t i = 0; i < ex_osm_key_count; i++) {
                        if (ex_osm_keys[i] == keycode) {
                            for (uint8_t j = i; j < ex_osm_key_count - 1; j++) {
                                ex_osm_keys[j] = ex_osm_keys[j+1];
                            }
                            ex_osm_key_count--;
                            break;
                        }
                    }
                    if (ex_osm_key_count == 0) {
                        clear_osm_mods();
                    }
                }
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

void send_raw_hid_report() {
    static_assert(sizeof(report_nkro_t) == RAW_EPSIZE, "report_nkro_t does not match raw HID report size");
    raw_hid_send((uint8_t*)&nkro_report_user, RAW_EPSIZE);

    static_assert(REPORT_ID_NKRO == 6, "REPORT_ID_NKRO unexpected value");
    static_assert(sizeof(matrix) <= RAW_EPSIZE-4, "Matrix too big for raw HID report size");
    static_assert(MATRIX_ROWS <= 0xFF, "Too many matrix rows for raw HID report");
    static_assert(MATRIX_COLS <= 0xFF, "Too many matrix cols for raw HID report");
    raw_hid_report[0] = 1; // non-standard report ID, we only need to stay away from REPORT_ID_NKRO
    raw_hid_report[1] = active_layer;
    raw_hid_report[2] = MATRIX_ROWS;
    raw_hid_report[3] = MATRIX_COLS;
    memcpy(raw_hid_report+4, matrix, sizeof(matrix));
    raw_hid_send(raw_hid_report, RAW_EPSIZE);
}

void raw_hid_receive(uint8_t *data, uint8_t length) {
    if(data[0] == 0xBE) {
        last_heartbeat_time = timer_read32();
        send_raw_hid_reports = true;
        suppress_real_reports = true;
        memset(raw_hid_report, 0, sizeof(raw_hid_report));
        raw_hid_report[1] = 0xEF;
        raw_hid_send(raw_hid_report, RAW_EPSIZE);
    } else if (data[0] == 0xBF) {
        send_raw_hid_reports = false;
        suppress_real_reports = false;
    }
}

void clear_osm_mods() {
    uint8_t bit;
    for (uint8_t mod = 0; mod < EX_NUM_MODS; mod++) {
        bit = MOD_BIT(mod);
        if ((ex_osm_bits & bit) > 0) {
            if ((ex_mod_bits & bit) == 0) {
                unregister_code(EX_MOD(mod));
            }
        }
    }
    ex_osm_bits = 0;
}

void housekeeping_task_user() {
    uint8_t bit;
    if (ex_osm_bits > 0 && ex_osm_key_count == 0) {
        for (uint8_t mod = 0; mod < EX_NUM_MODS; mod++) {
            bit = MOD_BIT(mod);
            if ((ex_osm_bits & bit) > 0) {
                bool holding = (ex_mod_bits & bit) > 0;
                uint32_t timeout = holding ? EX_MOD_HOLD_OSM_CLEAR_MS : EX_OSM_TIMEOUT_MS;
                if (timer_elapsed32(last_mod_time[mod]) > timeout) {
                    ex_osm_bits &= ~bit;
                    if (!holding) {
                        unregister_code(EX_MOD(mod));
                    }
                }
            }
        }
    }
    if (timer_elapsed32(last_heartbeat_time) > HEARTBEAT_TIMEOUT_MS) {
        send_raw_hid_reports = false;
        suppress_real_reports = false;
    }
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

// Ideas:
// Magic Sturdy: https://github.com/getreuer/qmk-keymap/tree/main?tab=readme-ov-file
// Except, merge current Seniply layers (for the most part)
// One problem is where to put the repeat key (which seems like a decent idea).
// After reading about Tap Flow and Speculative Hold, I have to wonder if the combination
//  of those two might not make it feasible to try putting shift as mod-taps on the middle
//  fingers on both sides (i.e., how shift is placed in the Magic Strudy description above).
//  This could both free up the current thumb shift key for use as a repeat key, but also might
//  be a comfortable solution to my recent dissatisfaction with the thumb shift placement. It's
//  also attractive in that it keeps shift on the same fingers that they happen to be placed in
//  Seniply (at least on the left).
// Some or all of the placements of keys in the outer columns from Magic Sturdy above may be
// nice to keep also, and I don't think wholly conflict with the Seniply layers.
// See also (maybe not needed here but noting so as not to forget its existence): "Neutralize flashing modifiers"

const uint16_t PROGMEM keymaps[][MATRIX_ROWS][MATRIX_COLS] = {
    [_MAGIC_STURDY] = LAYOUT_split_3x6_3(
        KC_TAB,  KC_V,  KC_M,  KC_L,  KC_C,  KC_P,      KC_B,  MAGIC, KC_U,    KC_O,   KC_Q,    KC_SLSH,
        KC_BSPC, KC_S,  KC_T,  HRM_R, KC_D,  KC_Y,      KC_F,  KC_N,  HRM_E,   KC_A,   KC_I,    KC_DEL,
        KC_SCLN, KC_X,  KC_K,  KC_J,  KC_G,  KC_W,      KC_Z,  KC_H,  KC_COMM, KC_DOT, KC_QUOT, KC_ENT,
                  EX_MO(_NUM), EX_MO(_EXT), QK_REP,      EX_MO(_SYM), KC_SPC, EX_MO(_FNC)
    ),
    [_EXT] = LAYOUT_split_3x6_3(
        KC_TRNS, KC_ESC, _BAK,  _FND,  _FWD,  KC_INS,   KC_PGUP, KC_HOME, KC_UP,   KC_END,  KC_CAPS, KC_TRNS,
        KC_TRNS, _LALT,  _LGUI, _LSFT, _LCTL, _RALT,    KC_PGDN, KC_LEFT, KC_DOWN, KC_RGHT, KC_DEL,  KC_TRNS,
        KC_TRNS, _UNDO,  _CUT,  _COPY, _WIN,  _PSTE,    KC_ENT,  KC_BSPC, KC_TAB,  KC_APP,  KC_PSCR, KC_TRNS,
                                      KC_TRNS, KC_TRNS, KC_TRNS,    KC_TRNS, KC_TRNS, KC_TRNS
    ),
    [_FNC] = LAYOUT_split_3x6_3(
        KC_TRNS, KC_MSTP, KC_MPRV, KC_MPLY,   KC_MNXT, KC_BRIU,      KC_F12, KC_F7, KC_F8, KC_F9, KC_SCRL, KC_TRNS,
        KC_TRNS, _LALT,   _LGUI,   _LSFT,     _LCTL,   KC_BRID,      KC_F11, KC_F4, KC_F5, KC_F6, KC_NO,   KC_TRNS,
        KC_TRNS, KC_MUTE, KC_VOLD, RCS(KC_C), KC_VOLU, RCS(KC_V),    KC_F10, KC_F1, KC_F2, KC_F3, KC_NO,   KC_TRNS,
                                       KC_TRNS, KC_TRNS, KC_TRNS,    KC_TRNS, KC_TRNS, KC_TRNS
    ),
    [_SYM] = LAYOUT_split_3x6_3(
        KC_TRNS, KC_EXLM, KC_AT, KC_HASH, KC_DLR,  KC_PERC,      KC_EQL,  KC_GRV,  KC_COLN, KC_SCLN, KC_PLUS, KC_TRNS,
        KC_TRNS, _LALT,   _LGUI, _LSFT,   _LCTL,   KC_CIRC,      KC_ASTR, KC_LPRN, KC_LCBR, KC_LBRC, KC_MINS, KC_TRNS,
        KC_TRNS, CW_TOGG, KC_NO, KC_BSLS, KC_PIPE, KC_AMPR,      KC_TILD, KC_RPRN, KC_RCBR, KC_RBRC, KC_UNDS, KC_TRNS,
                                 KC_TRNS, KC_TRNS, KC_TRNS,      KC_TRNS, KC_TRNS, KC_TRNS
    ),
    [_NUM] = LAYOUT_split_3x6_3(
        KC_TRNS, KC_NO, KC_NO,  KC_NO,  KC_DOT,  KC_NUM,      KC_EQL,  KC_7, KC_8, KC_9, KC_0, KC_TRNS,
        KC_TRNS, _LALT, _LGUI,  _LSFT,  _LCTL,   _RALT,       KC_ASTR, KC_4, KC_5, KC_6, KC_DOT, KC_TRNS,
        KC_TRNS, KC_NO, KC_APP, KC_TAB, KC_BSPC, KC_ENT,      KC_TILD, KC_1, KC_2, KC_3, KC_SLSH, KC_TRNS,
                              KC_TRNS, KC_TRNS, KC_TRNS,      KC_TRNS, KC_TRNS, KC_TRNS
    ),
    [_MSE] = LAYOUT_split_3x6_3(
        KC_TRNS,  KC_ESC, _BAK,  _FND,  _FWD,  KC_NO,   MS_WHLU, MS_WHLL, MS_UP, MS_WHLR, KC_TRNS, KC_TRNS,
        TG(_MSE), _LALT,  _LGUI, _LSFT, _LCTL, _RALT,   MS_WHLD, MS_LEFT, MS_DOWN, MS_RGHT, KC_TRNS, KC_TRNS,
        KC_NO,    _UNDO,  _CUT,  _COPY, _WIN,  _PSTE,   KC_TRNS, KC_TRNS, KC_TRNS, KC_TRNS, KC_TRNS, KC_TRNS,
                                             KC_NO, KC_NO, KC_NO,   MS_BTN2, MS_BTN1, MS_BTN3
    )
};

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
            return M_THE;

        // For navigating next/previous search results in Vim:
        // N -> Shift + N, Shift + N -> N.
        case KC_N:
            if ((mods & MOD_MASK_SHIFT) == 0) {
            return S(KC_N);
            }
            return KC_N;

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

        case KC_COMM:
            if ((mods & MOD_MASK_SHIFT) != 0) {
            return KC_EQL;  // ! -> =
            }
            return M_NOOP;
        case KC_QUOT:
            if ((mods & MOD_MASK_SHIFT) != 0) {
            return M_DOCSTR;  // " -> ""<cursor>"""
            }
            return M_NOOP;
        case KC_GRV:  // ` -> ``<cursor>``` (for Markdown code)
            return M_MKGRVS;
        case KC_LABK:  // < -> - (for Haskell)
            return KC_MINS;
        case KC_SLSH:
            return KC_SLSH;  // / -> / (easier reach than Repeat)

        case KC_PLUS:
        case KC_MINS:
        case KC_ASTR:
        case KC_PERC:
        case KC_PIPE:
        case KC_CIRC:
        case KC_TILD:
        case KC_EXLM:
        case KC_DLR:
        case KC_RABK:
        case KC_LPRN:
        case KC_RPRN:
        case KC_UNDS:
        case KC_COLN:
            return KC_EQL;

        case KC_F:
        case KC_V:
        case KC_X:
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
