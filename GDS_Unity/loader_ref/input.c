/* input.c -- R36S gamepad -> UnityEngine.Input for Game Dev Story.
 *
 * GDS reads input ONLY through UnityEngine.Input (legacy): GetKey*,
 * GetAxis*, mousePosition/GetMouseButton*, touches.  On Android those are
 * filled by Unity's Java MotionEvent/InputDevice pipeline, which does not
 * exist under our loader -- so every read returns "nothing pressed".  We fix
 * that at the seam the reference ports use (terraria-nextos native_pad.c,
 * verbatim technique): replace managed method bodies with
 * `ldr x16,[pc,#8]; br x16` trampolines into this file.
 *
 * 0.81 ARCHITECTURE CHANGE (device-forced): 0.80.x located the managed
 * methods through the il2cpp RUNTIME API (il2cpp_class_from_name etc).  On
 * device, enumerating the il2cpp image list dies inside the runtime
 * (SIGSEGV NULL deref at class+0x135, pc=libil2cpp+0xcfccd4, twice).  So
 * this build NEVER calls an il2cpp export: method addresses are static per
 * libil2cpp build, extracted offline from global-metadata.dat, and patched
 * by absolute vaddr with a first-instruction signature check.  Table is
 * overridable per game: $gamedir/gds_hooks.cfg overrides any entry (that's
 * the future-port story -- new APK, new cfg, zero C changes).
 *
 * THE GAME TALKS GAMEPAD NATIVELY (device-verified by the user: Kairosoft
 * titles take gamepads on Android incl. L1/R1).  Two layers are fed:
 *   1. UnityEngine.Input key/axis/button icalls (menu navigation path):
 *      dpad->arrows + dpad1_* axes, A/B/X/Y/L1/R1/START/SELECT -> every
 *      plausible Unity KeyCode enumeration (kairo's stored keycode table is
 *      BSS in the binary; both Unity default and kairo slot order fed).
 *   2. kairo.unity.ui.Canvas joystick virtual buttons (the console-style
 *      pad path the game has built in): ids 0-3 A/B/X/Y, 6=L1 7=R1,
 *      8/9 L2/R2 analog, 10=SELECT 11=START, 12-15 dpad -- answered
 *      directly from SDL state, no Unity KeyCode translation at all.
 * Landscape comes from kairo.unity.ui.IApplication::IsSide (the
 * canvas-orientation oracle SurfaceManager::Setup consults at boot) patched
 * to return true, plus UnityEngine.Screen orientation/dpi answers.
 *
 * Switches (gds_env.cfg): GDS_INPUT=0 disables ALL patching (boots exactly
 * like 0.79 display-only), GDS_CURSOR=1 pointer overlay + A-as-touch,
 * GDS_ARROWS=0 stops feeding dpad/stick as arrow keys, GDS_HOOK_DELAY=N
 * waits N frames before patching (default 3).
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
    return s_InitSubSystem && s_GameControllerOpen && s_GameControllerGetButton;
}

/* ---------------------------------------------------------- pad state */
/* NPB_* button indices live in gds.h (shared with osk.c). */
enum { NPA_LX, NPA_LY, NPA_RX, NPA_RY, NPA_LT, NPA_RT, NPA_COUNT };

static unsigned char g_npb[NPB_COUNT];
static unsigned char g_npb_prev[NPB_COUNT];
static float g_npa[NPA_COUNT];
static SDL_GameController *g_gc;
static int g_sdl_inited, g_open_retry;

static volatile int g_exit_requested;
void gds_input_request_exit(void) { g_exit_requested = 1; }
int  gds_input_exit_requested(void) { return g_exit_requested; }

static int input_on(void) {
    static int c = -1;
    if (c < 0) {
        const char *e = getenv("GDS_INPUT");
        c = (e && atoi(e) == 0) ? 0 : 1;   /* GDS_INPUT=0: boot display-only */
    }
    return c;
}
static int cursor_on(void) {
    static int c = -1;
    if (c < 0) {
        const char *e = getenv("GDS_CURSOR");
        c = (e && atoi(e) > 0) ? 1 : 0;
    }
    return c;
}
static int arrows_on(void) {
    static int c = -1;
    if (c < 0) {
        const char *e = getenv("GDS_ARROWS");
        c = (e && atoi(e) == 0) ? 0 : 1;
    }
    return c;
}
static int hook_delay(void) {
    static int c = -1;
    if (c < 0) {
        const char *e = getenv("GDS_HOOK_DELAY");
        c = e ? atoi(e) : 0;   /* 0.82: no delay -- see gds_input_install_now */
        if (c < 0) c = 0;
    }
    return c;
}
static int swap_ab(void) {
    static int c = -1;
    if (c < 0) {
        const char *e = getenv("GDS_SWAPAB");
        c = (e && atoi(e) == 0) ? 0 : 1;
    }
    return c;
}
static int swap_xy(void) {
    static int c = -1;
    if (c < 0) {
        const char *e = getenv("GDS_SWAPXY");
        c = (e && atoi(e) == 0) ? 0 : 1;
    }
    return c;
}
static int padlog_on(void) {
    static int c = -1;
    if (c < 0) {
        const char *e = getenv("GDS_PADLOG");
        c = (e && atoi(e) > 0) ? 1 : 0;
    }
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
    memcpy(g_npb_prev, g_npb, sizeof g_npb);
#define BTN(b) (s_GameControllerGetButton ? s_GameControllerGetButton(g_gc, b) : 0)
    /* 0.82: Nintendo-face swap, default ON (GDS_SWAPAB=0 / GDS_SWAPXY=0 off).
     * Device evidence: SDL on ArkOS maps buttons by POSITION (SDL "A" = the
     * physical bottom button, labeled B on the R36S's SNES-style face).
     * kairo slot 0 is semantic "A" (Switch layout = east button).  Without
     * the swap the user confirmed with the button labeled B -- exactly what
     * the 0.81 device run reported.  Reference ports carry the same toggle
     * (TER_SWAPAB in terraria/horizonchase native_pad.c). */
    g_npb[NPB_A]     = BTN(swap_ab() ? SDL_CONTROLLER_BUTTON_B : SDL_CONTROLLER_BUTTON_A);
    g_npb[NPB_B]     = BTN(swap_ab() ? SDL_CONTROLLER_BUTTON_A : SDL_CONTROLLER_BUTTON_B);
    g_npb[NPB_X]     = BTN(swap_xy() ? SDL_CONTROLLER_BUTTON_Y : SDL_CONTROLLER_BUTTON_X);
    g_npb[NPB_Y]     = BTN(swap_xy() ? SDL_CONTROLLER_BUTTON_X : SDL_CONTROLLER_BUTTON_Y);
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

    /* GDS_PADLOG=1: every physical button transition, one line.  This is the
     * "controller test" the user asked for: press a button, read the line. */
    if (padlog_on()) {
        static const char *const bn[NPB_COUNT] = {
            "A/east", "B/south", "X/north", "Y/west", "L1", "R1",
            "SELECT", "START", "L3", "R3", "D-UP", "D-DOWN", "D-LEFT", "D-RIGHT" };
        for (int i = 0; i < NPB_COUNT; i++) {
            if (g_npb[i] != g_npb_prev[i]) {
                fprintf(stderr, "[padlog] %-8s %s  (LT=%.2f RT=%.2f)\n",
                        bn[i], g_npb[i] ? "DOWN" : "up",
                        g_npa[NPA_LT], g_npa[NPA_RT]);
                fflush(stderr);
            }
        }
    }

    /* exit chord: SELECT+START (native_pad.c Bully/Sonic pattern) */
    if (!g_exit_requested && g_npb[NPB_BACK] && g_npb[NPB_START]) {
        g_exit_requested = 1;
        fprintf(stderr, "[input] SELECT+START -> exit\n");
    }
}

/* ------------------------------------------------ first-hit diagnostics */
static unsigned long long g_hit;
#define HITF(bit, ...) do { \
    if (!(g_hit & (1ull << (bit)))) { \
        g_hit |= 1ull << (bit); \
        fprintf(stderr, "[input] first-hit: " __VA_ARGS__); \
        fputc('\n', stderr); \
    } \
} while (0)

/* ------------------------------------------------ virtual pointer state */
static float g_cur_x = 320.0f, g_cur_y = 240.0f;
static int g_press0, g_press1;
static int g_press0_edge_down, g_press0_edge_up;
static int g_press1_edge_down, g_press1_edge_up;
static int g_moved_this_frame;
static int g_prev_press0 = -1;
static int g_touch_ended_frame;

/* Keyboard feed, Unity KeyCode values */
#define KC_RETURN 13
#define KC_ESCAPE 27
#define KC_SPACE 32
#define KC_UP 273
#define KC_DOWN 274
#define KC_RIGHT 275
#define KC_LEFT 276

static int g_key_now[512], g_key_prev[512];
static const int g_snap_keys[] = {
    13, 27, 32, 273, 274, 275, 276,
    19, 20, 21, 22,
    96, 97, 99, 100, 102, 103, 104, 105, 106, 107, 108, 109, 110,
    350, 351, 352, 353, 354, 355, 356, 357, 358, 359, 360, 361, 362, 363,
};

static int key_from_pad(int key) {
    /* Unity JoystickButton0.. (KeyCode 350..): both Unity default Android
     * order and kairo's table-slot order fed (stored table is BSS). */
    switch (key) {
    case 350: return g_npb[NPB_A];
    case 351: return g_npb[NPB_B];
    case 352: return g_npb[NPB_X];
    case 353: return g_npb[NPB_Y];
    case 354: return g_npb[NPB_LB];
    case 355: return g_npb[NPB_RB];
    case 356: return g_npb[NPB_LB];   /* kairo-order L1 (Unity-order L2) */
    case 357: return g_npb[NPB_RB];   /* kairo-order R1 (Unity-order R2) */
    case 358: return g_npb[NPB_L3];
    case 359: return g_npb[NPB_R3];
    case 360: return g_npb[NPB_BACK]; /* kairo SELECT (Unity START) */
    case 361: return g_npb[NPB_START];/* kairo START (Unity SELECT) */
    case 362: return g_npb[NPB_L3];
    case 363: return g_npb[NPB_R3];
    }
    /* Raw Android keycodes */
    switch (key) {
    case 96:  return g_npb[NPB_A];
    case 97:  return g_npb[NPB_B];
    case 99:  return g_npb[NPB_X];
    case 100: return g_npb[NPB_Y];
    case 102: return g_npb[NPB_LB];
    case 103: return g_npb[NPB_RB];
    case 104: return g_npa[NPA_LT] > 0.5f;
    case 105: return g_npa[NPA_RT] > 0.5f;
    case 106: return g_npb[NPB_L3];
    case 107: return g_npb[NPB_R3];
    case 108: return g_npb[NPB_START];
    case 109: return g_npb[NPB_BACK];
    case 19:  return g_npb[NPB_DU];
    case 20:  return g_npb[NPB_DD];
    case 21:  return g_npb[NPB_DL];
    case 22:  return g_npb[NPB_DR];
    }
    if (key == KC_ESCAPE) return g_npb[NPB_B];
    if (key == KC_RETURN) return g_npb[NPB_A];
    if (key == KC_SPACE)  return g_npb[NPB_A];
    if (arrows_on()) {
        switch (key) {
        case KC_UP:    return g_npb[NPB_DU] || g_npa[NPA_LY] < -0.5f;
        case KC_DOWN:  return g_npb[NPB_DD] || g_npa[NPA_LY] > 0.5f;
        case KC_LEFT:  return g_npb[NPB_DL] || g_npa[NPA_LX] < -0.5f;
        case KC_RIGHT: return g_npb[NPB_DR] || g_npa[NPA_LX] > 0.5f;
        }
    }
    return 0;
}

/* kairo's virtual joystick contract -- 0.82 disassembly-verified:
 *   Canvas::GetJoystickButton @0x171d8d8: id%(24) indexes a KeyCode table;
 *   zero entries (ids 8/9, 12-15) fall back to GetJoystickAxis(id) > 0.5f.
 *   Canvas::_decideKeyState @0x17240c8 jump table @0x50cc0c (Unity arrow
 *   KeyCodes 273-276) defines the dpad semantics DEFINITIVELY:
 *     keycode 273 (UP)    = axis17 < -0.5 || GetJoystickButton(13)
 *     keycode 274 (DOWN)  = axis17 > +0.5 || GetJoystickButton(15)
 *     keycode 275 (RIGHT) = axis16 > +0.5 || GetJoystickButton(14)
 *     keycode 276 (LEFT)  = axis16 < -0.5 || GetJoystickButton(12)
 *   => slot order is 12=LEFT 13=UP 14=RIGHT 15=DOWN  (0.81 had 12/13/14/15
 *   = U/D/L/R -- wrong; user evidence "down was up, up dead" matches this).
 *   cctor @0x172d9a0 triples {flag,slot}: 0x1001-8->0-3 (A/B/X/Y),
 *   0x1010/20->6/7 (L1/R1), 0x1040/80->8/9 (L2/R2), 0x1100/200->10/11
 *   (SELECT/START), 0x10000..0x80000->12-15, 0x2000 group -> 16-23. */
static int kjoy_button(int id) {
    /* 0.84 OSK gate (Terraria ter_vkbd_blocking equivalent): while the
     * on-screen keyboard owns the pad the game must see NOTHING pressed. */
    if (gds_osk_active()) return 0;
    switch (id & 0xff) {
    case 0:  return g_npb[NPB_A];
    case 1:  return g_npb[NPB_B];
    case 2:  return g_npb[NPB_X];
    case 3:  return g_npb[NPB_Y];
    case 6:  return g_npb[NPB_LB];
    case 7:  return g_npb[NPB_RB];
    case 8:  return g_npa[NPA_LT] > 0.5f;
    case 9:  return g_npa[NPA_RT] > 0.5f;
    case 10: return g_npb[NPB_BACK];
    case 11: return g_npb[NPB_START];
    case 12: return g_npb[NPB_DL];   /* LEFT  */
    case 13: return g_npb[NPB_DU];   /* UP    */
    case 14: return g_npb[NPB_DR];   /* RIGHT */
    case 15: return g_npb[NPB_DD];   /* DOWN  */
    }
    return 0;
}
static float kjoy_axis_value(int id) {
    if (gds_osk_active()) return 0.0f;
    switch (id & 0xff) {
    case 8:  return g_npa[NPA_LT];
    case 9:  return g_npa[NPA_RT];
    case 12: return (float)g_npb[NPB_DL];
    case 13: return (float)g_npb[NPB_DU];
    case 14: return (float)g_npb[NPB_DR];
    case 15: return (float)g_npb[NPB_DD];
    case 16: {                              /* dpad horizontal, +1 = RIGHT */
        float h = (float)(g_npb[NPB_DR] - g_npb[NPB_DL]);
        return h ? h : g_npa[NPA_LX];
    }
    case 17: {                              /* dpad vertical, +1 = DOWN */
        float v = (float)(g_npb[NPB_DD] - g_npb[NPB_DU]);
        return v ? v : g_npa[NPA_LY];
    }
    }
    return 0.0f;
}
static unsigned char g_kjoy_now[24], g_kjoy_prev[24];

/* The axis-name contract for Input.GetAxis: dpad names first (they contain
 * the stick names as substrings).  First sightings are logged. */
static char g_seen_axes[8][64];
static int g_seen_axis_n;
static void note_axis_query(const char *name) {
    for (int i = 0; i < g_seen_axis_n; i++)
        if (!strcmp(g_seen_axes[i], name)) return;
    if (g_seen_axis_n < 8) {
        snprintf(g_seen_axes[g_seen_axis_n++], 64, "%s", name);
        fprintf(stderr, "[input] axis query: \"%s\"\n", name);
    }
}
static float axis_from_name(const char *raw) {
    if (!raw || !raw[0]) return 0.0f;
    char name[80];
    size_t n = strlen(raw);
    if (n >= sizeof name) n = sizeof name - 1;
    for (size_t i = 0; i < n; i++) {
        char ch = raw[i];
        name[i] = (ch >= 'A' && ch <= 'Z') ? (char)(ch + 32) : ch;
    }
    name[n] = 0;
    float lx = g_npa[NPA_LX], ly = g_npa[NPA_LY];
    float rx = g_npa[NPA_RX], ry = g_npa[NPA_RY];
    float dpad_h = (float)(g_npb[NPB_DR] - g_npb[NPB_DL]);
    float dpad_v = (float)(g_npb[NPB_DU] - g_npb[NPB_DD]);   /* up = +1 */
    if (strstr(name, "dpad")) {
        if (strstr(name, "horiz")) return dpad_h;
        if (strstr(name, "vert"))  return dpad_v;
        return 0.0f;
    }
    if (!strcmp(name, "horizontal")) {
        if (lx > 0.15f || lx < -0.15f) return lx;
        return dpad_h;
    }
    if (!strcmp(name, "vertical")) {
        if (ly > 0.15f || ly < -0.15f) return -ly;  /* Unity: up = +1 */
        return dpad_v;
    }
    if (!strncmp(name, "axis", 4)) {
        int ax = atoi(name + 4);
        switch (ax) {
        case 0: return lx;
        case 1: return -ly;
        case 2: return rx;
        case 3: return -ry;
        case 4: return g_npa[NPA_LT];
        case 5: return g_npa[NPA_RT];
        case 8:  return g_npa[NPA_LT];
        case 9:  return g_npa[NPA_RT];
        case 12: return dpad_h ? dpad_h : lx;
        case 13: return dpad_v ? dpad_v : -ly;
        }
        return 0.0f;
    }
    if (strstr(name, "trigger")) {
        if (strstr(name, "right") || strchr(name, 'r')) return g_npa[NPA_RT];
        return g_npa[NPA_LT];
    }
    if (strstr(name, "right_x") || strstr(name, "rightx")) return rx;
    if (strstr(name, "right_y") || strstr(name, "righty")) return -ry;
    if (strstr(name, "left_x") || strstr(name, "leftx")) return lx;
    if (strstr(name, "left_y") || strstr(name, "lefty")) return -ly;
    return 0.0f;
}

static void frame_update(void) {
    float dx = 0.0f, dy = 0.0f;
    if (cursor_on()) {
        float dz = 0.18f;
        float ax = g_npa[NPA_LX], ay = g_npa[NPA_LY];
        if (ax > dz || ax < -dz) dx = ax * ax * (ax > 0 ? 1.0f : -1.0f) * 9.0f;
        if (ay > dz || ay < -dz) dy = ay * ay * (ay > 0 ? 1.0f : -1.0f) * 9.0f;
        if (g_npb[NPB_DL]) dx -= 5.0f;
        if (g_npb[NPB_DR]) dx += 5.0f;
        if (g_npb[NPB_DU]) dy -= 5.0f;
        if (g_npb[NPB_DD]) dy += 5.0f;
    }
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

    if (g_press0 && g_prev_press0 != 1) {
        g_prev_press0 = 1;
        g_touch_ended_frame = 0;
    } else if (g_press0) {
        g_prev_press0 = 2;
    } else if (g_prev_press0 > 0) {
        g_prev_press0 = -2;
        g_touch_ended_frame = 1;
    } else {
        g_prev_press0 = 0;
        g_touch_ended_frame = 0;
    }
}

/* ---------------------------------------------- hook bodies (C ABI) */
static int inp_GetKeyInt(int key) {
    HITF(0, "Input.GetKeyInt");
    return key_from_pad(key);
}
static int inp_GetKeyDownInt(int key) {
    HITF(1, "Input.GetKeyDownInt");
    if (key < 0 || key >= 512) return 0;
    return g_key_now[key] && !g_key_prev[key];
}
static int inp_GetKeyUpInt(int key) {
    HITF(2, "Input.GetKeyUpInt");
    if (key < 0 || key >= 512) return 0;
    return !g_key_now[key] && g_key_prev[key];
}
static void ilstr_to_utf8(void *il2_string, char *out, size_t outsz) {
    out[0] = 0;
    if (!il2_string) return;
    int len = *(int *)((char *)il2_string + 0x10);
    const uint16_t *s = (const uint16_t *)((char *)il2_string + 0x14);
    if (len <= 0 || (size_t)len >= outsz) return;
    for (int i = 0; i < len; i++)
        out[i] = (char)(s[i] < 128 ? s[i] : '?');
    out[len] = 0;
}
static int key_from_string_name(const char *name) {
    if (!name || !name[0]) return 0;
    char low[64];
    size_t n = strlen(name);
    if (n >= sizeof low) n = sizeof low - 1;
    for (size_t i = 0; i < n; i++) {
        char ch = name[i];
        low[i] = (ch >= 'A' && ch <= 'Z') ? (char)(ch + 32) : ch;
    }
    low[n] = 0;
    if (!strcmp(low, "space"))  return g_npb[NPB_A];
    if (!strcmp(low, "return") || !strcmp(low, "enter")) return g_npb[NPB_A];
    if (!strcmp(low, "escape")) return g_npb[NPB_B];
    if (!strcmp(low, "up"))     return key_from_pad(KC_UP);
    if (!strcmp(low, "down"))   return key_from_pad(KC_DOWN);
    if (!strcmp(low, "left"))   return key_from_pad(KC_LEFT);
    if (!strcmp(low, "right"))  return key_from_pad(KC_RIGHT);
    if (!strcmp(low, "joystick button 0")) return g_npb[NPB_A];
    if (!strcmp(low, "joystick button 1")) return g_npb[NPB_B];
    if (!strcmp(low, "joystick button 2")) return g_npb[NPB_X];
    if (!strcmp(low, "joystick button 3")) return g_npb[NPB_Y];
    return 0;
}
static int inp_GetKeyString(void *il2_string) {
    HITF(3, "Input.GetKey(string)");
    char nm[64];
    ilstr_to_utf8(il2_string, nm, sizeof nm);
    return key_from_string_name(nm);
}
static int inp_GetKeyString_unmanaged(const char *utf8, int len) {
    HITF(3, "Input.GetKey(string)[u]");
    char nm[64];
    if (!utf8 || len <= 0 || len >= (int)sizeof nm) return 0;
    memcpy(nm, utf8, (size_t)len);
    nm[len] = 0;
    return key_from_string_name(nm);
}
static int inp_anyKey(void) {
    HITF(4, "Input.get_anyKey");
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
    HITF(5, "Input.GetMouseButton");
    if (btn == 0) return g_press0;
    if (btn == 1) return g_press1;
    return 0;
}
static int inp_GetMouseButtonDown(int btn) {
    HITF(6, "Input.GetMouseButtonDown");
    if (btn == 0) return g_press0_edge_down;
    if (btn == 1) return g_press1_edge_down;
    return 0;
}
static int inp_GetMouseButtonUp(int btn) {
    HITF(7, "Input.GetMouseButtonUp");
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
static float inp_GetAxis(void *il2_string) {
    HITF(9, "Input.GetAxis");
    char nm[80];
    ilstr_to_utf8(il2_string, nm, sizeof nm);
    note_axis_query(nm);
    return axis_from_name(nm);
}
static float inp_GetAxis_unmanaged(const char *utf8, int len) {
    HITF(10, "Input.GetAxis[u]");
    char nm[80];
    if (!utf8 || len <= 0 || len >= (int)sizeof nm) return 0.0f;
    memcpy(nm, utf8, (size_t)len);
    nm[len] = 0;
    note_axis_query(nm);
    return axis_from_name(nm);
}
static int button_from_name(const char *raw) {
    if (!raw || !raw[0]) return 0;
    char low[64];
    size_t n = strlen(raw);
    if (n >= sizeof low) n = sizeof low - 1;
    for (size_t i = 0; i < n; i++) {
        char ch = raw[i];
        low[i] = (ch >= 'A' && ch <= 'Z') ? (char)(ch + 32) : ch;
    }
    low[n] = 0;
    if (!strcmp(low, "fire1") || !strcmp(low, "jump") ||
        !strcmp(low, "submit")) return g_npb[NPB_A];
    if (!strcmp(low, "fire2") || !strcmp(low, "cancel"))
        return g_npb[NPB_B];
    if (!strcmp(low, "fire3")) return g_npb[NPB_X];
    return 0;
}
static int inp_GetButton(void *il2_string) {
    HITF(11, "Input.GetButton");
    char nm[64];
    ilstr_to_utf8(il2_string, nm, sizeof nm);
    return button_from_name(nm);
}
static int inp_GetButton_unmanaged(const char *utf8, int len) {
    HITF(12, "Input.GetButton[u]");
    char nm[64];
    if (!utf8 || len <= 0 || len >= (int)sizeof nm) return 0;
    memcpy(nm, utf8, (size_t)len);
    nm[len] = 0;
    return button_from_name(nm);
}

/* UnityEngine.Touch (Unity 2022 layout, 60 bytes) */
static void inp_GetTouch(int index, unsigned char *out) {
    (void)index;
    HITF(8, "Input.GetTouch");
    memset(out, 0, 60);
    int h = egl_shim_screen_h();
    if (h <= 0) h = 480;
    float ux = g_cur_x, uy = (float)h - 1.0f - g_cur_y;
    *(int *)(out + 0) = 0;
    *(float *)(out + 4) = ux;
    *(float *)(out + 8) = uy;
    *(float *)(out + 12) = ux;
    *(float *)(out + 16) = uy;
    int phase = 2;
    if (g_prev_press0 == 1) phase = 0;
    else if (g_prev_press0 == 2)
        phase = g_moved_this_frame ? 1 : 2;
    else if (g_touch_ended_frame) phase = 3;
    *(int *)(out + 40) = phase;
    *(int *)(out + 44) = 1;
    *(float *)(out + 28) = 1.0f / 60.0f;
    *(float *)(out + 32) = 1.0f;
    *(int *)(out + 36) = 0;
}

/* Screen contract: LandscapeLeft on the 640x480 panel, dpi 160, no
 * autorotate -- IApplication maps this through GetOrientation @0x175b08c
 * (handles Unity values 2/3/4). */
static int inp_Screen_get_orientation(void) {
    HITF(13, "Screen.get_orientation");
    int w = egl_shim_screen_w(), h = egl_shim_screen_h();
    if (w <= 0) w = 640;
    if (h <= 0) h = 480;
    return w >= h ? 3 : 1;      /* 3 = LandscapeLeft, 1 = Portrait */
}
static float inp_Screen_get_dpi(void) {
    HITF(14, "Screen.get_dpi");
    return 160.0f;
}
static int inp_Screen_get_autorotateToPortrait(void) { return 0; }
static int inp_Screen_get_fullScreen(void) { return 1; }
static void inp_Screen_set_orientation(int o) { (void)o; }
static void inp_Screen_RequestOrientation(int o) { (void)o; }

/* Per-(family,id) query counters: which kairo slot the game actually asks
 * for, printed once per pair (the "which button does it poll" evidence the
 * 0.81 run only gave us one axis id for). */
static unsigned g_kq[12][24];
static void kq_note(int fam, const char *fn, int id) {
    if (fam < 0 || fam >= 12 || id < 0 || id >= 24) return;
    if (!g_kq[fam][id]++) {
        fprintf(stderr, "[input] kjoy query: %s(%d)\n", fn, id);
        fflush(stderr);
    }
}
/* kairo.unity.ui.Canvas virtual joystick layer (2 args: this, id). */
static int kjoy_GetJoystickButton(void *self, int id) {
    (void)self;
    HITF(16, "Canvas.GetJoystickButton");
    kq_note(0, "GetJoystickButton", id);
    return kjoy_button(id);
}
static float kjoy_GetJoystickAxis(void *self, int id) {
    (void)self;
    HITF(17, "Canvas.GetJoystickAxis(%d)", id);
    kq_note(1, "GetJoystickAxis", id);
    return kjoy_axis_value(id);
}
static int kjoy_GetJoystickDown(void *self, int id) {
    (void)self;
    HITF(18, "Canvas.GetJoystickDown");
    kq_note(2, "GetJoystickDown", id);
    if (id < 0 || id >= 24) return 0;
    return g_kjoy_now[id] && !g_kjoy_prev[id];
}
static int kjoy_GetJoystickUp(void *self, int id) {
    (void)self;
    HITF(19, "Canvas.GetJoystickUp");
    kq_note(3, "GetJoystickUp", id);
    if (id < 0 || id >= 24) return 0;
    return !g_kjoy_now[id] && g_kjoy_prev[id];
}
static int kjoy_GetJoystickPress(void *self, int id) {
    (void)self;
    HITF(20, "Canvas.GetJoystickPress");
    kq_note(4, "GetJoystickPress", id);
    return kjoy_button(id);
}
static float kjoy_GetJoystickAnalog(void *self, int id) {
    (void)self;
    HITF(21, "Canvas.GetJoystickAnalog(%d)", id);
    kq_note(5, "GetJoystickAnalog", id);
    return kjoy_axis_value(id);
}
static float kjoy_GetJoystickAnalogPress(void *self, int id) {
    (void)self;
    HITF(22, "Canvas.GetJoystickAnalogPress(%d)", id);
    kq_note(6, "GetJoystickAnalogPress", id);
    return kjoy_axis_value(id);
}
static int kjoy_GetJoystickHoldDown(void *self, int id) {
    (void)self;
    HITF(23, "Canvas.GetJoystickHoldDown");
    kq_note(7, "GetJoystickHoldDown", id);
    return kjoy_button(id);
}

/* ------------------------------------------------- address hook table */
/* role -> handler.  vaddr/sig verified against GDS 2.6.9 libil2cpp.so
 * (extracted with tools/symbols.py; first instructions read from the file).
 * $gamedir/gds_hooks.cfg may override any entry: "role=vaddr[:sig]" */
typedef struct {
    const char *role;
    uint32_t vaddr;
    uint32_t sig;
    int kind;          /* 0=trampoline, 1=return 1, 2=return 0 */
    const void *fn;
} hook_ent;

static hook_ent g_hooks[] = {
    { "kairo.IsSide",          0x175aeb0, 0xf81e0ffe, 1, NULL },
    { "Input.GetKeyInt",       0x1bf3d94, 0xf81e0ffe, 0, inp_GetKeyInt },
    { "Input.GetKeyDownInt",   0x1bf3e0c, 0xf81e0ffe, 0, inp_GetKeyDownInt },
    { "Input.GetKeyUpInt",     0x1bf3dd0, 0xf81e0ffe, 0, inp_GetKeyUpInt },
    { "Input.get_anyKey",      0x1bf4294, 0xa9bf4ffe, 0, inp_anyKey },
    { "Input.GetAxis",         0x1bf3c2c, 0xf81e0ffe, 0, inp_GetAxis },
    { "Input.GetAxisRaw",      0x1bf3ca4, 0xf81e0ffe, 0, inp_GetAxis },
    { "Input.GetButtonDown",   0x1bf3d1c, 0xf81e0ffe, 0, inp_GetButton },
    { "Input.GetMouseButton",  0x1bf3e48, 0xf81e0ffe, 0, inp_GetMouseButton },
    { "Input.GetMouseButtonDown", 0x1bf3e84, 0xf81e0ffe, 0, inp_GetMouseButtonDown },
    { "Input.GetMouseButtonUp", 0x1bf3ec0, 0xf81e0ffe, 0, inp_GetMouseButtonUp },
    { "Input.get_mousePosition_Injected", 0x1bf4330, 0xf81e0ffe, 0, inp_get_mousePosition },
    { "Input.get_mouseScrollDelta_Injected", 0x1bf43b0, 0xf81e0ffe, 0, inp_get_mouseScrollDelta },
    { "Input.GetTouch",        0x1bf3efc, 0xd101c3ff, 0, inp_GetTouch },
    { "Input.GetTouch_Injected", 0x1bf3f6c, 0xa9be57fe, 0, inp_GetTouch },
    { "Input.get_touchCount",  0x1bf459c, 0xa9bf4ffe, 0, inp_get_touchCount },
    { "Input.get_touchSupported", 0x1bf45c4, 0xa9bf4ffe, 0, inp_get_touchSupported },
    { "Input.get_mousePresent", 0x1bf4574, 0xa9bf4ffe, 0, inp_get_mousePresent },
    { "Input.get_multiTouchEnabled", 0x1bf45ec, 0xa9bf4ffe, 0, inp_get_multiTouchEnabled },
    { "Input.set_multiTouchEnabled", 0x1bf4614, 0xf81e0ffe, 0, inp_nop },   /* sig corrected 0.82 (0.81 SIG MISMATCH evidence) */
    { "Unsafe.GetAxis",        0x1bf3c68, 0xf81e0ffe, 0, inp_GetAxis },
    { "Unsafe.GetAxisRaw",     0x1bf3ce0, 0xf81e0ffe, 0, inp_GetAxis },
    { "Unsafe.GetAxis__Unmanaged", 0x1bf5910, 0xa9be57fe, 0, inp_GetAxis_unmanaged },
    { "Unsafe.GetAxisRaw__Unmanaged", 0x1bf5954, 0xa9be57fe, 0, inp_GetAxis_unmanaged },
    { "Unsafe.GetButton__Unmanaged", 0x1bf5998, 0xa9be57fe, 0, inp_GetButton_unmanaged },
    { "Unsafe.GetButtonDown",  0x1bf3d58, 0xf81e0ffe, 0, inp_GetButton },
    { "Unsafe.GetButtonDown__Unmanaged", 0x1bf59dc, 0xa9be57fe, 0, inp_GetButton_unmanaged },
    { "Unsafe.GetButtonUp__Unmanaged", 0x1bf5a20, 0xa9be57fe, 0, inp_GetButton_unmanaged },
    { "Unsafe.GetKeyString",   0x1bf40f0, 0xf81e0ffe, 0, inp_GetKeyString },
    { "Unsafe.GetKeyString__Unmanaged", 0x1bf5844, 0xa9be57fe, 0, inp_GetKeyString_unmanaged },
    { "Unsafe.GetKeyDownString", 0x1bf4258, 0xf81e0ffe, 0, inp_GetKeyString },
    { "Unsafe.GetKeyDownString__Unmanaged", 0x1bf58cc, 0xa9be57fe, 0, inp_GetKeyString_unmanaged },
    { "Unsafe.GetKeyUpString", 0x1bf41a4, 0xf81e0ffe, 0, inp_GetKeyString },
    { "Unsafe.GetKeyUpString__Unmanaged", 0x1bf5888, 0xa9be57fe, 0, inp_GetKeyString_unmanaged },
    { "Screen.get_orientation", 0x1b9e564, 0xa9bf4ffe, 0, inp_Screen_get_orientation },
    { "Screen.GetScreenOrientation", 0x1b9e53c, 0xa9bf4ffe, 0, inp_Screen_get_orientation },
    { "Screen.set_orientation", 0x1b9e58c, 0xf81e0ffe, 0, inp_Screen_set_orientation },
    { "Screen.RequestOrientation", 0x1b9e500, 0xf81e0ffe, 0, inp_Screen_RequestOrientation },
    { "Screen.get_autorotateToPortrait", 0x1b9e69c, 0xa9bf4ffe, 0, inp_Screen_get_autorotateToPortrait },
    { "Screen.get_dpi",        0x1b9e4d8, 0xa9bf4ffe, 0, inp_Screen_get_dpi },
    { "Screen.get_fullScreen", 0x1b9e6cc, 0xa9bf4ffe, 0, inp_Screen_get_fullScreen },
    { "kairo.JoyButton",       0x171d8d8, 0xa9be57fe, 0, kjoy_GetJoystickButton },
    { "kairo.JoyAxis",         0x171d9c0, 0xd10103ff, 0, kjoy_GetJoystickAxis },
    { "kairo.JoyDown",         0x171e428, 0xa9be57fe, 0, kjoy_GetJoystickDown },
    { "kairo.JoyUp",           0x171e500, 0xa9be57fe, 0, kjoy_GetJoystickUp },
    { "kairo.JoyPress",        0x171dfa4, 0xa9be57fe, 0, kjoy_GetJoystickPress },
    { "kairo.JoyAnalog",       0x171e5d8, 0x6dbc23e9, 0, kjoy_GetJoystickAnalog },
    { "kairo.JoyAnalogPress",  0x171e8ac, 0xf81d0ffe, 0, kjoy_GetJoystickAnalogPress },
    { "kairo.JoyHoldDown",     0x171e680, 0xd10103ff, 0, kjoy_GetJoystickHoldDown },
};

static void load_hooks_cfg(void) {
    char path[1200];
    snprintf(path, sizeof path, "%s/gds_hooks.cfg", gds_gamedir);
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        char *eq = strchr(line, '=');
        if (!eq || line[0] == '#') continue;
        *eq = 0;
        unsigned va = 0, sg = 0;
        int nread = sscanf(eq + 1, "%x:%x", &va, &sg);
        if (!nread) continue;
        if (nread == 1) sg = 0;
        size_t rl = strlen(line);
        while (rl && (line[rl-1] == ' ' || line[rl-1] == '\t')) line[--rl] = 0;
        for (size_t i = 0; i < sizeof g_hooks / sizeof *g_hooks; i++) {
            if (!strcmp(g_hooks[i].role, line)) {
                g_hooks[i].vaddr = va;
                g_hooks[i].sig = sg;
                fprintf(stderr, "[input] cfg override %s -> 0x%x\n", line, va);
            }
        }
    }
    fclose(f);
    fprintf(stderr, "[input] hook table overrides loaded from gds_hooks.cfg\n");
}

static int patch_one(const hook_ent *he, uint8_t *base) {
    uint8_t *addr = base + he->vaddr;
    if (he->sig && *(uint32_t *)addr != he->sig) {
        fprintf(stderr, "[input] SIG MISMATCH %-36s @0x%x have 0x%08x want 0x%08x -- skipped\n",
                he->role, he->vaddr, *(uint32_t *)addr, he->sig);
        return 0;
    }
    long pgsz = sysconf(_SC_PAGESIZE);
    uintptr_t page = (uintptr_t)addr & ~((uintptr_t)pgsz - 1);
    if (mprotect((void *)page, (size_t)pgsz * 2,
                 PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
        fprintf(stderr, "[input] mprotect %s: %s\n", he->role, strerror(errno));
        return 0;
    }
    uint32_t *code = (uint32_t *)addr;
    switch (he->kind) {
    case 1:                              /* return true/1  */
        code[0] = 0x52800020u;           /* mov w0, #1 */
        code[1] = 0xD65F03C0u;           /* ret */
        break;
    case 2:                              /* return false/0 */
        code[0] = 0x52800000u;           /* mov w0, #0 */
        code[1] = 0xD65F03C0u;           /* ret */
        break;
    default:                             /* trampoline */
        code[0] = 0x58000050u;           /* ldr x16, [pc, #8] */
        code[1] = 0xD61F0200u;           /* br x16 */
        *(uint64_t *)(code + 2) = (uint64_t)(uintptr_t)he->fn;
        break;
    }
    __builtin___clear_cache((char *)addr, (char *)addr + 16);
    mprotect((void *)page, (size_t)pgsz * 2, PROT_READ | PROT_EXEC);
    return 1;
}

static int g_patched;
static int install_hooks(void) {
    if (g_patched) return 1;
    if (!input_on()) {
        g_patched = 1;
        fprintf(stderr, "[input] GDS_INPUT=0 -- no hooks (0.79 display-only mode)\n");
        return 0;
    }
    static int cfg_done;
    if (!cfg_done) { cfg_done = 1; load_hooks_cfg(); }
    nx_mod *mod = nx_find_mod("libil2cpp.so");
    if (!mod || !mod->base) return 0;
    int got = 0, total = 0;
    for (size_t i = 0; i < sizeof g_hooks / sizeof *g_hooks; i++) {
        total++;
        if (patch_one(&g_hooks[i], mod->base)) got++;
    }
    g_patched = 1;
    fprintf(stderr, "[input] input hooks installed (%d/%d, address-table, no il2cpp API)\n",
            got, total);
    return 1;
}

/* ------------------------------------------------------- public hooks */
int gds_input_init(void) { return 0; }

/* 0.82 landscape fix: main.c calls this the moment all modules are mapped
 * (right after "modules loaded"), NOT on frame N.  The address-table patch
 * needs zero il2cpp runtime, and SurfaceManager::Setup consults IsSide at
 * boot (frames 1-3, evidence: main.AppData::Init -> Setup -> IsSide) -- the
 * 0.81 frame-3 install landed AFTER the consult, so landscape never latched
 * even though the patch itself was correct. */
void gds_input_install_now(void) {
    install_hooks();
}

void gds_input_poll(void *env, void *player, unsigned long frame) {
    (void)env; (void)player;
    pad_open();
    memcpy(g_key_prev, g_key_now, sizeof g_key_prev);
    pad_poll();
    frame_update();
    /* 0.84 OSK ownership of the pad (Terraria ter_vkbd_blocking gate):
     * while the keyboard is open the game sees zero buttons/keys, and the
     * 18-frame swallow after close keeps the confirming press from leaking
     * back into the game as a phantom edge (Terraria g_vkbd_swallow). */
    static int osk_was, osk_swallow;
    int osk_active = gds_osk_active();
    if (osk_was && !osk_active) osk_swallow = 18;
    osk_was = osk_active;
    if (osk_active)
        gds_osk_pad_tick(g_npb, g_npb_prev);
    if (osk_active || osk_swallow > 0) {
        if (osk_swallow > 0) osk_swallow--;
        memset(g_key_now, 0, sizeof g_key_now);
        memcpy(g_kjoy_prev, g_kjoy_now, sizeof g_kjoy_prev);
        memset(g_kjoy_now, 0, sizeof g_kjoy_now);
    } else {
        for (size_t i = 0; i < sizeof g_snap_keys / sizeof *g_snap_keys; i++)
            g_key_now[g_snap_keys[i]] = key_from_pad(g_snap_keys[i]);
        memcpy(g_kjoy_prev, g_kjoy_now, sizeof g_kjoy_prev);
        for (size_t i = 0; i < sizeof g_kjoy_now / sizeof *g_kjoy_now; i++)
            g_kjoy_now[i] = (unsigned char)kjoy_button((int)i);
    }
    /* fallback only: normally installed by gds_input_install_now at load */
    if (!g_patched && (long)frame >= hook_delay()) {
        install_hooks();
        if (!g_patched && frame > 0 && frame % 600 == 0)
            fprintf(stderr, "[input] hooks not installed yet (frame %lu)\n", frame);
    }
    /* periodic kjoy query histogram (every 1200 frames): what the game asks */
    if (frame > 0 && frame % 1200 == 0) {
        static const char *const fam[8] = { "Btn", "Axis", "Down", "Up",
                                            "Press", "Ana", "AnaP", "Hold" };
        char line[512]; int p = 0;
        p += snprintf(line + p, sizeof line - p, "[input] kjoy hit-summary:");
        for (int f = 0; f < 8; f++) {
            int first = 1;
            p += snprintf(line + p, sizeof line - p, " %s[", fam[f]);
            for (int i = 0; i < 24; i++)
                if (g_kq[f][i]) {
                    p += snprintf(line + p, sizeof line - p, "%s%d:%u",
                                  first ? "" : " ", i, g_kq[f][i]);
                    first = 0;
                }
            p += snprintf(line + p, sizeof line - p, "]");
        }
        fprintf(stderr, "%s\n", line);
        fflush(stderr);
    }
}

void gds_input_close(void) {
    if (g_gc && s_GameControllerClose) s_GameControllerClose(g_gc);
    g_gc = NULL;
}

/* egl_shim cursor overlay state (only when GDS_CURSOR=1). */
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

/* soft keyboard: Unity's showSoftInput path and the kairo FEP panel share
 * the same OSK (osk.c, Terraria controller-keyboard port). */
void gds_input_keyboard_open(const char *initial, int character_limit) {
    gds_osk_open(NULL, initial, character_limit); }
void gds_input_keyboard_set(const char *text) { gds_osk_set_text(text); }
void gds_input_keyboard_hide(void) { gds_osk_hide(); }
int gds_input_keyboard_snapshot(char *text, size_t text_size,
                                int *uppercase, int *selected,
                                const gds_keyboard_key **keys, size_t *key_count) {
    (void)text; (void)text_size; (void)uppercase; (void)selected;
    if (keys) *keys = NULL; if (key_count) *key_count = 0;
    return 0;
}
