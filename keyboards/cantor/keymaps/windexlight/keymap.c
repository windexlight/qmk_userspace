// Copyright 2022 Diego Palacios (@diepala)
// SPDX-License-Identifier: GPL-2.0

#include QMK_KEYBOARD_H

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

#define _LALT OSM(MOD_LALT)
#define _LGUI OSM(MOD_LGUI)
#define _LSFT OSM(MOD_LSFT)
#define _LCTL OSM(MOD_LCTL)
#define _RALT OSM(MOD_RALT)

#define _UNDO LCTL(KC_Z)
#define _CUT LCTL(KC_X)
#define _COPY LCTL(KC_C)
#define _WIN KC_LGUI
#define _PSTE LCTL(KC_V)

enum custom_keycodes {
    EX_LAYER = SAFE_RANGE,
    EX_LAYER_MAX = EX_LAYER + 10
};

#define EX_MO(n) (EX_LAYER + n)

static uint8_t layer_stack[6];
static uint8_t stack_size = 0;

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
    }
    return true;
}

const uint16_t PROGMEM keymaps[][MATRIX_ROWS][MATRIX_COLS] = {
     /*
      * ┌───┬───┬───┬───┬───┬───┐       ┌───┬───┬───┬───┬───┬───┐
      * │Tab│ Q │ W │ E │ R │ T │       │ Y │ U │ I │ O │ P │Bsp│
      * ├───┼───┼───┼───┼───┼───┤       ├───┼───┼───┼───┼───┼───┤
      * │Ctl│ A │ S │ D │ F │ G │       │ H │ J │ K │ L │ ; │ ' │
      * ├───┼───┼───┼───┼───┼───┤       ├───┼───┼───┼───┼───┼───┤
      * │Sft│ Z │ X │ C │ V │ B │       │ N │ M │ , │ . │ / │Sft│
      * └───┴───┴───┴───┴───┴───┘       └───┴───┴───┴───┴───┴───┘
      *               ┌───┐                   ┌───┐
      *               │GUI├───┐           ┌───┤Alt│
      *               └───┤   ├───┐   ┌───┤   ├───┘
      *                   └───┤Bsp│   │Ent├───┘
      *                       └───┘   └───┘
      */
    [_COLEMAK_DH] = LAYOUT_split_3x6_3(
        KC_ESC,   KC_Q,  KC_W,  KC_F,  KC_P,  KC_B,      KC_J,  KC_L,  KC_U,    KC_Y,   KC_QUOT, KC_BSPC,
        TG(_MSE), KC_A,  KC_R,  KC_S,  KC_T,  KC_G,      KC_M,  KC_N,  KC_E,    KC_I,   KC_O,    KC_ENT,
        KC_NO,    KC_Z,  KC_X,  KC_C,  KC_D,  KC_V,      KC_K,  KC_H,  KC_COMM, KC_DOT, KC_SLSH, KC_DEL,
                   EX_MO(_NUM), EX_MO(_EXT), _LSFT,      EX_MO(_SYM), KC_SPC, EX_MO(_FNC)
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
                                       KC_TRNS, KC_TRNS, KC_TRNS,    KC_TRNS, KC_TRNS, KC_TRNS
    ),
    [_SYM] = LAYOUT_split_3x6_3(
        KC_TRNS, KC_EXLM, KC_AT, KC_HASH, KC_DLR,  KC_PERC,      KC_EQL,  KC_GRV,  KC_COLN, KC_SCLN, KC_PLUS, KC_TRNS,
        KC_NO,   _LALT,   _LGUI, _LSFT,   _LCTL,   KC_CIRC,      KC_ASTR, KC_LPRN, KC_LCBR, KC_LBRC, KC_MINS, KC_TRNS,
        KC_NO,   CW_TOGG, KC_NO, KC_BSLS, KC_PIPE, KC_AMPR,      KC_TILD, KC_RPRN, KC_RCBR, KC_RBRC, KC_UNDS, KC_TRNS,
                                 KC_TRNS, KC_TRNS, KC_TRNS,      KC_TRNS, KC_TRNS, KC_TRNS
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