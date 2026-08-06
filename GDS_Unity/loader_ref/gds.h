/* gds.h -- shared declarations for the Game Dev Story port. */

#ifndef GDS_H
#define GDS_H

#include <stddef.h>
#include <stdint.h>

/* Where the game data lives at runtime (argv[1], or the launcher's cwd). */
extern char gds_gamedir[1024];
extern char gds_datadir[1024];   /* <gamedir>/assets */
extern char gds_apk[1024];       /* <gamedir>/assets -- the extracted base APK */
extern char gds_home[1024];      /* <gamedir>/home  -- persistentDataPath */

/* Debug switches, all read once from the environment at start-up and all off
 * by default so the shipped binary is quiet. */
extern int gds_log_level;    /* GDS_LOGCAT   : mirror the game's own log     */
extern int gds_trace_jni;    /* GDS_JNILOG   : every JNI call                */
extern int gds_trace_gl;     /* GDS_GLLOG    : GL calls and shader sources   */
extern long gds_max_frames;  /* GDS_FRAMES=N : stop after N frames           */
extern int gds_capture_mode; /* always zero; retained by the EGL abstraction */

void gds_bionic_init(void);
size_t gds_bionic_count(void);
void gds_pthread_init(void);
void gds_android_init(void);
void gds_egl_init(void);
void gds_jni_init(void);

void *gds_android_sym(const char *name);
void *gds_egl_sym(const char *name);
void *gds_gl_sym(const char *name);
void *gds_jni_sym(const char *name);
void *gds_jni_env(void);
void *gds_jni_vm(void);
void *gds_jni_activity(void);
void *gds_jni_native(const char *cls, const char *name);
void *gds_jret_obj(const char *cls);
void *gds_jret_class(const char *cls);
void *gds_jret_str(const char *text);
void gds_jni_set_unity_player(void *player);
void gds_jni_input_device_info(const char *name, int vendor, int product,
                               const char *descriptor);
void *gds_jni_key_event(int action, int keycode, int scancode);
void *gds_jni_motion_event(float lx, float ly, float rx, float ry,
                           float lt, float rt, float hat_x, float hat_y);
void *gds_jni_touch_event(int action, float x, float y);
void *gds_native_window(void);

/* Unity's Android FMOD backend normally feeds an AudioTrack from
 * FMODAudioDevice.run().  The JNI shim keeps the original fmodGetInfo /
 * fmodProcess contract and opensles_audio.c supplies the missing Java thread through
 * SDL's native NextOS output. */
void *gds_jni_fmod_device(void);
void *gds_jni_fmod_bytebuffer(void);
void *gds_jni_fmod_pcm(void);
int gds_jni_fmod_pcm_capacity(void);
void gds_jni_fmod_set_buffer_size(int bytes);
int gds_jni_fmod_should_run(void);
int gds_audio_start(void *env);
void gds_audio_stop(void);

/* Linux controller -> Android KeyEvent/MotionEvent bridge.  Events are
 * injected on Unity's render thread, just as UnityPlayer forwards View input
 * on Android. */
int gds_input_init(void);
void gds_input_install_now(void);   /* 0.82: patch hooks at module-load time */
void gds_input_poll(void *env, void *player, unsigned long frame);
void gds_input_close(void);
int gds_input_exit_requested(void);
void gds_input_request_exit(void);
/* Right-stick pointer, in 1280x720 top-left coordinates.  EGL reads the
 * snapshot on the render thread immediately before swap. */
int gds_input_cursor(float *x, float *y);
/* EGL publishes the exact viewport used to draw that cursor.  Input then
 * maps the same 1280x720 design point into Unity's physical pointer space. */
void gds_input_set_screen_size(int width, int height);

enum {
    GDS_KEY_CHARACTER,
    GDS_KEY_BACKSPACE,
    GDS_KEY_SHIFT,
    GDS_KEY_SPACE,
    GDS_KEY_DONE,
};

typedef struct {
    int x, y, w, h;
    char label[8];
    char lower;
    char upper;
    int action;
} gds_keyboard_key;

/* Android soft-input replacement.  Unity still opens and receives text
 * through its original showSoftInput/nativeSetInputString lifecycle; input.c
 * supplies the controller UI and EGL only reads its snapshot for drawing. */
void gds_input_keyboard_open(const char *initial, int character_limit);
void gds_input_keyboard_set(const char *text);
void gds_input_keyboard_hide(void);
int gds_input_keyboard_snapshot(char *text, size_t text_size,
                                int *uppercase, int *selected,
                                const gds_keyboard_key **keys,
                                size_t *key_count);
void gds_jni_soft_input_text(const char *text);
void gds_jni_soft_input_selection(int start, int length);
void gds_jni_soft_input_visible(int visible);
void gds_jni_soft_input_closed(int canceled);

/* ---------------- on-screen keyboard (Terraria-style, osk.c) -------------
 * Gamepad-driven QWERTY overlay.  Two drivers open it:
 *   - kairo native plugin: Utility.showInputPanel (FEP panel, the game's
 *     own name/text entry) -> polled via isEndInputPanel/getResultInputPanel
 *   - Unity generic: UnityPlayer.showSoftInput (kept working in parallel)
 * While gds_osk_active() the whole pad feed to the game is blocked
 * (input.c gates the kairo joystick + Unity key layers), so dpad+A drive
 * the keyboard only -- same gate as Terraria's ter_vkbd_blocking(). */
enum { NPB_A, NPB_B, NPB_X, NPB_Y, NPB_LB, NPB_RB, NPB_BACK, NPB_START,
       NPB_L3, NPB_R3, NPB_DU, NPB_DD, NPB_DL, NPB_DR, NPB_COUNT };
void gds_osk_open(const char *title, const char *initial, int maxlen);
void gds_osk_set_text(const char *text);
void gds_osk_hide(void);            /* external close => cancel */
int  gds_osk_active(void);          /* overlay visible, owns the pad */
int  gds_osk_done(void);            /* latched: entry finished (ok or cancel) */
int  gds_osk_result_ok(void);       /* DONE=1 / CANCEL=0 (valid when done) */
const char *gds_osk_text(void);
void gds_osk_pad_tick(const unsigned char *cur, const unsigned char *prev);
void gds_osk_draw(void);            /* called pre-swap from egl_shim */

/* The three arm64 objects, in load order. */
int gds_load_modules(void);
void gds_arm_frame_watchdog(void);
void gds_watchdog_frame(void);

int gds_iterate_mods(int (*cb)(void *, size_t, void *), void *data);

#endif /* GDS_H */

void gds_fs_set_data_dir(const char *dir);
