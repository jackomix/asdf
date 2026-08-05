/* input.c -- minimal input for GDS (Kairosoft touch game).
 * GDS is driven by touch/pointer, not a gamepad.  We provide the reference
 * gds_input_* interface: a clean-exit chord (SELECT+START via SDL event queue
 * through our dlopen SDL, polled from the render loop) and no-op soft-keyboard
 * callbacks.  No SDL2 headers needed - reached through dlsym. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dlfcn.h>
#include <signal.h>
#include <stdint.h>

#include "gds.h"
#include "nx_elf.h"

static volatile int g_exit_requested;

void gds_input_request_exit(void) { g_exit_requested = 1; }
int  gds_input_exit_requested(void) { return g_exit_requested; }

/* optional SDL event pump for the exit chord; reached via dlsym so no headers */
static void *sdl_h;
typedef struct { uint32_t type, timestamp; int32_t windowID; uint8_t state;
                 uint8_t padding1, padding2, padding3; uint8_t axis, padding4;
                 int16_t padding5; int16_t value, padding6; } sdl_joystick_event;
typedef struct { uint32_t type, timestamp; uint8_t state, button, padding1, padding2;
                 uint8_t which, padding3, padding4, padding5; } sdl_controller_event;
#define SDL_JOYDEVICEADDED 0x601
#define SDL_CONTROLLERDEVICEADDED 0x650
#define SDL_CONTROLLERBUTTONDOWN 0x651
#define SDL_CONTROLLERBUTTONUP 0x652
#define SDL_CONTROLLER_BUTTON_BACK 4
#define SDL_CONTROLLER_BUTTON_START 6
#define SDL_EVENTQUIT 0x100

static int poll_exit(void) {
    /* Poll SDL events (dlopen).  Return 1 if SELECT+START or QUIT. */
    typedef int (*t_poll)(void);
    typedef void (*t_pump)(void);
    static t_poll spoll; static t_pump spump;
    if (!sdl_h) {
        sdl_h = dlopen("libSDL2.so.0", RTLD_NOW | RTLD_GLOBAL);
        if (!sdl_h) sdl_h = dlopen("libSDL2-2.0.so.0", RTLD_NOW | RTLD_GLOBAL);
        if (sdl_h) {
            spump = (t_pump)dlsym(sdl_h, "SDL_PumpEvents");
            spoll = (t_poll)dlsym(sdl_h, "SDL_PollEvent");
        }
    }
    if (!spoll || !spump) return 0;
    static int sel_down, start_down;
    spump();
    /* poll a bounded number of events */
    for (int i = 0; i < 64; i++) {
        /* event is 56 bytes; we read the first uint32 (type) + relevant bytes */
        unsigned char ev[56];
        typedef int (*t_pe)(unsigned char *);
        if (((t_pe)spoll)(ev) == 0) break;
        uint32_t type = ((uint32_t *)ev)[0];
        if (type == SDL_CONTROLLERBUTTONDOWN || type == SDL_CONTROLLERBUTTONUP) {
            uint8_t button = ev[6];
            int down = type == SDL_CONTROLLERBUTTONDOWN;
            if (button == SDL_CONTROLLER_BUTTON_BACK) sel_down = down;
            if (button == SDL_CONTROLLER_BUTTON_START) start_down = down;
            if (sel_down && start_down) return 1;
        }
    }
    return 0;
}

int gds_input_init(void) { return 0; }

void gds_input_poll(void *env, void *player, unsigned long frame) {
    (void)env; (void)player; (void)frame;
    if (poll_exit()) g_exit_requested = 1;
}

void gds_input_close(void) { }

int gds_input_cursor(float *x, float *y) { (void)x; (void)y; return 0; }
void gds_input_set_screen_size(int width, int height) { (void)width; (void)height; }

void gds_input_keyboard_open(const char *initial, int character_limit) {
    (void)initial; (void)character_limit; }
void gds_input_keyboard_set(const char *text) { (void)text; }
void gds_input_keyboard_hide(void) { }
int gds_input_keyboard_snapshot(char *text, size_t text_size,
                                int *uppercase, int *selected,
                                const gds_keyboard_key **keys, size_t *key_count) {
    (void)text; (void)text_size; (void)uppercase; (void)selected;
    if (keys) *keys = NULL; if (key_count) *key_count = 0;
    return 0;
}
