#pragma once

#define POINTING_DEVICE_HIRES_SCROLL_ENABLE 0
#define POINTING_DEVICE_HIRES_SCROLL_MULTIPLIER 15 // Maybe play with this
#define WHEEL_EXTENDED_REPORT // Necessary to send wheel reports with 16-bit values to avoid overflowing

// TO DO - Also throttle the rate at which scroll messages are sent in ploopyco.c

#define PLOOPY_DRAGSCROLL_MOMENTARY
#define PLOOPY_DRAGSCROLL_DIVISOR_H 120.0
#define PLOOPY_DRAGSCROLL_DIVISOR_V 80.0
#define PLOOPY_DRAGSCROLL_INVERT

#define PLOOPY_DRAGSCROLL_SCROLLOCK