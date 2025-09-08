// Copyright 2022 Diego Palacios (@diepala)
// SPDX-License-Identifier: GPL-2.0

#include "action.h"
#include "action_util.h"
#include "keyboard.h"
#include "timer.h"
#include QMK_KEYBOARD_H

void clear_osm_mods(void);

enum layers {
  _COLEMAK_DH,
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

enum custom_keycodes {
    EX_LAYER = SAFE_RANGE,
    EX_LAYER_MAX = EX_LAYER + 10,
    EX_ONE_SHOT_MOD,
    EX_ONE_SHOT_MOD_MAX = EX_ONE_SHOT_MOD + EX_NUM_MODS - 1,
    CLR_OSM,
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
static uint16_t ex_osm_keys[EX_NUM_OSM_KEYS];
static uint8_t ex_osm_key_count = 0;

bool process_record_user(uint16_t keycode, keyrecord_t *record) {
    if (keycode >= EX_LAYER && keycode <= EX_LAYER_MAX) {
        uint8_t layer = keycode - EX_LAYER;
        if (record->event.pressed) {
            if (stack_size < sizeof(layer_stack)) {
                layer_stack[stack_size++] = layer;
            }
            layer_move(layer);
        } else {
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
                layer_move(layer_stack[stack_size-1]);
            } else {
                layer_move(0);
            }
        }
        return false;
    } else if (keycode >= EX_ONE_SHOT_MOD && keycode <= EX_ONE_SHOT_MOD_MAX) {
        uint8_t mod = keycode - EX_ONE_SHOT_MOD;
        uint8_t code = EX_MOD(mod);
        uint8_t bit = MOD_BIT(code);
        if (record->event.pressed) {
            register_code(code);
            send_keyboard_report();
            ex_mod_bits |= bit;
            ex_osm_bits |= bit;
            last_mod_time[mod] = timer_read32();
        } else {
            if ((ex_osm_bits & bit) == 0) {
                unregister_code(code);
                send_keyboard_report();
            }
            ex_mod_bits &= ~bit;
        }
        return false;
    } else if (keycode == CLR_OSM) {
        ex_osm_key_count = 0;
        clear_osm_mods();
        send_keyboard_report();
        return false;
    } else if (ex_osm_bits > 0) {
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
    return true;
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
    bool need_report = false;
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
                        need_report = true;
                    }
                }
            }
        }
        if (need_report) {
            send_keyboard_report();
        }
    }
}

const uint16_t PROGMEM keymaps[][MATRIX_ROWS][MATRIX_COLS] = {
    [_COLEMAK_DH] = LAYOUT_split_3x6_3(
        KC_ESC,   KC_Q,  KC_W,  KC_F,  KC_P,  KC_B,      KC_J,  KC_L,  KC_U,    KC_Y,   KC_QUOT, KC_BSPC,
        TG(_MSE), KC_A,  KC_R,  KC_S,  KC_T,  KC_G,      KC_M,  KC_N,  KC_E,    KC_I,   KC_O,    KC_ENT,
        CLR_OSM,  KC_Z,  KC_X,  KC_C,  KC_D,  KC_V,      KC_K,  KC_H,  KC_COMM, KC_DOT, KC_SLSH, KC_DEL,
                   EX_MO(_NUM), KC_LSFT, EX_MO(_EXT),      EX_MO(_SYM), KC_SPC, EX_MO(_FNC)
    ),
    [_EXT] = LAYOUT_split_3x6_3(
        KC_TRNS, KC_ESC, _BAK,  _FND,  _FWD,  KC_INS,   KC_PGUP, KC_HOME, KC_UP,   KC_END,  KC_CAPS, KC_TRNS,
        KC_NO,   _LALT,  _LGUI, _LSFT, _LCTL, _RALT,    KC_PGDN, KC_LEFT, KC_DOWN, KC_RGHT, KC_DEL,  KC_TRNS,
        KC_NO,   _UNDO,  _CUT,  _COPY, _WIN,  _PSTE,    KC_ENT,  KC_BSPC, KC_TAB,  KC_APP,  KC_PSCR, KC_TRNS,
                          KC_TRNS, KC_TRNS, KC_TRNS,    KC_TRNS, KC_TRNS, KC_TRNS
    ),
    [_FNC] = LAYOUT_split_3x6_3(
        KC_TRNS, KC_MSTP, KC_MPRV, KC_MPLY,   KC_MNXT, KC_BRIU,      KC_F12, KC_F7, KC_F8, KC_F9, KC_SCRL, KC_TRNS,
        KC_NO,   _LALT,   _LGUI,   _LSFT,     _LCTL,   KC_BRID,      KC_F11, KC_F4, KC_F5, KC_F6, KC_NO,   KC_TRNS,
        KC_NO,  KC_MUTE, KC_VOLD, RCS(KC_C), KC_VOLU, RCS(KC_V),    KC_F10, KC_F1, KC_F2, KC_F3, KC_NO,   KC_TRNS,
                                       KC_TRNS, KC_SPC, KC_TRNS,    KC_TRNS, KC_TRNS, KC_TRNS
    ),
    [_SYM] = LAYOUT_split_3x6_3(
        KC_TRNS, KC_EXLM, KC_AT, KC_HASH, KC_DLR,  KC_PERC,      KC_EQL,  KC_GRV,  KC_COLN, KC_SCLN, KC_PLUS, KC_TRNS,
        KC_NO,   _LALT,   _LGUI, _LSFT,   _LCTL,   KC_CIRC,      KC_ASTR, KC_LPRN, KC_LCBR, KC_LBRC, KC_MINS, KC_TRNS,
        KC_NO,   CW_TOGG, KC_NO, KC_BSLS, KC_PIPE, KC_AMPR,      KC_TILD, KC_RPRN, KC_RCBR, KC_RBRC, KC_UNDS, KC_TRNS,
                                 KC_TRNS, KC_SPC, KC_TRNS,      KC_TRNS, KC_TRNS, KC_TRNS
    ),
    [_NUM] = LAYOUT_split_3x6_3(
        KC_TRNS, KC_NO, KC_NO,  KC_NO,  KC_DOT,  KC_NUM,      KC_EQL,  KC_7, KC_8, KC_9, KC_0, KC_TRNS,
        KC_NO,   _LALT, _LGUI,  _LSFT,  _LCTL,   _RALT,       KC_ASTR, KC_4, KC_5, KC_6, KC_DOT, KC_TRNS,
        KC_NO,   KC_NO, KC_APP, KC_TAB, KC_BSPC, KC_ENT,      KC_TILD, KC_1, KC_2, KC_3, KC_SLSH, KC_TRNS,
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
            return true;

        default:
            return false;  // Deactivate Caps Word.
    }
}