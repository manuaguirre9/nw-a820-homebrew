/* keys.h: NW-A829 button codes from /dev/input/event0 (measured 2026-09-26 with homebrew/keymap).
 * Events are type 1 (EV_KEY); NOTE the inverted value: 0 = pressed, 1 = released.
 * The HOLD switch sends code 143 then code 15 (value 0) on every move, in both directions. */
#ifndef KEYS_H
#define KEYS_H
#define K_PLAY    0	/* centre */
#define K_RIGHT   1
#define K_LEFT    2
#define K_UP      3
#define K_DOWN    4
#define K_VOLDN   16
#define K_VOLUP   17
#define K_BACK    18	/* BACK / HOME */
#define K_OPTION  19
#define K_BT      20	/* Bluetooth button */
#define K_HOLD_A  143
#define K_HOLD_B  15
#define KEY_PRESSED(v)  ((v) == 0)
#endif
