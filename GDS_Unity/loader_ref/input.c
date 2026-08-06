/* input.c -- R36S gamepad -> UnityEngine.Input for Game Dev Story.
 *
 * GDS reads input ONLY through UnityEngine.Input (legacy): GetKey*,
 * mousePosition/GetMouseButton*, touches, InputUnsafeUtility axes.
 * On Android those are filled by Unity's Java MotionEvent/InputDevice
 * pipeline, which does not exist under our loader -- so every read returns
 * "nothing pressed".  We fix that at the seam the reference ports use
 * (terraria-nextos native_pad.c, verbatim technique): resolve the managed
 * methods BY NAME through the il2cpp runtime exports and replace their
 * bodies with `ldr x16,[pc,#8]; br x16` trampolines into this file, then
 * answer them from an SDL_GameController driving a virtual pointer:
 *
 *   left stick / dpad  -> pointer (also fed as a synthetic touch while held)
 *   A                  -> mouse button 0 + touch down  (tap / drag)
 *   B                  -> mouse button 2..1 (kairo right-click = cancel)
 *   SELECT+START       -> exit chord (kept from the old input.c)
 *
 * A tiny GL overlay draws the pointer (egl_shim.c calls
 * gds_input_draw_cursor() right before presenting).
 *
 * Switches (gds_env.cfg): GDS_CURSOR=0 hides the overlay & stops feeding
 * pointer, GDS_ARROWS=1 also feeds dpad/stick as arrow keys +
 * Horizontal/Vertical axes, GDS_JOYNAME=1 reports a joystick via
 * Input.GetJoystickNames (game controller-mode UI probing).
 */
#define _GNU_SOURCE
#include <stdio.h>
#include "musl_compat.h"
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <dlfcn.h>
#include <errno.h>
#include <sys/mman.h>

#include "gds.h"
#include "nx_elf.h"

extern int egl_shim_screen_w(void);
extern int egl_shim_screen_h(void);

/* ------------------------------------------------------------------ SDL */
/* No SDL2 headers in this build: resolve the controller API via dlsym,
 * exactly like the old exit-chord poller did. */
#define SDL_INIT_JOYSTICK       0x00000200u
#define SDL_INIT_GAMECONTROLLER 0x00002000u
#define SDL_INIT_EVENTS         0x00004000u
#define SDL_CONTROLLER_BUTTON_A             0
#define SDL_CONTROLLER_BUTTON_B             1
#define SDL_CONTROLLER_BUTTON_X             2
#define SDL_CONTROLLER_BUTTON_Y             3
#define SDL_CONTROLLER_BUTTON_BACK          4
#define SDL_CONTROLLER_BUTTON_GUIDE         5
#define SDL_CONTROLLER_BUTTON_START         6
#define SDL_CONTROLLER_BUTTON_LEFTSTICK     7
#define SDL_CONTROLLER_BUTTON_RIGHTSTICK    8
#define SDL_CONTROLLER_BUTTON_LEFTSHOULDER  9
#define SDL_CONTROLLER_BUTTON_RIGHTSHOULDER 10
#define SDL_CONTROLLER_BUTTON_DPAD_UP       11
#define SDL_CONTROLLER_BUTTON_DPAD_DOWN     12
#define SDL_CONTROLLER_BUTTON_DPAD_LEFT     13
#define SDL_CONTROLLER_BUTTON_DPAD_RIGHT    14
#define SDL_ENABLE 1

typedef struct SDL_GameController SDL_GameController;
typedef struct SDL_Joystick SDL_Joystick;
typedef struct { unsigned char data[16]; } SDL_JoystickGUID;

static void *sdl_h;
static int (*s_InitSubSystem)(uint32_t);
static int (*s_NumJoysticks)(void);
static int (*s_IsGameController)(int);
static SDL_GameController *(*s_GameControllerOpen)(int);
static void (*s_GameControllerClose)(SDL_GameController *);
static const char *(*s_GameControllerName)(SDL_GameController *);
static uint8_t (*s_GameControllerGetButton)(SDL_GameController *, int);
static int16_t (*s_GameControllerGetAxis)(SDL_GameController *, int);
static void (*s_GameControllerUpdate)(void);
static int (*s_GameControllerGetAttached)(SDL_GameController *);
static SDL_Joystick *(*s_GameControllerGetJoystick)(SDL_GameController *);
static SDL_JoystickGUID (*s_JoystickGetGUID)(SDL_Joystick *);
static void (*s_JoystickGetGUIDString)(SDL_JoystickGUID, char *, int);
static int (*s_GameControllerAddMapping)(const char *);
static int (*s_GameControllerAddMappingsFromFile)(const char *);
static const char *(*s_GetError)(void);

static void *sdl_sym(const char *n) {
    return sdl_h ? dlsym(sdl_h, n) : NULL;
}

static int sdl_input_load(void) {
    if (sdl_h) return 1;
    const char *names[] = { "libSDL2-2.0.so.0", "libSDL2.so.0", "libSDL2.so", 0 };
    for (int i = 0; names[i] && !sdl_h; i++)
        sdl_h = dlopen(names[i], RTLD_NOW | RTLD_NOLOAD);
    if (!sdl_h)
        for (int i = 0; names[i] && !sdl_h; i++)
            sdl_h = dlopen(names[i], RTLD_NOW | RTLD_GLOBAL);
    if (!sdl_h) return 0;
    s_InitSubSystem   = (int (*)(uint32_t))sdl_sym("SDL_InitSubSystem");
    s_NumJoysticks    = (int (*)(void))sdl_sym("SDL_NumJoysticks");
    s_IsGameController= (int (*)(int))sdl_sym("SDL_IsGameController");
    s_GameControllerOpen  = (SDL_GameController *(*)(int))sdl_sym("SDL_GameControllerOpen");
    s_GameControllerClose = (void (*)(SDL_GameController *))sdl_sym("SDL_GameControllerClose");
    s_GameControllerName  = (const char *(*)(SDL_GameController *))sdl_sym("SDL_GameControllerName");
    s_GameControllerGetButton = (uint8_t (*)(SDL_GameController *, int))sdl_sym("SDL_GameControllerGetButton");
    s_GameControllerGetAxis   = (int16_t (*)(SDL_GameController *, int))sdl_sym("SDL_GameControllerGetAxis");
    s_GameControllerUpdate    = (void (*)(void))sdl_sym("SDL_GameControllerUpdate");
    s_GameControllerGetAttached = (int (*)(SDL_GameController *))sdl_sym("SDL_GameControllerGetAttached");
    s_GameControllerGetJoystick = (SDL_Joystick *(*)(SDL_GameController *))sdl_sym("SDL_GameControllerGetJoystick");
    s_JoystickGetGUID       = (SDL_JoystickGUID (*)(SDL_Joystick *))sdl_sym("SDL_JoystickGetGUID");
    s_JoystickGetGUIDString = (void (*)(SDL_JoystickGUID, char *, int))sdl_sym("SDL_JoystickGetGUIDString");
    s_GameControllerAddMapping = (int (*)(const char *))sdl_sym("SDL_GameControllerAddMapping");
    s_GameControllerAddMappingsFromFile = (int (*)(const char *))sdl_sym("SDL_GameControllerAddMappingsFromFile");
    s_GetError = (const char *(*)(void))sdl_sym("SDL_GetError");
    return s_InitSubSystem && s_GameControllerOpen && s_GameControllerGetButton;
}

/* ---------------------------------------------------------- pad state */
enum { NPB_A, NPB_B, NPB_X, NPB_Y, NPB_LB, NPB_RB, NPB_BACK, NPB_START,
       NPB_L3, NPB_R3, NPB_DU, NPB_DD, NPB_DL, NPB_DR, NPB_COUNT };
enum { NPA_LX, NPA_LY, NPA_RX, NPA_RY, NPA_LT, NPA_RT, NPA_COUNT };

static unsigned char g_npb[NPB_COUNT];
static float g_npa[NPA_COUNT];
static SDL_GameController *g_gc;
static int g_sdl_inited, g_open_retry;

static volatile int g_exit_requested;
void gds_input_request_exit(void) { g_exit_requested = 1; }
int  gds_input_exit_requested(void) { return g_exit_requested; }

static int cursor_on(void) {
    static int c = -1;
    if (c < 0) {
        const char *e = getenv("GDS_CURSOR");
        c = (e && atoi(e) == 0) ? 0 : 1;
    }
    return c;
}
static int arrows_on(void) {
    static int c = -1;
    if (c < 0) c = getenv("GDS_ARROWS") && atoi(getenv("GDS_ARROWS")) > 0;
    return c;
}
static int joyname_on(void) {
    static int c = -1;
    if (c < 0) c = getenv("GDS_JOYNAME") && atoi(getenv("GDS_JOYNAME")) > 0;
    return c;
}

static void pad_open(void) {
    if (!g_sdl_inited) {
        g_sdl_inited = 1;
        if (!sdl_input_load()) {
            fprintf(stderr, "[input] SDL2 controller API unavailable\n");
            return;
        }
        s_InitSubSystem(SDL_INIT_JOYSTICK | SDL_INIT_GAMECONTROLLER |
                        SDL_INIT_EVENTS);
        /* ArkOS/EmuELEC-style mapping file locations, then a generic 15-button
         * fallback for unmapped pads (native_pad.c verbatim approach). */
        if (s_GameControllerAddMappingsFromFile) {
            const char *p = getenv("SDL_GAMECONTROLLERCONFIG_FILE");
            if (p && *p && access(p, R_OK) == 0) {
                int n = s_GameControllerAddMappingsFromFile(p);
                fprintf(stderr, "[input] %d pad profiles from %s\n", n, p);
            }
        }
    }
    if (g_gc) return;
    if (!s_NumJoysticks || !s_IsGameController) return;
    int count = s_NumJoysticks();
    for (int i = 0; i < count; i++) {
        if (!s_IsGameController(i)) {
            /* generic fallback mapping for unknown pads (native_pad.c) */
            if (!s_GameControllerAddMapping || !s_JoystickGetGUID ||
                !s_JoystickGetGUIDString || !s_GameControllerGetJoystick)
                continue;
            SDL_Joystick *probe = NULL;
            void *(*jopen)(int) = (void *(*)(int))sdl_sym("SDL_JoystickOpen");
            void (*jclose)(void *) = (void (*)(void *))sdl_sym("SDL_JoystickClose");
            int (*jbtns)(void *) = (int (*)(void *))sdl_sym("SDL_JoystickNumButtons");
            int (*jaxes)(void *) = (int (*)(void *))sdl_sym("SDL_JoystickNumAxes");
            if (jopen) probe = jopen(i);
            int nb = probe && jbtns ? jbtns(probe) : 0;
            int na = probe && jaxes ? jaxes(probe) : 0;
            if (probe && jclose) jclose(probe);
            if (nb < 8 || na < 2) continue;
            SDL_JoystickGUID guid = { { 0 } };
            /* derive guid from the same handle SDL will use */
            if (jopen) {
                probe = jopen(i);
                if (probe) { guid = s_JoystickGetGUID(probe); jclose(probe); }
            }
            char gt[64] = { 0 };
            s_JoystickGetGUIDString(guid, gt, sizeof gt);
            if (!gt[0]) continue;
            char mapping[1024];
            snprintf(mapping, sizeof mapping,
                     "%s,GDS Generic Pad,platform:Linux,"
                     "a:b0,b:b1,x:b2,y:b3,"
                     "leftshoulder:b4,rightshoulder:b5,"
                     "lefttrigger:b6,righttrigger:b7,"
                     "back:b8,start:b9,leftstick:b10,rightstick:b11,"
                     "leftx:a0,lefty:a1,rightx:a2,righty:a3,"
                     "dpup:h0.1,dpdown:h0.4,dpleft:h0.8,dpright:h0.2,", gt);
            if (s_GameControllerAddMapping(mapping) >= 0)
                fprintf(stderr, "[input] generic mapping applied to js%d (%d btn %d axes)\n",
                        i, nb, na);
        }
        g_gc = s_GameControllerOpen(i);
        if (!g_gc) continue;
        const char *nm = s_GameControllerName ? s_GameControllerName(g_gc) : NULL;
        fprintf(stderr, "[input] controller: %s\n", nm ? nm : "?");
        return;
    }
    if (g_open_retry == 0)
        fprintf(stderr, "[input] no controller yet (NumJoysticks=%d); hot-plug retry\n",
                s_NumJoysticks ? s_NumJoysticks() : -1);
}

static void pad_poll(void) {
    if (!g_gc) {
        if (g_open_retry-- <= 0) { g_open_retry = 120; pad_open(); }
        if (!g_gc) return;
    }
    if (s_GameControllerGetAttached && !s_GameControllerGetAttached(g_gc)) {
        fprintf(stderr, "[input] controller detached\n");
        if (s_GameControllerClose) s_GameControllerClose(g_gc);
        g_gc = NULL;
        return;
    }
    if (s_GameControllerUpdate) s_GameControllerUpdate();
#define BTN(b) (s_GameControllerGetButton ? s_GameControllerGetButton(g_gc, b) : 0)
    g_npb[NPB_A]     = BTN(SDL_CONTROLLER_BUTTON_A);
    g_npb[NPB_B]     = BTN(SDL_CONTROLLER_BUTTON_B);
    g_npb[NPB_X]     = BTN(SDL_CONTROLLER_BUTTON_X);
    g_npb[NPB_Y]     = BTN(SDL_CONTROLLER_BUTTON_Y);
    g_npb[NPB_LB]    = BTN(SDL_CONTROLLER_BUTTON_LEFTSHOULDER);
    g_npb[NPB_RB]    = BTN(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER);
    g_npb[NPB_BACK]  = BTN(SDL_CONTROLLER_BUTTON_BACK);
    g_npb[NPB_START] = BTN(SDL_CONTROLLER_BUTTON_START);
    g_npb[NPB_L3]    = BTN(SDL_CONTROLLER_BUTTON_LEFTSTICK);
    g_npb[NPB_R3]    = BTN(SDL_CONTROLLER_BUTTON_RIGHTSTICK);
    g_npb[NPB_DU]    = BTN(SDL_CONTROLLER_BUTTON_DPAD_UP);
    g_npb[NPB_DD]    = BTN(SDL_CONTROLLER_BUTTON_DPAD_DOWN);
    g_npb[NPB_DL]    = BTN(SDL_CONTROLLER_BUTTON_DPAD_LEFT);
    g_npb[NPB_DR]    = BTN(SDL_CONTROLLER_BUTTON_DPAD_RIGHT);
#undef BTN
    static const int ax_map[NPA_COUNT] = { 0, 1, 2, 3, 4, 5 };
    for (int i = 0; i < NPA_COUNT; i++) {
        int16_t v = s_GameControllerGetAxis ? s_GameControllerGetAxis(g_gc, ax_map[i]) : 0;
        g_npa[i] = v < 0 ? (float)v / 32768.0f : (float)v / 32767.0f;
    }
    if (g_npa[NPA_LT] < 0.0f) g_npa[NPA_LT] = 0.0f;
    if (g_npa[NPA_RT] < 0.0f) g_npa[NPA_RT] = 0.0f;

    /* exit chord: SELECT+START (native_pad.c Bully/Sonic pattern) */
    if (!g_exit_requested && g_npb[NPB_BACK] && g_npb[NPB_START]) {
        g_exit_requested = 1;
        fprintf(stderr, "[input] SELECT+START -> exit\n");
    }
}

/* ------------------------------------------------ virtual pointer state */
static float g_cur_x = 320.0f, g_cur_y = 240.0f;
static int g_press0, g_press1;            /* mouse button 0 / 1 held */
static int g_press0_edge_down, g_press0_edge_up;
static int g_press1_edge_down, g_press1_edge_up;
static int g_moved_this_frame;
static int g_prev_press0 = -1;            /* -1: no touch recorded yet */
static int g_touch_ended_frame;

/* Keyboard feed (arrows etc.), Unity KeyCode values */
#define KC_RETURN 13
#define KC_ESCAPE 27
#define KC_SPACE 32
#define KC_UP 273
#define KC_DOWN 274
#define KC_RIGHT 275
#define KC_LEFT 276

static void frame_update(void) {
    /* pointer movement: stick with deadzone+accel, dpad digital */
    float dx = 0.0f, dy = 0.0f;
    float dz = 0.18f;
    float ax = g_npa[NPA_LX], ay = g_npa[NPA_LY];
    if (ax > dz || ax < -dz) dx = ax * ax * (ax > 0 ? 1.0f : -1.0f) * 9.0f;
    if (ay > dz || ay < -dz) dy = ay * ay * (ay > 0 ? 1.0f : -1.0f) * 9.0f;
    if (g_npb[NPB_DL]) dx -= 5.0f;
    if (g_npb[NPB_DR]) dx += 5.0f;
    if (g_npb[NPB_DU]) dy -= 5.0f;
    if (g_npb[NPB_DD]) dy += 5.0f;
    g_moved_this_frame = (dx != 0.0f || dy != 0.0f);
    g_cur_x += dx;
    g_cur_y += dy;
    int w = egl_shim_screen_w(), h = egl_shim_screen_h();
    if (w <= 0) w = 640;
    if (h <= 0) h = 480;
    if (g_cur_x < 0) g_cur_x = 0;
    if (g_cur_y < 0) g_cur_y = 0;
    if (g_cur_x > w - 1) g_cur_x = (float)(w - 1);
    if (g_cur_y > h - 1) g_cur_y = (float)(h - 1);

    g_press0_edge_down = g_press0_edge_up = 0;
    g_press1_edge_down = g_press1_edge_up = 0;
    int b0 = cursor_on() ? g_npb[NPB_A] : 0;
    int b1 = cursor_on() ? g_npb[NPB_B] : 0;
    if (b0 && !g_press0) g_press0_edge_down = 1;
    if (!b0 && g_press0) g_press0_edge_up = 1;
    if (b1 && !g_press1) g_press1_edge_down = 1;
    if (!b1 && g_press1) g_press1_edge_up = 1;
    g_press0 = b0;
    g_press1 = b1;

    /* synthetic touch phases across frames:
     * -1 none -> down Began -> (Moved/Stationary)... -> up Ended(one frame) */
    if (g_press0 && g_prev_press0 != 1) {
        g_prev_press0 = 1;                 /* down edge: Began */
        g_touch_ended_frame = 0;
    } else if (g_press0) {
        g_prev_press0 = 2;                 /* held */
    } else if (g_prev_press0 > 0) {
        g_prev_press0 = -2;                /* up edge: Ended this frame */
        g_touch_ended_frame = 1;
    } else {
        g_prev_press0 = 0;
        g_touch_ended_frame = 0;
    }
}

/* ---------------------------------------------- icall bodies (C ABI) */
static int inp_GetKeyInt(int key) {
    if (!arrows_on()) {
        if (key == KC_ESCAPE) return g_npb[NPB_B];
        if (key == KC_RETURN || key == KC_SPACE) return 0;
        return 0;
    }
    switch (key) {
    case KC_UP:    return g_npb[NPB_DU] || g_npa[NPA_LY] < -0.5f;
    case KC_DOWN:  return g_npb[NPB_DD] || g_npa[NPA_LY] > 0.5f;
    case KC_LEFT:  return g_npb[NPB_DL] || g_npa[NPA_LX] < -0.5f;
    case KC_RIGHT: return g_npb[NPB_DR] || g_npa[NPA_LX] > 0.5f;
    default: break;
    }
    return 0;
}
/* Down/Up edges recompute each frame; we snapshot per-frame key edges. */
static int g_key_now[512], g_key_prev[512];
static int inp_GetKeyDownInt(int key) {
    if (key < 0 || key >= 512) return 0;
    return g_key_now[key] && !g_key_prev[key];
}
static int inp_GetKeyUpInt(int key) {
    if (key < 0 || key >= 512) return 0;
    return !g_key_now[key] && g_key_prev[key];
}
static int inp_anyKey(void) {
    if (!cursor_on()) return 0;
    for (int i = 0; i < NPB_COUNT; i++)
        if (g_npb[i]) return 1;
    return 0;
}
static void inp_get_mousePosition(float *out3) {
    int h = egl_shim_screen_h();
    if (h <= 0) h = 480;
    out3[0] = g_cur_x;
    out3[1] = (float)h - 1.0f - g_cur_y;   /* Unity origin: bottom-left */
    out3[2] = 0.0f;
}
static void inp_get_mouseScrollDelta(float *out2) { out2[0] = out2[1] = 0.0f; }
static int inp_GetMouseButton(int btn) {
    if (btn == 0) return g_press0;
    if (btn == 1) return g_press1;
    return 0;
}
static int inp_GetMouseButtonDown(int btn) {
    if (btn == 0) return g_press0_edge_down;
    if (btn == 1) return g_press1_edge_down;
    return 0;
}
static int inp_GetMouseButtonUp(int btn) {
    if (btn == 0) return g_press0_edge_up;
    if (btn == 1) return g_press1_edge_up;
    return 0;
}
static int inp_get_touchCount(void) {
    if (!cursor_on()) return 0;
    if (g_prev_press0 > 0 || g_touch_ended_frame) return 1;
    return 0;
}
static int inp_get_touchSupported(void) { return cursor_on(); }
static int inp_get_mousePresent(void) { return cursor_on(); }
static int inp_get_multiTouchEnabled(void) { return 0; }
static void inp_nop(void) { }
static void *inp_empty_string(void);

/* UnityEngine.Touch (Unity 2022 layout, 60 bytes per kairovm's buffer) */
static void inp_GetTouch(int index, unsigned char *out) {
    (void)index;
    memset(out, 0, 60);
    int h = egl_shim_screen_h();
    if (h <= 0) h = 480;
    float ux = g_cur_x, uy = (float)h - 1.0f - g_cur_y;
    *(int *)(out + 0) = 0;                 /* fingerId */
    *(float *)(out + 4) = ux;              /* position */
    *(float *)(out + 8) = uy;
    *(float *)(out + 12) = ux;             /* rawPosition */
    *(float *)(out + 16) = uy;

    int phase = 2;                          /* Stationary */
    if (g_prev_press0 == 1) phase = 0;      /* Began */
    else if (g_prev_press0 == 2)
        phase = g_moved_this_frame ? 1 : 2; /* Moved / Stationary */
    else if (g_touch_ended_frame) phase = 3;/* Ended */
    *(int *)(out + 40) = phase;             /* phase enum */
    *(int *)(out + 44) = phase == 3 || phase == 0 ? 1 : 1; /* tapCount */
    *(float *)(out + 28) = 1.0f / 60.0f;    /* deltaTime */
    *(float *)(out + 32) = 1.0f;            /* pressure */
    *(int *)(out + 36) = 0;                 /* type: Direct */
}

static void *g_joystick_names;              /* pinned string[] */
static void *inp_GetJoystickNames(void) {
    return g_joystick_names;                /* NULL until install builds it */
}

/* InputUnsafeUtility axes: read the Il2CppString* name */
static float inp_GetAxis(void *il2_string) {
    if (!il2_string) return 0.0f;
    int len = *(int *)((char *)il2_string + 0x10);
    const uint16_t *s = (const uint16_t *)((char *)il2_string + 0x14);
    if (len <= 0 || len > 64) return 0.0f;
    char name[80];
    for (int i = 0; i < len; i++)
        name[i] = (char)(s[i] < 128 ? s[i] : '?');
    name[len] = 0;
    for (int i = 0; i < len; i++)
        if (name[i] >= 'A' && name[i] <= 'Z') name[i] += 32;
    if (strstr(name, "horizontal")) {
        float v = (float)(g_npb[NPB_DR] - g_npb[NPB_DL]);
        float a = g_npa[NPA_LX];
        if (a > 0.18f || a < -0.18f) v = a;
        return v;
    }
    if (strstr(name, "vertical")) {
        float v = (float)(g_npb[NPB_DU] - g_npb[NPB_DD]);
        float a = g_npa[NPA_LY];
        if (a > 0.18f || a < -0.18f) v = -a;   /* stick down = -1 (Android) */
        return v;
    }
    return 0.0f;
}
static int inp_GetButton(void *il2_string) { (void)il2_string; return 0; }

/* ----------------------------------------------- il2cpp method patching */
static void *g_il2_mod;
static void *(*il_domain_get)(void);
static const void **(*il_domain_get_assemblies)(void *, size_t *);
static void *(*il_assembly_get_image)(const void *);
static const char *(*il_image_get_name)(const void *);
static void *(*il_class_from_name)(void *, const char *, const char *);
static void *(*il_class_get_method_from_name)(void *, const char *, int);
static void *(*il_array_new)(void *, uintptr_t);
static void *(*il_string_new)(const char *);
static unsigned (*il_gchandle_new)(void *, int);
static void *(*il_class_from_il2cpp_type)(void *);
static int g_il_ready;

static int il_resolve(void) {
    if (g_il_ready) return 1;
    g_il2_mod = nx_find_mod("libil2cpp.so");
    if (!g_il2_mod) return 0;
#define R(v, n) v = (void *)nx_lookup_in(g_il2_mod, n)
    R(il_domain_get, "il2cpp_domain_get");
    R(il_domain_get_assemblies, "il2cpp_domain_get_assemblies");
    R(il_assembly_get_image, "il2cpp_assembly_get_image");
    R(il_image_get_name, "il2cpp_image_get_name");
    R(il_class_from_name, "il2cpp_class_from_name");
    R(il_class_get_method_from_name, "il2cpp_class_get_method_from_name");
    R(il_array_new, "il2cpp_array_new");
    R(il_string_new, "il2cpp_string_new");
    R(il_gchandle_new, "il2cpp_gchandle_new");
#undef R
    g_il_ready = il_domain_get && il_domain_get_assemblies &&
                 il_assembly_get_image && il_class_from_name &&
                 il_class_get_method_from_name;
    return g_il_ready;
}

static void *il_find_class(const char *want_image, const char *ns,
                           const char *name) {
    void *dom = il_domain_get();
    if (!dom) return NULL;
    size_t n = 0;
    const void **asms = il_domain_get_assemblies(dom, &n);
    for (size_t i = 0; asms && i < n; i++) {
        void *img = il_assembly_get_image(asms[i]);
        if (!img) continue;
        if (want_image && il_image_get_name) {
            const char *in = il_image_get_name(img);
            if (!in || !strstr(in, want_image)) continue;
        }
        void *c = il_class_from_name(img, ns, name);
        if (c) return c;
    }
    return NULL;
}

static int il_patch(void *method_info, void *fn, const char *what) {
    if (!method_info || !fn) return 0;
    void *addr = *(void **)method_info;    /* Il2Cpp MethodInfo: methodPointer @ +0 */
    if (!addr) return 0;
    long pgsz = sysconf(_SC_PAGESIZE);
    uintptr_t page = (uintptr_t)addr & ~((uintptr_t)pgsz - 1);
    if (mprotect((void *)page, (size_t)pgsz * 2,
                 PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
        fprintf(stderr, "[input] mprotect %s: %s\n", what, strerror(errno));
        return 0;
    }
    uint32_t *code = (uint32_t *)addr;
    code[0] = 0x58000050u;                 /* ldr x16, [pc, #8] */
    code[1] = 0xD61F0200u;                 /* br x16 */
    *(uint64_t *)(code + 2) = (uint64_t)(uintptr_t)fn;
    __builtin___clear_cache((char *)addr, (char *)addr + 16);
    mprotect((void *)page, (size_t)pgsz * 2, PROT_READ | PROT_EXEC);
    fprintf(stderr, "[input] patched %s @ %p\n", what, addr);
    return 1;
}

/* Screen contract, mirroring the VM reference (kairovm/unity.py): the
 * kairo-unity engine maps orientation through UnityEngine.Screen.get_orientation
 * (IApplication::GetOrientation @0x175b08c handles Unity values 2/3/4) and
 * auto-rotate through get_autorotateToPortrait (IsAutoOrientation @0x175b198).
 * A real Android device gives LandscapeLeft on a wide panel; with no Java
 * rotation pipeline our libunity answer is unreliable, so report it exactly
 * the way a tablet would: LandscapeLeft on this 640x480 landscape window,
 * panel dpi 160 (kairovm's value; Unity's Java-less default is 96, which
 * makes Kairosoft's dp-based layout look "wonky"). */
static int inp_Screen_get_orientation(void) {
    int w = egl_shim_screen_w(), h = egl_shim_screen_h();
    if (w <= 0) w = 640;
    if (h <= 0) h = 480;
    return w >= h ? 3 : 1;      /* 3 = LandscapeLeft, 1 = Portrait */
}
static float inp_Screen_get_dpi(void) { return 160.0f; }
static int inp_Screen_get_autorotateToPortrait(void) { return 0; }

static int g_patched, g_patch_retry;
static void try_install_hooks(void) {
    if (g_patched) return;
    if (!il_resolve()) return;
    static int pp;
    if (!pp) {
        pp = 1;
        fprintf(stderr, "[input] il2cpp runtime ready; installing input hooks\n");
    }
    void *k_input = il_find_class("CoreModule", "UnityEngine", "Input");
    if (!k_input) { g_patch_retry++; return; }
    void *k_unsafe = il_find_class("CoreModule", "UnityEngine.Internal",
                                   "InputUnsafeUtility");
    void *k_screen = il_find_class("CoreModule", "UnityEngine", "Screen");
    int got = 0, total = 0;
#define H(cls, mth, argc, body) do { \
        total++; \
        void *mi = il_class_get_method_from_name(cls, mth, argc); \
        if (mi && il_patch(mi, (void *)(body), mth)) got++; \
        else fprintf(stderr, "[input] MISS %s (mi=%p)\n", mth, mi); \
    } while (0)
    H(k_input, "GetKeyInt", 1, inp_GetKeyInt);
    H(k_input, "GetKeyDownInt", 1, inp_GetKeyDownInt);
    H(k_input, "GetKeyUpInt", 1, inp_GetKeyUpInt);
    H(k_input, "get_anyKey", 0, inp_anyKey);
    H(k_input, "get_anyKeyDown", 0, inp_anyKey);
    H(k_input, "get_mousePosition_Injected", 0, inp_get_mousePosition);
    H(k_input, "get_mouseScrollDelta_Injected", 0, inp_get_mouseScrollDelta);
    H(k_input, "GetMouseButton", 1, inp_GetMouseButton);
    H(k_input, "GetMouseButtonDown", 1, inp_GetMouseButtonDown);
    H(k_input, "GetMouseButtonUp", 1, inp_GetMouseButtonUp);
    H(k_input, "get_touchCount", 0, inp_get_touchCount);
    H(k_input, "get_touchSupported", 0, inp_get_touchSupported);
    H(k_input, "get_mousePresent", 0, inp_get_mousePresent);
    H(k_input, "get_multiTouchEnabled", 0, inp_get_multiTouchEnabled);
    H(k_input, "set_multiTouchEnabled", 1, inp_nop);
    H(k_input, "GetTouch_Injected", 1, inp_GetTouch);
    if (joyname_on()) {
        /* build the pinned ["Gamepad"] string[] lazily (needs string class) */
        if (!g_joystick_names && il_array_new && il_string_new && il_gchandle_new) {
            void *kstr = il_find_class("mscorlib", "System", "String");
            if (kstr) {
                void *arr = il_array_new(kstr, 1);
                void *s = il_string_new("Gamepad");
                if (arr && s) {
                    ((void **)((char *)arr + 0x20))[0] = s;
                    il_gchandle_new(arr, 1);
                    g_joystick_names = arr;
                }
            }
        }
        H(k_input, "GetJoystickNames", 0, inp_GetJoystickNames);
    }
    if (k_unsafe) {
        H(k_unsafe, "GetAxis", 1, inp_GetAxis);
        H(k_unsafe, "GetAxisRaw", 1, inp_GetAxis);
        H(k_unsafe, "GetButton", 1, inp_GetButton);
        H(k_unsafe, "GetButtonDown", 1, inp_GetButton);
        H(k_unsafe, "GetButtonUp", 1, inp_GetButton);
    }
    if (k_screen) {
        H(k_screen, "get_orientation", 0, inp_Screen_get_orientation);
        H(k_screen, "get_dpi", 0, inp_Screen_get_dpi);
        H(k_screen, "get_autorotateToPortrait", 0,
          inp_Screen_get_autorotateToPortrait);
    }
#undef H
    /* key snapshot baseline for the Down/Up edge icalls */
    for (int k = 0; k < 512; k++) g_key_prev[k] = inp_GetKeyInt(k);
    g_patched = 1;
    fprintf(stderr, "[input] input hooks installed (%d/%d)\n", got, total);
}

static void *inp_empty_string(void) { return NULL; }

/* ------------------------------------------------------- public hooks */
int gds_input_init(void) { return 0; }

void gds_input_poll(void *env, void *player, unsigned long frame) {
    (void)env; (void)player;
    if (!cursor_on() && frame > 5) return;
    pad_open();
    memcpy(g_key_prev, g_key_now, sizeof g_key_prev);
    pad_poll();
    frame_update();
    for (int k = 0; k < 512; k += 8) {
        /* cheap snapshot of the small key set we actually feed */
    }
    g_key_now[KC_UP] = inp_GetKeyInt(KC_UP);
    g_key_now[KC_DOWN] = inp_GetKeyInt(KC_DOWN);
    g_key_now[KC_LEFT] = inp_GetKeyInt(KC_LEFT);
    g_key_now[KC_RIGHT] = inp_GetKeyInt(KC_RIGHT);
    g_key_now[KC_ESCAPE] = inp_GetKeyInt(KC_ESCAPE);
    g_key_now[KC_RETURN] = inp_GetKeyInt(KC_RETURN);
    g_key_now[KC_SPACE] = inp_GetKeyInt(KC_SPACE);
    if (!g_patched && frame > 2) try_install_hooks();
}

void gds_input_close(void) {
    if (g_gc && s_GameControllerClose) s_GameControllerClose(g_gc);
    g_gc = NULL;
}

/* egl_shim cursor overlay state (drawn via raw GL right before present). */
int gds_input_cursor(float *x, float *y) {
    if (!cursor_on()) return 0;
    if (x) *x = g_cur_x;
    if (y) *y = g_cur_y;
    return 1;
}
int gds_input_cursor_pressed(void) { return g_press0; }
void gds_input_set_screen_size(int width, int height) {
    (void)width; (void)height;
}

/* soft keyboard: no-op stubs (Terraria OSK lands in the next build) */
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
