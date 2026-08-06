/* input.c -- R36S gamepad -> UnityEngine.Input for Game Dev Story.
 *
 * GDS reads input ONLY through UnityEngine.Input (legacy): GetKey*,
 * GetAxis*, mousePosition/GetMouseButton*, touches.  On Android those are
 * filled by Unity's Java MotionEvent/InputDevice pipeline, which does not
 * exist under our loader -- so every read returns "nothing pressed".  We fix
 * that at the seam the reference ports use (terraria-nextos native_pad.c,
 * verbatim technique): resolve the managed methods BY NAME through the
 * il2cpp runtime exports and replace their bodies with
 * `ldr x16,[pc,#8]; br x16` trampolines into this file.
 *
 * THE GAME TALKS GAMEPAD NATIVELY (device-verified by the user: Kairosoft
 * titles take gamepads on Android incl. L1/R1).  Disassembly contract:
 *   kairo.unity.ui.Canvas::GetJoystickButton(id):
 *     - keycode table ids 0-3 = A/B/X/Y, 6=L1, 7=R1, 10=SELECT, 11=START
 *       (tail-calls UnityEngine.Input::GetKey with a stored Unity KeyCode;
 *        stored values are BSS in the binary so we feed BOTH plausible
 *        enumerations: Unity default order 350+ and kairo's table order)
 *     - ids 8/9 = L2/R2 trigger AXES, ids 12-15 = dpad directions as AXES
 *       (answered when the axis value passes 0.5)
 *   kairo.unity.ui.Canvas::GetJoystickAxis(id) -> Input.GetAxis(<cached
 *   name>); axis-name strings seen in metadata: dpad1_horizontal,
 *   dpad1_vertical, axis12, axis13, Horizontal, Vertical,
 *   joystick1_right_trigger, TriggerL, TriggerR -- all answered by name.
 *
 * 0.80 fixes over 0.79 (all evidence-backed):
 *   - UnityEngine.Input/InputUnsafeUtility live in
 *     UnityEngine.InputLegacyModule.dll, NOT CoreModule -- 0.79's
 *     il_find_class("CoreModule", ...) returned NULL and bailed silently, so
 *     NO hook ever installed (device log: "installing input hooks" with no
 *     "installed (N/M)" line).  Classes are now located across ALL images.
 *   - il2cpp icall arities for 2022.3 fixed against metadata:
 *     GetTouch_Injected takes 2 args (index, out Touch),
 *     get_mousePosition_Injected/get_mouseScrollDelta_Injected take 1 (out),
 *     Input has NO get_anyKeyDown and NO GetJoystickNames in this build
 *     (stripped), InputUnsafeUtility's button icalls are the __Unmanaged
 *     (byte*,int) variants.
 *   - Hooks install on the FIRST poll (before frame 0 renders) so the
 *     managed boot sees everything; 0.79 waited for frame>2 which was after
 *     SurfaceManager::Setup latched the portrait canvas (frames 1-3).
 *   - Landscape: kairo.common.cfg.Config.LANDSCAPE_GAME (static bool, field
 *     #38, read by IApplication::IsSide @0x175aeb0 -- the canvas-orientation
 *     oracle SurfaceManager::Setup consults at boot) is set to TRUE via the
 *     il2cpp runtime before frame 0.  Old targets (GetRotateCheck, the
 *     IsTablet cache site, CheckNativeRotation) are dead code: BL-xref scan
 *     proved zero callers.
 *   - The virtual cursor is OFF by default (GDS_CURSOR=1 re-enables); the
 *     pad feeds as a pad, not a pointer.
 *
 * Switches (gds_env.cfg): GDS_CURSOR=1 pointer overlay + A-as-touch,
 * GDS_ARROWS=0 stops feeding dpad/stick as arrow keys, GDS_LANDSCAPE=0
 * skips the LANDSCAPE_GAME write, GDS_KEYLOG=1 makes first-hit/unknown-name
 * logging verbose (default logs each family once).
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
        c = (e && atoi(e) > 0) ? 1 : 0;    /* 0.80: native pad default */
    }
    return c;
}
static int arrows_on(void) {
    static int c = -1;
    if (c < 0) {
        const char *e = getenv("GDS_ARROWS");
        c = (e && atoi(e) == 0) ? 0 : 1;   /* arrows default ON */
    }
    return c;
}
static int landscape_on(void) {
    static int c = -1;
    if (c < 0) {
        const char *e = getenv("GDS_LANDSCAPE");
        c = (e && atoi(e) == 0) ? 0 : 1;   /* LANDSCAPE_GAME write default ON */
    }
    return c;
}
static int keylog_on(void) {
    static int c = -1;
    if (c < 0) c = getenv("GDS_KEYLOG") && atoi(getenv("GDS_KEYLOG")) > 0;
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

/* ------------------------------------------------ first-hit diagnostics */
static unsigned long long g_hit;
#define HITF(bit, ...) do { \
    if (!(g_hit & (1ull << (bit)))) { \
        g_hit |= 1ull << (bit); \
        fprintf(stderr, "[input] first-hit: " __VA_ARGS__); \
        fputc('\n', stderr); \
    } \
} while (0)

/* unknown-name spotting for the axis/button string contracts */
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
static char g_seen_btns[8][64];
static int g_seen_btn_n;
static void note_button_query(const char *name) {
    for (int i = 0; i < g_seen_btn_n; i++)
        if (!strcmp(g_seen_btns[i], name)) return;
    if (g_seen_btn_n < 8) {
        snprintf(g_seen_btns[g_seen_btn_n++], 64, "%s", name);
        fprintf(stderr, "[input] button query: \"%s\"\n", name);
    }
}
static void note_string_key(const char *name) {
    if (keylog_on())
        fprintf(stderr, "[input] string-key query: \"%s\"\n", name);
}

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

/* The pad->logical-KeyCode answer.  Centralised so every icall (GetKeyInt,
 * Down/Up edges, string keys) agrees. */
static int key_from_pad(int key) {
    /* Unity JoystickButton0.. (KeyCode 350..).  kairo's pad table stores one
     * Unity KeyCode per table slot; the values are BSS in the binary so we
     * feed both the Unity default Android order and kairo's own slot order:
     *   kairo ids: 0-3 A/B/X/Y, 6=L1 7=R1, 10=SELECT 11=START
     *   Unity default: 0=A 1=B 2=X 3=Y, 4=L1 5=R1, 6=L2 7=R2,
     *                  8=THUMBL 9=THUMBR, 10=START 11=SELECT */
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
    /* Raw Android keycodes (kairo metadata keeps an AndroidKeyCode table;
     * Unity also delivers dpad key events this way). */
    switch (key) {
    case 96:  return g_npb[NPB_A];       /* KEYCODE_BUTTON_A */
    case 97:  return g_npb[NPB_B];
    case 99:  return g_npb[NPB_X];
    case 100: return g_npb[NPB_Y];
    case 102: return g_npb[NPB_LB];      /* KEYCODE_BUTTON_L1 */
    case 103: return g_npb[NPB_RB];
    case 104: return g_npa[NPA_LT] > 0.5f; /* KEYCODE_BUTTON_L2 */
    case 105: return g_npa[NPA_RT] > 0.5f;
    case 106: return g_npb[NPB_L3];
    case 107: return g_npb[NPB_R3];
    case 108: return g_npb[NPB_START];   /* KEYCODE_BUTTON_START */
    case 109: return g_npb[NPB_BACK];    /* KEYCODE_BUTTON_SELECT */
    case 19:  return g_npb[NPB_DU];      /* KEYCODE_DPAD_UP */
    case 20:  return g_npb[NPB_DD];
    case 21:  return g_npb[NPB_DL];
    case 22:  return g_npb[NPB_DR];
    }
    /* Keyboard conveniences (desktop-style forms read these too) */
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

/* The axis-name contract (Canvas::GetJoystickAxis names come from two
 * 14-entry tables chosen by Config.LANDSCAPE_GAME; payloads unresolvable
 * offline, so answer every plausible name).  All names are also logged on
 * first sight so the next build can tighten this to exact strings. */
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
    /* dpad names first (they contain the stick names as substrings) */
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
    if (!strncmp(name, "axis", 4)) {                 /* axis8..axis15 family */
        int ax = atoi(name + 4);
        switch (ax) {
        case 8:  case 9:  case 12: case 13:
            /* dpad/trigger generic slots in the kairo tables: answer with the
             * strongest digital content of that family */
            if (ax == 8 || ax == 9)
                return g_npa[ax == 8 ? NPA_LT : NPA_RT];
            return ax == 12 ? (float)(dpad_h ? dpad_h : lx)
                            : (float)(dpad_v ? dpad_v : -ly);
        case 0: return lx;
        case 1: return -ly;
        case 2: return rx;
        case 3: return -ry;
        case 4: return g_npa[NPA_LT];
        case 5: return g_npa[NPA_RT];
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

/* ---------------------------------------------- icall bodies (C ABI) */
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

/* string-key icalls (InputUnsafeUtility GetKeyString family).  The kairo
 * helper may go through UnityEngine.Input::GetKey(string). */
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
    note_string_key(name);
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

/* UnityEngine.Touch (Unity 2022 layout, 60 bytes per kairovm's buffer) */
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
    *(int *)(out + 44) = 1;                  /* tapCount */
    *(float *)(out + 28) = 1.0f / 60.0f;     /* deltaTime */
    *(float *)(out + 32) = 1.0f;             /* pressure */
    *(int *)(out + 36) = 0;                  /* type: Direct */
}

/* get_inputString: cache one empty Il2CppString at install time. */
static void *g_empty_ilstr;
static void *inp_get_inputString(void) { return g_empty_ilstr; }

/* GetAxis family: Il2CppString* managed entry + (byte*,int) __Unmanaged icall */
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
    note_button_query(nm);
    return button_from_name(nm);
}
static int inp_GetButton_unmanaged(const char *utf8, int len) {
    HITF(12, "Input.GetButton[u]");
    char nm[64];
    if (!utf8 || len <= 0 || len >= (int)sizeof nm) return 0;
    memcpy(nm, utf8, (size_t)len);
    nm[len] = 0;
    note_button_query(nm);
    return button_from_name(nm);
}

/* ----------------------------------------------- il2cpp method patching */
static void *g_il2_mod;
static void *(*il_domain_get)(void);
static const void **(*il_domain_get_assemblies)(void *, size_t *);
static void *(*il_assembly_get_image)(const void *);
static const char *(*il_image_get_name)(const void *);
static void *(*il_class_from_name)(void *, const char *, const char *);
static void *(*il_class_get_method_from_name)(void *, const char *, int);
static void *(*il_class_get_fields)(void *, void **);
static const char *(*il_field_get_name)(void *);
static void (*il_field_static_get_value)(void *, void *);
static void (*il_field_static_set_value)(void *, const void *);
static void *(*il_string_new)(const char *);
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
    R(il_class_get_fields, "il2cpp_class_get_fields");
    R(il_field_get_name, "il2cpp_field_get_name");
    R(il_field_static_get_value, "il2cpp_field_static_get_value");
    R(il_field_static_set_value, "il2cpp_field_static_set_value");
    R(il_string_new, "il2cpp_string_new");
#undef R
    g_il_ready = il_domain_get && il_domain_get_assemblies &&
                 il_assembly_get_image && il_class_from_name &&
                 il_class_get_method_from_name;
    return g_il_ready;
}

/* Locate (ns, name) in the FIRST image whose name matches one of the
 * candidates (list of substring markers, NULL-terminated).  0.79's blind
 * CoreModule-only filter missed UnityEngine.Input (it lives in
 * UnityEngine.InputLegacyModule.dll in this 2022.3 player -- verified in
 * global-metadata), but 0.80's naive scan of every image died inside the
 * il2cpp runtime (SIGSEGV class+0x135 NULL deref on some image at frame 0 --
 * loader.log crash dump).  So: name-filtered scans only, evidence-based
 * image names, and step logging so any future fault is attributable from
 * port_launch.log. */
static void *il_find_class_in(const char *const *candidates, const char *ns,
                              const char *name, const char **out_image,
                              int step_log) {
    void *dom = il_domain_get();
    if (!dom) return NULL;
    if (step_log) fprintf(stderr, "[input] step: domain_get_assemblies\n");
    size_t n = 0;
    const void **asms = il_domain_get_assemblies(dom, &n);
    if (step_log) fprintf(stderr, "[input] step: %zu assemblies\n", n);
    for (int c = 0; candidates[c]; c++) {
        for (size_t i = 0; asms && i < n; i++) {
            void *img = il_assembly_get_image(asms[i]);
            if (!img) continue;
            if (step_log && c == 0)
                fprintf(stderr, "[input] step: image_get_name(%zu/%zu)\n", i, n);
            const char *in = il_image_get_name ? il_image_get_name(img) : NULL;
            if (step_log && c == 0)
                fprintf(stderr, "[input] step:   image #%zu = '%s'\n",
                        i, in ? in : "(null)");
            if (!in || !strstr(in, candidates[c])) continue;
            if (step_log)
                fprintf(stderr, "[input] step: class_from_name(%s, %s, %s)\n",
                        in, ns, name);
            void *k = il_class_from_name(img, ns, name);
            if (k) {
                if (out_image) *out_image = in;
                return k;
            }
        }
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
    return 1;
}

/* Screen contract, mirroring the VM reference (kairovm/unity.py): the
 * kairo-unity engine maps orientation through UnityEngine.Screen.get_orientation
 * (IApplication::GetOrientation @0x175b08c handles Unity values 2/3/4) and
 * auto-rotate through get_autorotateToPortrait (IsAutoOrientation @0x175b198).
 * A real Android device gives LandscapeLeft on a wide panel; with no Java
 * rotation pipeline our libunity answer is unreliable, so report it exactly
 * the way a tablet would: LandscapeLeft on this 640x480 landscape window,
 * panel dpi 160 (kairovm's value). */
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

/* kairo.common.cfg.Config.LANDSCAPE_GAME -- the flag behind
 * IApplication::IsSide @0x175aeb0, the canvas-orientation oracle that
 * SurfaceManager::Setup consults at boot (frames 1-3).  Metadata: field #38,
 * static bool.  il2cpp_field_from_name is NOT exported by this runtime, so
 * walk the field list.  cfg.MyConfig's ctor/cctor chain does not write this
 * field (verified @0xef9888/0xef98e0) so our value survives. */
static void set_landscape_game(void) {
    if (!landscape_on()) {
        fprintf(stderr, "[input] GDS_LANDSCAPE=0 -- leaving LANDSCAPE_GAME alone\n");
        return;
    }
    if (!il_class_get_fields || !il_field_get_name ||
        !il_field_static_set_value) {
        fprintf(stderr, "[input] LANDSCAPE_GAME: field APIs missing\n");
        return;
    }
    static const char *const cfg_imgs[] = { "KairoLibrary", NULL };
    const char *img = "?";
    void *kcfg = il_find_class_in(cfg_imgs, "kairo.common.cfg", "Config",
                                  &img, 1);
    if (!kcfg) {
        fprintf(stderr, "[input] LANDSCAPE_GAME: kairo.common.cfg.Config NOT FOUND\n");
        return;
    }
    fprintf(stderr, "[input] step: walk Config fields (%s)\n", img);
    void *field = NULL;
    void *iter = NULL;
    for (void *f = il_class_get_fields(kcfg, &iter); f;
         f = il_class_get_fields(kcfg, &iter)) {
        const char *fn = il_field_get_name(f);
        if (fn && !strcmp(fn, "LANDSCAPE_GAME")) { field = f; break; }
    }
    if (!field) {
        fprintf(stderr, "[input] LANDSCAPE_GAME: field not found in Config\n");
        return;
    }
    unsigned char before = 0xee, after = 0xee, one = 1;
    if (il_field_static_get_value)
        il_field_static_get_value(field, &before);
    fprintf(stderr, "[input] step: LANDSCAPE_GAME static_set_value (before=%u)\n",
            before);
    il_field_static_set_value(field, &one);
    if (il_field_static_get_value)
        il_field_static_get_value(field, &after);
    fprintf(stderr, "[input] Config.LANDSCAPE_GAME (%s): %u -> %u\n",
            img, before, after);
}

static int g_patched;
static void try_install_hooks(void) {
    if (g_patched) return;
    if (!il_resolve()) return;
    static int pp;
    if (!pp) {
        pp = 1;
        fprintf(stderr, "[input] il2cpp runtime ready; installing input hooks\n");
    }
    static int attempts;
    int sl = attempts < 3;   /* rich step logging for crash attribution */
    attempts++;
    /* verified image locations (global-metadata, 2022.3.62f2 il2cpp):
     * Input/InputUnsafeUtility -> UnityEngine.InputLegacyModule.dll;
     * Screen -> UnityEngine.CoreModule.dll */
    static const char *const legacy_imgs[] = { "InputLegacy", "CoreModule", NULL };
    static const char *const core_imgs[] = { "CoreModule", NULL };
    const char *img_i = "?", *img_u = "?", *img_s = "?";
    if (sl) fprintf(stderr, "[input] step: locate UnityEngine.Input\n");
    void *k_input = il_find_class_in(legacy_imgs, "UnityEngine", "Input",
                                     &img_i, sl);
    if (sl) fprintf(stderr, "[input] step: locate InputUnsafeUtility\n");
    void *k_unsafe = il_find_class_in(legacy_imgs, "UnityEngine.Internal",
                                      "InputUnsafeUtility", &img_u, sl);
    if (sl) fprintf(stderr, "[input] step: locate UnityEngine.Screen\n");
    void *k_screen = il_find_class_in(core_imgs, "UnityEngine", "Screen",
                                      &img_s, sl);
    if (!k_input) {
        static int miss_logged;
        if (!miss_logged) {
            miss_logged = 1;
            fprintf(stderr, "[input] UnityEngine.Input not found yet; retrying\n");
        }
        return;
    }
    if (sl) fprintf(stderr, "[input] step: set LANDSCAPE_GAME\n");
    set_landscape_game();

    if (!g_empty_ilstr && il_string_new) {
        if (sl) fprintf(stderr, "[input] step: il2cpp_string_new(\"\")\n");
        g_empty_ilstr = il_string_new("");
    }

    int got = 0, total = 0;
    static char hooklog[4096];
#define H(cls, mth, argc, body) do { \
        total++; \
        void *mi = il_class_get_method_from_name(cls, mth, argc); \
        if (mi && il_patch(mi, (void *)(body), mth)) { got++; \
            size_t l = strlen(hooklog); \
            snprintf(hooklog + l, sizeof hooklog - l, "%s ", mth); \
        } else fprintf(stderr, "[input] MISS %s/%d (mi=%p)\\n", mth, argc, mi); \
    } while (0)

    /* --- UnityEngine.Input (InputLegacyModule; arities per 2022.3 metadata) */
    H(k_input, "GetKeyInt", 1, inp_GetKeyInt);
    H(k_input, "GetKeyDownInt", 1, inp_GetKeyDownInt);
    H(k_input, "GetKeyUpInt", 1, inp_GetKeyUpInt);
    /* GetKey/GetKeyDown/GetKeyUp have TWO 1-arg overloads each (KeyCode and
     * string) -- ambiguous by name.  Both overloads chain into the icalls
     * patched here (GetKeyInt family / GetKeyString family), so the wrappers
     * are deliberately left alone. */
    H(k_input, "get_anyKey", 0, inp_anyKey);
    H(k_input, "get_inputString", 0, inp_get_inputString);
    H(k_input, "get_mousePosition_Injected", 1, inp_get_mousePosition);
    H(k_input, "get_mouseScrollDelta_Injected", 1, inp_get_mouseScrollDelta);
    H(k_input, "GetMouseButton", 1, inp_GetMouseButton);
    H(k_input, "GetMouseButtonDown", 1, inp_GetMouseButtonDown);
    H(k_input, "GetMouseButtonUp", 1, inp_GetMouseButtonUp);
    H(k_input, "get_touchCount", 0, inp_get_touchCount);
    H(k_input, "get_touchSupported", 0, inp_get_touchSupported);
    H(k_input, "get_mousePresent", 0, inp_get_mousePresent);
    H(k_input, "get_multiTouchEnabled", 0, inp_get_multiTouchEnabled);
    H(k_input, "set_multiTouchEnabled", 1, inp_nop);
    H(k_input, "GetTouch", 1, inp_GetTouch);
    H(k_input, "GetTouch_Injected", 2, inp_GetTouch);
    H(k_input, "GetAxis", 1, inp_GetAxis);
    H(k_input, "GetAxisRaw", 1, inp_GetAxis);
    H(k_input, "GetButtonDown", 1, inp_GetButton);
    /* --- UnityEngine.Internal.InputUnsafeUtility (runs under GetKey(string)
     *     and the axis/button icalls) */
    if (k_unsafe) {
        H(k_unsafe, "GetKeyString", 1, inp_GetKeyString);
        H(k_unsafe, "GetKeyString__Unmanaged", 2, inp_GetKeyString_unmanaged);
        H(k_unsafe, "GetKeyUpString", 1, inp_GetKeyString);
        H(k_unsafe, "GetKeyUpString__Unmanaged", 2, inp_GetKeyString_unmanaged);
        H(k_unsafe, "GetKeyDownString", 1, inp_GetKeyString);
        H(k_unsafe, "GetKeyDownString__Unmanaged", 2, inp_GetKeyString_unmanaged);
        H(k_unsafe, "GetAxis", 1, inp_GetAxis);
        H(k_unsafe, "GetAxis__Unmanaged", 2, inp_GetAxis_unmanaged);
        H(k_unsafe, "GetAxisRaw", 1, inp_GetAxis);
        H(k_unsafe, "GetAxisRaw__Unmanaged", 2, inp_GetAxis_unmanaged);
        H(k_unsafe, "GetButton__Unmanaged", 2, inp_GetButton_unmanaged);
        H(k_unsafe, "GetButtonDown", 1, inp_GetButton);
        H(k_unsafe, "GetButtonDown__Unmanaged", 2, inp_GetButton_unmanaged);
        H(k_unsafe, "GetButtonUp__Unmanaged", 2, inp_GetButton_unmanaged);
    } else {
        fprintf(stderr, "[input] InputUnsafeUtility NOT FOUND\n");
    }
    /* --- UnityEngine.Screen */
    if (k_screen) {
        H(k_screen, "get_orientation", 0, inp_Screen_get_orientation);
        H(k_screen, "GetScreenOrientation", 0, inp_Screen_get_orientation);
        H(k_screen, "set_orientation", 1, inp_nop);
        H(k_screen, "RequestOrientation", 1, inp_nop);
        H(k_screen, "get_dpi", 0, inp_Screen_get_dpi);
        H(k_screen, "get_autorotateToPortrait", 0,
          inp_Screen_get_autorotateToPortrait);
        H(k_screen, "get_fullScreen", 0, inp_Screen_get_fullScreen);
    } else {
        fprintf(stderr, "[input] UnityEngine.Screen NOT FOUND\n");
    }
#undef H
    /* key snapshot baseline for the Down/Up edge icalls */
    g_patched = 1;
    fprintf(stderr, "[input] input hooks installed (%d/%d) imgs: input=%s unsafe=%s screen=%s\n",
            got, total, img_i, img_u, img_s);
    fprintf(stderr, "[input] hooked: %s\n", hooklog);
}

/* ------------------------------------------------------- public hooks */
int gds_input_init(void) { return 0; }

void gds_input_poll(void *env, void *player, unsigned long frame) {
    (void)env; (void)player; (void)frame;
    pad_open();
    memcpy(g_key_prev, g_key_now, sizeof g_key_prev);
    pad_poll();
    frame_update();
    for (size_t i = 0; i < sizeof g_snap_keys / sizeof *g_snap_keys; i++)
        g_key_now[g_snap_keys[i]] = key_from_pad(g_snap_keys[i]);
    /* 0.80: install hooks immediately (this poll precedes frame 0's render,
     * so the managed boot -- including SurfaceManager::Setup at frames 1-3 --
     * sees the landscape flag and every Input answer). */
    if (!g_patched) try_install_hooks();
}

void gds_input_close(void) {
    if (g_gc && s_GameControllerClose) s_GameControllerClose(g_gc);
    g_gc = NULL;
}

/* egl_shim cursor overlay state (drawn via raw GL right before present;
 * only when GDS_CURSOR=1). */
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
