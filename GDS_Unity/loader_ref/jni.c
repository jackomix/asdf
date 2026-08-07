/*
 * jni.c -- the JNIEnv the Android objects talk to.
 *
 * The vtable is built by index rather than by naming the fields of
 * JNINativeInterface: the port has to match the real slot numbers exactly
 * because Unity and its Android plugins call through them by offset.  Keeping
 * an index table makes every slot checkable against the disassembly.
 *
 * Objects are small tagged structs, not opaque cookies, so GetObjectClass and
 * IsInstanceOf can tell a KeyEvent from a MotionEvent from a byte[] -- Unity
 * needs that distinction for its native Android input and asset paths.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include "jni_cert.h"
#include "musl_compat.h"
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <errno.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>

#include "nx_elf.h"
#include "gds.h"
#include "jni_slots.h"

int gds_trace_jni = 0;

/* JT used to go through nx_log which is itself gated on GDS_VERBOSE -- so
 * GDS_JNILOG=1 alone silently printed nothing (another swallowed-trace trap).
 * Log JT independently, straight to stderr. */
#define JT(...) do { if (gds_trace_jni) { \
    fprintf(stderr, "[jni] " __VA_ARGS__); \
    fputc('\n', stderr); fflush(stderr); } } while (0)

/* ------------------------------------------------------------- object model */

typedef enum {
    O_CLASS, O_STRING, O_BYTEARRAY, O_OBJARRAY, O_OBJECT, O_THROWABLE,
    O_DIRECTBUF,
} otype;

typedef struct jobj {
    otype type;
    const char *cls;         /* class name for O_OBJECT / O_CLASS */
    char *str;               /* O_STRING payload (UTF-8, owned) */
    void *data;              /* array payload */
    int len;
    struct jobj **elems;     /* O_OBJARRAY */
    int64_t prim;            /* payload of a boxed primitive */
    int boxed;
} jobj;

static jobj *new_obj(otype t, const char *cls)
{
    jobj *o = calloc(1, sizeof *o);
    o->type = t;
    o->cls = cls;
    return o;
}

static jobj *mk_class(const char *name)
{
    /* Classes are interned so IsSameObject and the method tables work. */
    static jobj *cache[256];
    static const char *names[256];
    static int n;
    for (int i = 0; i < n; i++)
        if (strcmp(names[i], name) == 0)
            return cache[i];
    if (n == 256)
        nx_die("class cache full (%s)", name);
    names[n] = strdup(name);
    cache[n] = new_obj(O_CLASS, names[n]);
    return cache[n++];
}

static jobj *mk_string(const char *s)
{
    jobj *o = new_obj(O_STRING, "java/lang/String");
    o->str = strdup(s ? s : "");
    o->len = (int)strlen(o->str);
    return o;
}

static jobj *mk_object(const char *cls)
{
    return new_obj(O_OBJECT, cls);
}

static jobj *mk_file(const char *path)
{
    jobj *o = mk_object("java/io/File");
    o->str = strdup(path ? path : "");
    o->len = (int)strlen(o->str);
    return o;
}

/* Objects published to Unity's Android input backend. */
static jobj *input_device;
static jobj *touch_device;
static jobj *motion_range_list;
static jobj *touch_motion_range_list;
static jobj *motion_range_iterator;
static jobj *motion_ranges[8];
static jobj *key_event_object;
static jobj *motion_event_object;
static jobj *fmod_device_object;
static jobj *fmod_bytebuffer;
static jobj *preferences_object;
static jobj *preferences_editor;
static jobj *armory_activity_object;
static jobj *permission_plugin_object;
static unsigned char fmod_pcm[65536];
static int fmod_buffer_size = sizeof fmod_pcm;
static int fmod_should_run;
static char input_device_name[128] = "NextOS Gamepad";
static char input_device_descriptor[160] = "nextos-native-gamepad";
static int input_device_vendor;
static int input_device_product;
static struct {
    int action;
    int keycode;
    int source;
    int device_id;
    int meta_state;
    int repeat;
    int scancode;
    int flags;
    int unicode;
    int64_t event_time;
    int64_t down_time[256];
} key_event;
typedef struct {
    int action;
    int source;
    int device_id;
    int meta_state;
    int button_state;
    int flags;
    int64_t event_time;
    int64_t down_time;
    float axis[48];
} motion_payload;
static motion_payload motion_event;
#define MOTION_CLONE_COUNT 64
static jobj *motion_clones[MOTION_CLONE_COUNT];
static motion_payload motion_clone_data[MOTION_CLONE_COUNT];
static unsigned motion_clone_next;

static const int motion_axis_ids[8] = {
    0, 1, 11, 14, 17, 18, 15, 16,
};

static motion_payload *motion_from_object(jobj *object)
{
    return object && object->data
        ? (motion_payload *)object->data : &motion_event;
}

static int64_t monotonic_millis(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

void gds_jni_input_device_info(const char *name, int vendor, int product,
                               const char *descriptor)
{
    snprintf(input_device_name, sizeof input_device_name, "%s",
             name && *name ? name : "NextOS Gamepad");
    snprintf(input_device_descriptor, sizeof input_device_descriptor,
             "nextos-%04x-%04x-%s", vendor & 0xffff, product & 0xffff,
             descriptor && *descriptor ? descriptor : "gamepad");
    input_device_vendor = vendor;
    input_device_product = product;
}

/*
 * ===== Snapshot por evento (lição do Oceanhorn v1.0.6) =====
 * O motor lê parte do evento DURANTE o nativeInjectEvent e o RESTO depois, da
 * fila de input. O MotionEvent já estava protegido porque o Unity o copia com
 * MotionEvent.obtain() (ver motion_clones). O KeyEvent não tem obtain no
 * Android: com um objeto único lendo a struct global, dois KeyEvents no mesmo
 * quadro — soltar uma diagonal, trocar de direção rápido — faziam a leitura
 * adiada do primeiro devolver os campos do segundo, e a soltura sumia. Cada
 * injeção agora congela seus campos num slot próprio.
 */
#define KEY_CLONE_COUNT 32
static jobj *key_clones[KEY_CLONE_COUNT];
static struct {
    int action, keycode, source, device_id, meta_state, repeat, scancode;
    int flags, unicode;
    int64_t event_time, down_time;
} key_clone_data[KEY_CLONE_COUNT];
static unsigned key_clone_next;

static int key_clone_index(jobj *object)
{
    for (int i = 0; i < KEY_CLONE_COUNT; i++)
        if (object && object == key_clones[i]) return i;
    return -1;
}

void *gds_jni_key_event(int action, int keycode, int scancode)
{
    int64_t now = monotonic_millis();
    if (keycode < 0 || keycode >= (int)(sizeof key_event.down_time /
                                        sizeof *key_event.down_time))
        keycode = 0;
    if (action == 0 || key_event.down_time[keycode] == 0)
        key_event.down_time[keycode] = now;
    key_event.action = action;
    key_event.keycode = keycode;
    /* Android reports these devices as GAMEPAD | DPAD | JOYSTICK. */
    key_event.source = 0x01000611;
    key_event.device_id = 1;
    key_event.meta_state = 0;
    key_event.repeat = 0;
    key_event.scancode = scancode;
    key_event.flags = 0;
    key_event.unicode = 0;
    key_event.event_time = now;
    unsigned slot = key_clone_next++ % KEY_CLONE_COUNT;
    if (!key_clones[slot])
        key_clones[slot] = mk_object("android/view/KeyEvent");
    key_clone_data[slot].action = key_event.action;
    key_clone_data[slot].keycode = key_event.keycode;
    key_clone_data[slot].source = key_event.source;
    key_clone_data[slot].device_id = key_event.device_id;
    key_clone_data[slot].meta_state = key_event.meta_state;
    key_clone_data[slot].repeat = key_event.repeat;
    key_clone_data[slot].scancode = key_event.scancode;
    key_clone_data[slot].flags = key_event.flags;
    key_clone_data[slot].unicode = key_event.unicode;
    key_clone_data[slot].event_time = now;
    key_clone_data[slot].down_time = key_event.down_time[keycode];
    return key_clones[slot];
}

void *gds_jni_motion_event(float lx, float ly, float rx, float ry,
                           float lt, float rt, float hat_x, float hat_y)
{
    int64_t now = monotonic_millis();
    motion_event.action = 2; /* MotionEvent.ACTION_MOVE */
    motion_event.source = 0x01000010; /* InputDevice.SOURCE_JOYSTICK */
    motion_event.device_id = 1;
    motion_event.meta_state = 0;
    motion_event.button_state = 0;
    motion_event.flags = 0;
    motion_event.event_time = now;
    motion_event.down_time = now;
    memset(motion_event.axis, 0, sizeof motion_event.axis);
    motion_event.axis[0] = lx;   /* AXIS_X */
    motion_event.axis[1] = ly;   /* AXIS_Y */
    motion_event.axis[11] = rx;  /* AXIS_Z */
    motion_event.axis[14] = ry;  /* AXIS_RZ */
    motion_event.axis[17] = lt;  /* AXIS_LTRIGGER */
    motion_event.axis[18] = rt;  /* AXIS_RTRIGGER */
    motion_event.axis[15] = hat_x;
    motion_event.axis[16] = hat_y;
    return motion_event_object;
}

void *gds_jni_touch_event(int action, float x, float y)
{
    int64_t now = monotonic_millis();
    motion_event.action = action;
    motion_event.source = 0x00001002; /* InputDevice.SOURCE_TOUCHSCREEN */
    motion_event.device_id = 0;
    motion_event.meta_state = 0;
    motion_event.button_state = 0;
    motion_event.flags = 0;
    motion_event.event_time = now;
    if (action == 0 || motion_event.down_time == 0)
        motion_event.down_time = now;
    memset(motion_event.axis, 0, sizeof motion_event.axis);
    motion_event.axis[0] = x;
    motion_event.axis[1] = y;
    return motion_event_object;
}

void *gds_jni_fmod_device(void)
{
    return fmod_device_object;
}

void *gds_jni_fmod_bytebuffer(void)
{
    return fmod_bytebuffer;
}

void *gds_jni_fmod_pcm(void)
{
    return fmod_pcm;
}

int gds_jni_fmod_pcm_capacity(void)
{
    return (int)sizeof fmod_pcm;
}

/* Current direct-buffer size as FMOD sees it via GetDirectBufferCapacity
 * (shrunk from the 64KiB backing array by the pump thread). */
int gds_jni_fmod_buffer_size(void)
{
    return fmod_buffer_size;
}

void gds_jni_fmod_set_buffer_size(int bytes)
{
    if (bytes <= 0 || bytes > (int)sizeof fmod_pcm)
        return;
    fmod_buffer_size = bytes;
    if (fmod_bytebuffer)
        fmod_bytebuffer->len = bytes;
}

int gds_jni_fmod_should_run(void)
{
    return __atomic_load_n(&fmod_should_run, __ATOMIC_ACQUIRE);
}

/* ----------------------------------------------------------- method registry */

/* Method IDs are indices into a flat table so a handler can be looked up in
 * one step and an unknown method still returns a usable ID (returning NULL
 * from GetMethodID makes Unity abort long before we learn what it wanted). */
typedef struct {
    const char *cls, *name, *sig;
    void *handler;          /* gds_jni_handler, or NULL for "safe default" */
} jmethod;

#define MAX_METHODS 1024
static jmethod methods[MAX_METHODS];
static int method_n;

typedef struct {
    void *env;
    jobj *self;
    va_list *ap;
    const uint64_t *args;
    unsigned arg_index;
    const jmethod *m;
} jctx;
typedef int64_t (*gds_jni_handler)(jctx *);

/* JNI's A calls carry a jvalue[] instead of a va_list.  Both representations
 * use one eight-byte slot per argument on this aarch64 target; keeping the
 * readers here lets the same platform handler serve Call*Method, *V and *A. */
static jobj *jarg_obj(jctx *c)
{
    if (c->args)
        return (jobj *)(uintptr_t)c->args[c->arg_index++];
    return va_arg(*c->ap, jobj *);
}

static int32_t jarg_int(jctx *c)
{
    if (c->args)
        return (int32_t)c->args[c->arg_index++];
    return va_arg(*c->ap, int32_t);
}

static int64_t jarg_long(jctx *c)
{
    if (c->args)
        return (int64_t)c->args[c->arg_index++];
    return va_arg(*c->ap, int64_t);
}

static float jarg_float(jctx *c)
{
    float value;
    if (c->args) {
        uint32_t bits = (uint32_t)c->args[c->arg_index++];
        memcpy(&value, &bits, sizeof value);
        return value;
    }
    return (float)va_arg(*c->ap, double);
}

static void *method_id(const char *cls, const char *name, const char *sig)
{
    for (int i = 0; i < method_n; i++)
        if (strcmp(methods[i].name, name) == 0 &&
            strcmp(methods[i].sig, sig) == 0 &&
            (!cls || !methods[i].cls || strcmp(methods[i].cls, cls) == 0))
            return (void *)(uintptr_t)(i + 1);
    if (method_n == MAX_METHODS)
        nx_die("method table full");
    methods[method_n].cls = cls ? strdup(cls) : NULL;
    methods[method_n].name = strdup(name);
    methods[method_n].sig = strdup(sig);
    methods[method_n].handler = NULL;
    JT("new method id %d %s.%s%s", method_n + 1, cls ? cls : "?", name, sig);
    /* 0.85 audio evidence: FMOD's actual dex/JNI surface, unconditionally */
    if (cls && (strstr(cls, "fmod") || strstr(cls, "FMOD") ||
                strstr(cls, "AudioTrack") || strstr(cls, "AudioManager"))) {
        fprintf(stderr, "[audio] new method id %d %s.%s%s\n",
                method_n + 1, cls, name, sig);
        fflush(stderr);
    }
    return (void *)(uintptr_t)(++method_n);
}

static const jmethod *by_id(void *mid)
{
    uintptr_t i = (uintptr_t)mid;
    return (i >= 1 && i <= (uintptr_t)method_n) ? &methods[i - 1] : NULL;
}

void gds_jni_bind(const char *cls, const char *name, const char *sig, void *fn)
{
    void *id = method_id(cls, name, sig);
    methods[(uintptr_t)id - 1].handler = fn;
}

/* --------------------------------------------------------------- registry of
 * natives that the loaded objects register with us */

typedef struct { char cls[128]; char name[128]; char sig[192]; void *fn; } jnative;
#define MAX_NATIVES 512
static jnative natives[MAX_NATIVES];
static int natives_n;
static jobj *unity_player_object;

void *gds_jni_native(const char *cls, const char *name)
{
    for (int i = 0; i < natives_n; i++)
        if (strcmp(natives[i].name, name) == 0 &&
            (!cls || strcmp(natives[i].cls, cls) == 0))
            return natives[i].fn;
    return NULL;
}

void gds_jni_set_unity_player(void *player)
{
    unity_player_object = player;
}

static jobj *soft_input_player(void)
{
    return unity_player_object
        ? unity_player_object
        : mk_class("com/unity3d/player/UnityPlayer");
}

void gds_jni_soft_input_text(const char *text)
{
    void *native =
        gds_jni_native("com/unity3d/player/UnityPlayer",
                       "nativeSetInputString");
    if (!native)
        return;
    jobj *value = mk_string(text ? text : "");
    ((void (*)(void *, void *, void *))native)(
        gds_jni_env(), soft_input_player(), value);
}

void gds_jni_soft_input_selection(int start, int length)
{
    void *native =
        gds_jni_native("com/unity3d/player/UnityPlayer",
                       "nativeSetInputSelection");
    if (native)
        ((void (*)(void *, void *, int, int))native)(
            gds_jni_env(), soft_input_player(), start, length);
}

void gds_jni_soft_input_visible(int visible)
{
    void *native =
        gds_jni_native("com/unity3d/player/UnityPlayer",
                       "nativeSetKeyboardIsVisible");
    if (native)
        ((void (*)(void *, void *, int))native)(
            gds_jni_env(), soft_input_player(), visible != 0);
}

void gds_jni_soft_input_closed(int canceled)
{
    const char *name =
        canceled ? "nativeSoftInputCanceled" : "nativeSoftInputClosed";
    void *native =
        gds_jni_native("com/unity3d/player/UnityPlayer", name);
    if (native)
        ((void (*)(void *, void *))native)(
            gds_jni_env(), soft_input_player());
}

/* ------------------------------------------------------------------ vtable */

static void *vt[JNI_SLOT_COUNT];
static void *env_ptr = vt;          /* JNIEnv* is a pointer to the vtable ptr */
static void *jvm_vt[8];
static void *jvm_ptr = jvm_vt;

void *gds_jni_env(void) { return &env_ptr; }
void *gds_jni_vm(void) { return &jvm_ptr; }

/* --------------------------------------------------- Java callback/Looper flow
 *
 * Unity 2022 implements UnityChoreographer through bitter/jnibridge:
 *
 *   newInterfaceProxy(Handler.Callback + FrameCallback)
 *   -> HandlerThread.start()
 *   -> Handler(obtained looper, proxy)
 *   -> obtainMessage().sendToTarget()
 *   -> proxy.handleMessage()
 *   -> proxy.doFrame(frameTimeNanos), once per display tick
 *
 * The Android classes do not exist on NextOS, but the native proxy and its
 * callbacks do.  These objects preserve that exact ordering and invoke the
 * registered JNIBridge native just as the Java Looper would.
 */

enum {
    PROXY_HANDLER_CALLBACK = 1u << 0,
    PROXY_FRAME_CALLBACK   = 1u << 1,
    PROXY_RUNNABLE         = 1u << 2,
};

static jobj *choreo_proxy;
static jobj *looper_object;
static jobj *choreographer_object;
static jobj *message_object;
static jobj *doframe_method;
static jobj *handlemsg_method;
static jobj *run_method;
static jobj *doframe_args;
static jobj *handlemsg_args;
static jobj *empty_args;
static jobj *frame_time_box;
static void *doframe_mid;
static void *handlemsg_mid;
static void *run_mid;
static int message_pending;
static int choreo_thread_started;
static int message_what;

static int64_t monotonic_nanos(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (int64_t)now.tv_sec * 1000000000LL + now.tv_nsec;
}

static unsigned proxy_interfaces(jobj *interfaces)
{
    unsigned flags = 0;
    if (!interfaces)
        return flags;
    int n = interfaces->type == O_OBJARRAY ? interfaces->len : 1;
    for (int i = 0; i < n; i++) {
        jobj *c = interfaces->type == O_OBJARRAY
                    ? interfaces->elems[i] : interfaces;
        const char *name = c ? c->cls : NULL;
        if (!name)
            continue;
        if (strcmp(name, "android/os/Handler$Callback") == 0)
            flags |= PROXY_HANDLER_CALLBACK;
        else if (strcmp(name, "android/view/Choreographer$FrameCallback") == 0)
            flags |= PROXY_FRAME_CALLBACK;
        else if (strcmp(name, "java/lang/Runnable") == 0)
            flags |= PROXY_RUNNABLE;
    }
    return flags;
}

static void *invoke_proxy(jobj *proxy, jobj *iface, jobj *method, jobj *args)
{
    if (!proxy || !iface || !method || !args)
        return NULL;
    void *invoke = gds_jni_native("bitter/jnibridge/JNIBridge", "invoke");
    if (!invoke || !proxy->prim)
        return NULL;
    return ((void *(*)(void *, void *, int64_t, void *, void *, void *))invoke)(
        gds_jni_env(), iface, proxy->prim, iface, method, args);
}

static int deliver_handle_message(void)
{
    jobj *proxy = __atomic_load_n(&choreo_proxy, __ATOMIC_ACQUIRE);
    if (!proxy)
        return 0;
    jobj *iface = mk_class("android/os/Handler$Callback");
    nx_log("jni: UnityChoreographer handleMessage(what=%d)", message_what);
    (void)invoke_proxy(proxy, iface, handlemsg_method, handlemsg_args);
    return 1;
}

static int deliver_frame(int64_t nanos)
{
    jobj *proxy = __atomic_load_n(&choreo_proxy, __ATOMIC_ACQUIRE);
    if (!proxy)
        return 0;
    __atomic_store_n(&frame_time_box->prim, nanos, __ATOMIC_RELEASE);
    jobj *iface = mk_class("android/view/Choreographer$FrameCallback");
    (void)invoke_proxy(proxy, iface, doframe_method, doframe_args);
    return 1;
}

static void *choreographer_driver(void *arg)
{
    (void)arg;
    void *domain = NULL;
    void *thread = NULL;
    nx_mod *il2cpp = nx_find_mod("libil2cpp.so");
    if (il2cpp) {
        void *(*domain_get)(void) = nx_lookup_in(il2cpp, "il2cpp_domain_get");
        void *(*thread_attach)(void *) =
            nx_lookup_in(il2cpp, "il2cpp_thread_attach");
        if (domain_get && thread_attach) {
            domain = domain_get();
            thread = thread_attach(domain);
        }
    }
    nx_log("jni: UnityChoreographer HandlerThread active"
           " (il2cpp domain=%p thread=%p)", domain, thread);

    while (!__atomic_exchange_n(&message_pending, 0, __ATOMIC_ACQ_REL))
        usleep(1000);
    if (!deliver_handle_message())
        return NULL;

    const int64_t period = 16666667LL;
    int64_t next = monotonic_nanos() + period;
    unsigned long frames = 0;
    for (;;) {
        while (__atomic_exchange_n(&message_pending, 0, __ATOMIC_ACQ_REL))
            (void)deliver_handle_message();

        struct timespec until = {
            .tv_sec = next / 1000000000LL,
            .tv_nsec = next % 1000000000LL,
        };
        while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &until, NULL) ==
               EINTR)
            ;
        if (deliver_frame(next)) {
            frames++;
            if (frames == 1)
                nx_log("jni: UnityChoreographer first doFrame delivered");
        }
        next += period;
        int64_t now = monotonic_nanos();
        if (next < now)
            next = now + period;
    }
}

/* --- basics ------------------------------------------------------------- */

static int32_t j_GetVersion(void *e) { (void)e; return 0x00010006; }

static jobj *j_FindClass(void *e, const char *name)
{
    (void)e;
    JT("FindClass %s", name);
    return mk_class(name);
}

static jobj *j_GetObjectClass(void *e, jobj *o)
{
    (void)e;
    return mk_class(o && o->cls ? o->cls : "java/lang/Object");
}

static uint8_t j_IsInstanceOf(void *e, jobj *o, jobj *c)
{
    (void)e;
    if (!o || !c || !c->cls)
        return 0;
    if (strcmp(c->cls, "java/lang/Object") == 0)
        return 1;
    if (strcmp(c->cls, "android/view/InputEvent") == 0 && o->cls &&
        (strcmp(o->cls, "android/view/KeyEvent") == 0 ||
         strcmp(o->cls, "android/view/MotionEvent") == 0))
        return 1;
    return o->cls && strcmp(o->cls, c->cls) == 0;
}

static uint8_t j_IsAssignableFrom(void *e, jobj *a, jobj *b)
{
    (void)e;
    return a && b && a->cls && b->cls && strcmp(a->cls, b->cls) == 0;
}

static jobj *j_GetSuperclass(void *e, jobj *c) { (void)e; (void)c; return mk_class("java/lang/Object"); }

static void *j_NewGlobalRef(void *e, void *o) { (void)e; return o; }
static void *j_NewLocalRef(void *e, void *o) { (void)e; return o; }
static void *j_NewWeakGlobalRef(void *e, void *o) { (void)e; return o; }
static void j_DeleteRef(void *e, void *o) { (void)e; (void)o; }
static uint8_t j_IsSameObject(void *e, void *a, void *b) { (void)e; return a == b; }
static int32_t j_PushLocalFrame(void *e, int32_t n) { (void)e; (void)n; return 0; }
static void *j_PopLocalFrame(void *e, void *r) { (void)e; return r; }
static int32_t j_EnsureLocalCapacity(void *e, int32_t n) { (void)e; (void)n; return 0; }

static jobj *pending_exception;
static void *j_ExceptionOccurred(void *e) { (void)e; return pending_exception; }
static void j_ExceptionDescribe(void *e) { (void)e; }
static void j_ExceptionClear(void *e) { (void)e; pending_exception = NULL; }
static uint8_t j_ExceptionCheck(void *e) { (void)e; return pending_exception != NULL; }
static int32_t j_Throw(void *e, jobj *t) { (void)e; pending_exception = t; return 0; }
static int32_t j_ThrowNew(void *e, jobj *c, const char *m)
{
    (void)e;
    nx_log("jni: ThrowNew %s: %s", c && c->cls ? c->cls : "?", m ? m : "");
    pending_exception = new_obj(O_THROWABLE, c ? c->cls : "java/lang/Exception");
    return 0;
}
static void j_FatalError(void *e, const char *m) { (void)e; nx_die("JNI FatalError: %s", m); }

static int32_t j_GetJavaVM(void *e, void **vm) { (void)e; *vm = gds_jni_vm(); return 0; }
static int32_t j_MonitorEnter(void *e, void *o) { (void)e; (void)o; return 0; }
static int32_t j_MonitorExit(void *e, void *o) { (void)e; (void)o; return 0; }

/* --- strings ----------------------------------------------------------- */

/* 0.93: jstring -> managed-string conversion probe.  Device evidence: the
 * FepPanel result [0] AND the on-disk save both read "unny Studios" though
 * getInputPanelResult returned "Sunny Studios" -- the first char drops
 * somewhere in the conversion path.  Log short printable strings through
 * these four entry points (the only ones libunity can use) until found. */
static int g_strprobe_n;
static void strprobe(const char *what, jobj *s)
{
    if (!s || !s->str || s->len < 2 || s->len > 64) return;
    if (g_strprobe_n >= 24) return;
    for (int i = 0; i < s->len; i++)
        if ((unsigned char)s->str[i] < 0x20 || (unsigned char)s->str[i] > 0x7e)
            return;
    g_strprobe_n++;
    fprintf(stderr, "[jni] %s: len=%d '%s'\n", what, s->len, s->str);
}

static jobj *j_NewStringUTF(void *e, const char *s) { (void)e; return mk_string(s); }
static const char *j_GetStringUTFChars(void *e, jobj *s, uint8_t *copy)
{
    (void)e;
    if (copy) *copy = 0;
    strprobe("GetStringUTFChars", s);
    return s && s->str ? s->str : "";
}
static void j_ReleaseStringUTFChars(void *e, jobj *s, const char *c) { (void)e; (void)s; (void)c; }
static int32_t j_GetStringUTFLength(void *e, jobj *s) { (void)e; return s ? s->len : 0; }
static int32_t j_GetStringLength(void *e, jobj *s) { (void)e; strprobe("GetStringLength", s); return s ? s->len : 0; }
static jobj *j_NewString(void *e, const uint16_t *u, int32_t n)
{
    (void)e;
    char *b = malloc((size_t)n + 1);
    for (int32_t i = 0; i < n; i++)
        b[i] = (char)(u[i] < 128 ? u[i] : '?');
    b[n] = 0;
    jobj *o = mk_string(b);
    free(b);
    return o;
}
static const uint16_t *j_GetStringChars(void *e, jobj *s, uint8_t *copy)
{
    (void)e;
    if (copy) *copy = 1;
    strprobe("GetStringChars", s);
    int n = s ? s->len : 0;
    uint16_t *u = calloc((size_t)n + 1, 2);
    for (int i = 0; i < n; i++)
        u[i] = (uint8_t)s->str[i];
    return u;
}
static void j_ReleaseStringChars(void *e, jobj *s, const uint16_t *u) { (void)e; (void)s; free((void *)u); }
static void j_GetStringUTFRegion(void *e, jobj *s, int32_t off, int32_t len, char *out)
{
    (void)e;
    if (s && s->str) {
        { static int n; if (n++ < 24 && s->len >= 2 && s->len <= 64)
            fprintf(stderr, "[jni] GetStringUTFRegion: off=%d len=%d (strlen=%d) '%s'\n",
                    off, len, s->len, s->str); }
        if (off < 0) off = 0;
        if (off > s->len) off = s->len;
        if (len > s->len - off) len = s->len - off;
        memcpy(out, s->str + off, (size_t)len);
    }
}

/* --- arrays ------------------------------------------------------------ */

static int32_t j_GetArrayLength(void *e, jobj *a) { (void)e; return a ? a->len : 0; }

static jobj *j_NewByteArray(void *e, int32_t n)
{
    (void)e;
    jobj *o = new_obj(O_BYTEARRAY, "[B");
    o->len = n;
    o->data = calloc((size_t)(n > 0 ? n : 1), 1);
    return o;
}

static jobj *j_NewIntArray(void *e, int32_t n)
{
    (void)e;
    jobj *o = new_obj(O_BYTEARRAY, "[I");
    o->len = n;
    o->data = calloc((size_t)(n > 0 ? n : 1), sizeof(int32_t));
    return o;
}

static jobj *mk_int_array(const int32_t *values, int32_t n)
{
    jobj *o = j_NewIntArray(NULL, n);
    if (values && n > 0)
        memcpy(o->data, values, (size_t)n * sizeof *values);
    return o;
}
static void *j_GetByteArrayElements(void *e, jobj *a, uint8_t *copy)
{
    (void)e;
    if (copy) *copy = 0;
    return a ? a->data : NULL;
}
static void j_ReleaseArrayElements(void *e, jobj *a, void *p, int32_t mode)
{
    (void)e; (void)a; (void)p; (void)mode;
}
static void j_GetByteArrayRegion(void *e, jobj *a, int32_t s, int32_t n, void *buf)
{
    (void)e;
    if (a && a->data)
        memcpy(buf, (char *)a->data + s, (size_t)n);
}
static void j_SetByteArrayRegion(void *e, jobj *a, int32_t s, int32_t n, const void *buf)
{
    (void)e;
    if (a && a->data)
        memcpy((char *)a->data + s, buf, (size_t)n);
}

static void j_GetIntArrayRegion(void *e, jobj *a, int32_t s, int32_t n, void *buf)
{
    (void)e;
    if (a && a->data)
        memcpy(buf, (int32_t *)a->data + s, (size_t)n * sizeof(int32_t));
}

static void j_SetIntArrayRegion(void *e, jobj *a, int32_t s, int32_t n,
                                const void *buf)
{
    (void)e;
    if (a && a->data)
        memcpy((int32_t *)a->data + s, buf, (size_t)n * sizeof(int32_t));
}
static jobj *j_NewObjectArray(void *e, int32_t n, jobj *cls, jobj *init)
{
    (void)e; (void)cls;
    jobj *o = new_obj(O_OBJARRAY, "[Ljava/lang/Object;");
    o->len = n;
    o->elems = calloc((size_t)(n > 0 ? n : 1), sizeof *o->elems);
    for (int32_t i = 0; i < n; i++)
        o->elems[i] = init;
    return o;
}
static jobj *j_GetObjectArrayElement(void *e, jobj *a, int32_t i)
{
    (void)e;
    return (a && a->elems && i >= 0 && i < a->len) ? a->elems[i] : NULL;
}
static void j_SetObjectArrayElement(void *e, jobj *a, int32_t i, jobj *v)
{
    (void)e;
    if (a && a->elems && i >= 0 && i < a->len)
        a->elems[i] = v;
}
static void *j_GetPrimitiveArrayCritical(void *e, jobj *a, uint8_t *copy)
{
    return j_GetByteArrayElements(e, a, copy);
}
static void j_ReleasePrimitiveArrayCritical(void *e, jobj *a, void *p, int32_t m)
{
    (void)e; (void)a; (void)p; (void)m;
}
static jobj *j_NewDirectByteBuffer(void *e, void *addr, int64_t cap)
{
    (void)e;
    jobj *o = new_obj(O_DIRECTBUF, "java/nio/ByteBuffer");
    o->data = addr;
    o->len = (int)cap;
    return o;
}
static void *j_GetDirectBufferAddress(void *e, jobj *b) { (void)e; return b ? b->data : NULL; }
static int64_t j_GetDirectBufferCapacity(void *e, jobj *b) { (void)e; return b ? b->len : -1; }
static int32_t j_GetObjectRefType(void *e, void *o) { (void)e; return o ? 2 : 0; }

/* --- calls ------------------------------------------------------------- */

static int is_unbox(const char *name)
{
    static const char *const n[] = { "longValue", "intValue", "shortValue",
                                     "byteValue", "charValue", "booleanValue",
                                     "floatValue", "doubleValue" };
    for (size_t i = 0; i < sizeof n / sizeof *n; i++)
        if (strcmp(name, n[i]) == 0)
            return 1;
    return 0;
}

/* --- Android input devices, KeyEvent and MotionEvent ------------------ */

static int64_t jfloat_result(float value)
{
    uint32_t bits;
    memcpy(&bits, &value, sizeof bits);
    return bits;
}

static int64_t j_InputDevice_getDeviceIds(jctx *c)
{
    (void)c;
    /* Real Android enumerates its built-in touchscreen as an InputDevice too.
     * Unity must see that device during its startup scan before later
     * touchscreen MotionEvents can become Touches. */
    const int32_t ids[] = { 0, 1 };
    static int logged;
    if (!logged++)
        nx_log("jni: publishing Android touchscreen id=0 and gamepad id=1 "
               "(%s %04x:%04x)",
               input_device_name, input_device_vendor & 0xffff,
               input_device_product & 0xffff);
    return (int64_t)(uintptr_t)mk_int_array(ids, 2);
}

static int64_t j_InputDevice_getDevice(jctx *c)
{
    int id = jarg_int(c);
    return (int64_t)(uintptr_t)(id == 0 ? touch_device :
                                id == 1 ? input_device : NULL);
}

static int64_t j_InputEvent_getDevice(jctx *c)
{
    if (c->self && c->self->cls &&
        strcmp(c->self->cls, "android/view/MotionEvent") == 0 &&
        motion_from_object(c->self)->device_id == 0) {
        JT("touch getDevice -> %p (id=0 sources=%#x)",
           (void *)touch_device, 0x00001002);
        return (int64_t)(uintptr_t)touch_device;
    }
    return (int64_t)(uintptr_t)input_device;
}

static int64_t j_InputDevice_getString(jctx *c)
{
    if (c->self == touch_device)
        return (int64_t)(uintptr_t)mk_string(
            strcmp(c->m->name, "getDescriptor") == 0
                ? "nextos:touch:0" : "NextOS Touchscreen");
    const char *value = strcmp(c->m->name, "getDescriptor") == 0
        ? input_device_descriptor : input_device_name;
    return (int64_t)(uintptr_t)mk_string(value);
}

static int64_t j_InputDevice_getInt(jctx *c)
{
    if (c->self == touch_device) {
        if (strcmp(c->m->name, "getSources") == 0) return 0x00001002;
        if (strcmp(c->m->name, "getId") == 0) return 0;
        return 0;
    }
    if (strcmp(c->m->name, "getVendorId") == 0) return input_device_vendor;
    if (strcmp(c->m->name, "getProductId") == 0) return input_device_product;
    if (strcmp(c->m->name, "getSources") == 0) return 0x01000611;
    if (strcmp(c->m->name, "getId") == 0) return 1;
    if (strcmp(c->m->name, "getControllerNumber") == 0) return 1;
    return 0; /* KEYBOARD_TYPE_NONE */
}

static int64_t j_InputDevice_supportsSource(jctx *c)
{
    int source = jarg_int(c);
    int available = c->self == touch_device ? 0x00001002 : 0x01000611;
    return (available & source) == source;
}

static int64_t j_InputDevice_getMotionRanges(jctx *c)
{
    return (int64_t)(uintptr_t)(c->self == touch_device
                                    ? touch_motion_range_list
                                    : motion_range_list);
}

static int64_t j_InputDevice_getMotionRange(jctx *c)
{
    int axis = jarg_int(c);
    if (c->self == touch_device)
        return 0;
    for (size_t i = 0; i < sizeof motion_axis_ids /
                            sizeof *motion_axis_ids; i++)
        if (motion_axis_ids[i] == axis)
            return (int64_t)(uintptr_t)motion_ranges[i];
    return 0;
}

static int64_t j_List_size(jctx *c)
{
    return c->self ? c->self->len : 0;
}

static int64_t j_List_get(jctx *c)
{
    int index = jarg_int(c);
    if (!c->self || !c->self->elems || index < 0 || index >= c->self->len)
        return 0;
    return (int64_t)(uintptr_t)c->self->elems[index];
}

static int64_t j_List_iterator(jctx *c)
{
    motion_range_iterator->elems = c->self ? c->self->elems : NULL;
    motion_range_iterator->len = c->self ? c->self->len : 0;
    motion_range_iterator->prim = 0;
    return (int64_t)(uintptr_t)motion_range_iterator;
}

static int64_t j_Iterator_hasNext(jctx *c)
{
    return c->self && c->self->prim < c->self->len;
}

static int64_t j_Iterator_next(jctx *c)
{
    if (!c->self || !c->self->elems ||
        c->self->prim < 0 || c->self->prim >= c->self->len)
        return 0;
    return (int64_t)(uintptr_t)c->self->elems[c->self->prim++];
}

static int64_t j_MotionRange_getInt(jctx *c)
{
    if (!c->self)
        return 0;
    if (strcmp(c->m->name, "getAxis") == 0)
        return c->self->prim;
    return 0x01000010; /* SOURCE_JOYSTICK */
}

static int64_t j_MotionRange_getFloat(jctx *c)
{
    int axis = c->self ? (int)c->self->prim : -1;
    float value = 0.0f;
    if (strcmp(c->m->name, "getMin") == 0)
        value = axis == 17 || axis == 18 ? 0.0f : -1.0f;
    else if (strcmp(c->m->name, "getMax") == 0)
        value = 1.0f;
    else if (strcmp(c->m->name, "getFlat") == 0)
        value = 0.05f;
    else if (strcmp(c->m->name, "getResolution") == 0)
        value = 1.0f / 32767.0f;
    return jfloat_result(value);
}

static int64_t j_KeyEvent_getInt(jctx *c)
{
    int slot = key_clone_index(c->self);
    const char *n = c->m->name;
#define KEY_FIELD(field) (slot >= 0 ? key_clone_data[slot].field : key_event.field)
    if (strcmp(n, "getAction") == 0) return KEY_FIELD(action);
    if (strcmp(n, "getKeyCode") == 0) return KEY_FIELD(keycode);
    if (strcmp(n, "getSource") == 0) return KEY_FIELD(source);
    if (strcmp(n, "getDeviceId") == 0) return KEY_FIELD(device_id);
    if (strcmp(n, "getMetaState") == 0) return KEY_FIELD(meta_state);
    if (strcmp(n, "getRepeatCount") == 0) return KEY_FIELD(repeat);
    if (strcmp(n, "getScanCode") == 0) return KEY_FIELD(scancode);
    if (strcmp(n, "getFlags") == 0) return KEY_FIELD(flags);
    if (strcmp(n, "getUnicodeChar") == 0) return KEY_FIELD(unicode);
#undef KEY_FIELD
    return 0;
}

static int64_t j_KeyEvent_getLong(jctx *c)
{
    int slot = key_clone_index(c->self);
    if (strcmp(c->m->name, "getDownTime") == 0)
        return slot >= 0 ? key_clone_data[slot].down_time
                         : key_event.down_time[key_event.keycode];
    return slot >= 0 ? key_clone_data[slot].event_time : key_event.event_time;
}

static int64_t j_KeyEvent_isSystem(jctx *c)
{
    (void)c;
    return 0;
}

static int64_t j_MotionEvent_getInt(jctx *c)
{
    const char *name = c->m->name;
    if (c->self && c->self->cls &&
        strcmp(c->self->cls, "android/view/KeyEvent") == 0)
        return j_KeyEvent_getInt(c);
    motion_payload *event = motion_from_object(c->self);
    if (strcmp(name, "getPointerId") == 0 ||
        strcmp(name, "findPointerIndex") == 0 ||
        strcmp(name, "getToolType") == 0)
        (void)jarg_int(c);
    if (strcmp(name, "getAction") == 0 ||
        strcmp(name, "getActionMasked") == 0) {
        int value = strcmp(name, "getActionMasked") == 0
            ? event->action & 0xff : event->action;
        if (event->source == 0x00001002)
            JT("touch %s -> %d", name, value);
        return value;
    }
    if (strcmp(name, "getSource") == 0) {
        if (event->source == 0x00001002)
            JT("touch getSource -> %#x", event->source);
        return event->source;
    }
    if (strcmp(name, "getDeviceId") == 0) {
        if (event->source == 0x00001002)
            JT("touch getDeviceId -> %d", event->device_id);
        return event->device_id;
    }
    if (strcmp(name, "getMetaState") == 0) return event->meta_state;
    if (strcmp(name, "getButtonState") == 0) return event->button_state;
    if (strcmp(name, "getFlags") == 0) return event->flags;
    if (strcmp(name, "getPointerCount") == 0) return 1;
    if (strcmp(name, "getPointerId") == 0) return 0;
    if (strcmp(name, "findPointerIndex") == 0) return 0;
    if (strcmp(name, "getToolType") == 0)
        return event->source == 0x00001002 ? 1 : 0; /* TOOL_TYPE_FINGER */
    return 0; /* history/action index, edge flags, tool type, display id */
}

static int64_t j_MotionEvent_getLong(jctx *c)
{
    if (c->self && c->self->cls &&
        strcmp(c->self->cls, "android/view/KeyEvent") == 0)
        return j_KeyEvent_getLong(c);
    motion_payload *event = motion_from_object(c->self);
    if (c->m->sig[1] == 'I')
        (void)jarg_int(c);
    int64_t value = strcmp(c->m->name, "getDownTime") == 0
        ? event->down_time : event->event_time;
    if (event->source == 0x00001002)
        JT("touch %s -> %lld", c->m->name, (long long)value);
    return value;
}

static int64_t j_MotionEvent_getFloat(jctx *c)
{
    const char *name = c->m->name;
    motion_payload *event = motion_from_object(c->self);
    int axis = -1;
    if (strcmp(name, "getAxisValue") == 0 ||
        strcmp(name, "getHistoricalAxisValue") == 0) {
        axis = jarg_int(c);
    } else if (strcmp(name, "getX") == 0 ||
               strcmp(name, "getHistoricalX") == 0 ||
               strcmp(name, "getRawX") == 0) {
        axis = 0;
    } else if (strcmp(name, "getY") == 0 ||
               strcmp(name, "getHistoricalY") == 0 ||
               strcmp(name, "getRawY") == 0) {
        axis = 1;
    }
    float value;
    if (strcmp(name, "getPressure") == 0)
        value = event->source == 0x00001002 ? 1.0f : 0.0f;
    else if (strcmp(name, "getSize") == 0)
        value = event->source == 0x00001002 ? 1.0f : 0.0f;
    else if (strcmp(name, "getTouchMajor") == 0 ||
             strcmp(name, "getTouchMinor") == 0 ||
             strcmp(name, "getToolMajor") == 0 ||
             strcmp(name, "getToolMinor") == 0)
        value = event->source == 0x00001002 ? 8.0f : 0.0f;
    else
        value = axis >= 0 && axis < (int)(sizeof event->axis /
                                          sizeof *event->axis)
                  ? event->axis[axis] : 0.0f;
    if (event->source == 0x00001002)
        JT("touch %s axis=%d -> %.3f", name, axis, value);
    return jfloat_result(value);
}

static int64_t j_MotionEvent_isFromSource(jctx *c)
{
    motion_payload *event = motion_from_object(c->self);
    int source = jarg_int(c);
    return (event->source & source) == source;
}

static int64_t j_MotionEvent_obtain(jctx *c)
{
    jobj *source = jarg_obj(c);
    unsigned slot = motion_clone_next++ % MOTION_CLONE_COUNT;
    if (!motion_clones[slot])
        motion_clones[slot] = mk_object("android/view/MotionEvent");
    motion_clone_data[slot] = *motion_from_object(source);
    motion_clones[slot]->data = &motion_clone_data[slot];
    if (motion_clone_data[slot].source == 0x00001002)
        JT("touch obtain clone=%u action=%d xy=%.1f,%.1f "
           "down=%lld event=%lld", slot, motion_clone_data[slot].action,
           motion_clone_data[slot].axis[0], motion_clone_data[slot].axis[1],
           (long long)motion_clone_data[slot].down_time,
           (long long)motion_clone_data[slot].event_time);
    return (int64_t)(uintptr_t)motion_clones[slot];
}

static int64_t j_MotionEvent_recycle(jctx *c)
{
    (void)c;
    return 0;
}

/* java.io.File.  Unity and Android plugins inspect their data files before
 * opening them.  A zero length is not a neutral answer: it means a truncated
 * or missing install and selects their normal report-and-bail path. */
static int64_t j_File_length(jctx *c)
{
    const char *p = c->self ? c->self->str : NULL;
    struct stat st;
    if (!p || stat(p, &st) != 0) {
        JT("File.length(\"%s\") -> 0 (no such file)", p ? p : "(null)");
        return 0;
    }
    JT("File.length(\"%s\") -> %lld", p, (long long)st.st_size);
    return (int64_t)st.st_size;
}

static int64_t j_File_exists(jctx *c)
{
    const char *p = c->self ? c->self->str : NULL;
    struct stat st;
    int64_t r = p && stat(p, &st) == 0;
    JT("File.exists(\"%s\") -> %lld", p ? p : "(null)", (long long)r);
    return r;
}

static int64_t j_File_getPath(jctx *c)
{
    return (int64_t)(uintptr_t)mk_string(c->self ? c->self->str : NULL);
}

static int64_t j_File_getParent(jctx *c)
{
    const char *path = c->self && c->self->str ? c->self->str : "";
    char buf[1200];
    snprintf(buf, sizeof buf, "%s", path);
    char *slash = strrchr(buf, '/');
    if (!slash)
        buf[0] = 0;
    else if (slash == buf)
        slash[1] = 0;
    else
        *slash = 0;
    return (int64_t)(uintptr_t)mk_string(buf);
}

static int64_t j_File_getParentFile(jctx *c)
{
    jobj *s = (jobj *)(uintptr_t)j_File_getParent(c);
    return (int64_t)(uintptr_t)mk_file(s ? s->str : "");
}

static int64_t j_File_isDirectory(jctx *c)
{
    struct stat st;
    const char *p = c->self && c->self->str ? c->self->str : "";
    return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

static int64_t j_File_mkdirs(jctx *c)
{
    const char *p = c->self && c->self->str ? c->self->str : "";
    if (!*p)
        return 0;
    if (mkdir(p, 0755) == 0 || errno == EEXIST)
        return 1;
    return 0;
}

static char gds_android_filesdir[1400];

/* Real Android: context.getFilesDir()       -> /data/data/<pkg>/files
 *               context.getExternalFilesDir -> /sdcard/Android/data/<pkg>/files
 * Kairosoft's Storage::GetFolder cuts the package name out of that path with
 * LastIndexOf("data/")+5 .. LastIndexOf("/files") and Substring().  When
 * we answered with the bare <gamedir>/home both probes missed and Substring
 * (start=4, len=-5) raised ArgumentOutOfRangeException, which unwound
 * Storage::Open(4) -> RecordStore::Setup stored nothing -> later
 * GetNumRecords NRE -> "An error has occurred." dialog at frame 3.
 * Answer with an Android-shaped path rooted under gds_home instead and
 * pre-create it (their File.mkdirs() only does a single mkdir). */
static const char *gds_filesdir_path(void)
{
    if (!*gds_android_filesdir) {
        snprintf(gds_android_filesdir, sizeof gds_android_filesdir,
                 "%s/Android/data/net.kairosoft.android.gamedev3en/files",
                 gds_home);
        char tmp[1400];
        snprintf(tmp, sizeof tmp, "%s", gds_android_filesdir);
        for (char *p = tmp + 1; ; p++) {
            if (*p == '/' || *p == 0) {
                int end = *p == 0;
                *p = 0;
                if (mkdir(tmp, 0755) && errno != EEXIST)
                    nx_log("filesdir: mkdir %s: %s", tmp, strerror(errno));
                if (end)
                    break;
                *p = '/';
            }
        }
    }
    return gds_android_filesdir;
}

static int64_t j_Context_getFilesDir(jctx *c)
{
    (void)c;
    return (int64_t)(uintptr_t)mk_file(gds_filesdir_path());
}

static int64_t j_Context_getPackageCodePath(jctx *c)
{
    (void)c;
    /* Unity treats this as the extracted package root and appends
     * assets/bin/Data itself.  Returning gds_datadir would produce
     * <gamedir>/assets/assets/bin/Data. */
    return (int64_t)(uintptr_t)mk_string(gds_gamedir);
}

static int64_t j_Context_getPackageName(jctx *c)
{
    (void)c;
    return (int64_t)(uintptr_t)mk_string("net.kairosoft.android.gamedev3en");
}

static int64_t j_Context_getAssets(jctx *c)
{
    (void)c;
    return (int64_t)(uintptr_t)mk_object("android/content/res/AssetManager");
}

static int64_t j_Context_getContentResolver(jctx *c)
{
    (void)c;
    return (int64_t)(uintptr_t)mk_object("android/content/ContentResolver");
}

static int64_t j_SettingsSecure_getString(jctx *c)
{
    (void)jarg_obj(c); /* ContentResolver */
    jobj *key = jarg_obj(c);
    const char *name = key && key->str ? key->str : "";

    /* Android's ANDROID_ID is a stable, app-visible 64-bit hexadecimal
     * identifier.  Keep it deterministic so the game's offline profile and
     * obfuscation keys remain valid between launches. */
    const char *value = strcmp(name, "android_id") == 0
                            ? "6e6578746f736867"
                            : "";
    JT("Settings.Secure.getString(\"%s\") -> \"%s\"", name, value);
    return (int64_t)(uintptr_t)mk_string(value);
}

static int64_t j_Locale_getDefault(jctx *c)
{
    (void)c;
    return (int64_t)(uintptr_t)mk_object("java/util/Locale");
}

static int64_t j_Locale_getString(jctx *c)
{
    const char *value = "en-US";
    if (strcmp(c->m->name, "getLanguage") == 0)
        value = "en";
    else if (strcmp(c->m->name, "getCountry") == 0)
        value = "US";
    else if (strcmp(c->m->name, "toString") == 0)
        value = "en_US";
    return (int64_t)(uintptr_t)mk_string(value);
}

static int64_t j_GoogleIAB_instance(jctx *c)
{
    (void)c;
    return (int64_t)(uintptr_t)mk_object("com/prime31/GoogleIABPlugin");
}

static int64_t j_PlayGames_initialize(jctx *c)
{
    (void)jarg_obj(c);
    return 0;
}

static int64_t j_PlayGames_getSignInClient(jctx *c)
{
    (void)jarg_obj(c);
    return (int64_t)(uintptr_t)mk_object(
        "com/google/android/gms/games/GamesSignInClient");
}

/* UnityPlayer's constructor only publishes itself and creates this
 * permission helper.  These objects reproduce that Java-side state for the
 * C# AndroidJavaObject calls while all permissions needed by the local port
 * are already satisfied by its own game directory. */
static int64_t j_Armory_getActivity(jctx *c)
{
    (void)c;
    return (int64_t)(uintptr_t)armory_activity_object;
}

static void normalize_reflection_signature(char *out, size_t capacity,
                                           const char *signature)
{
    if (!capacity)
        return;
    size_t i = 0;
    for (; signature && signature[i] && i + 1 < capacity; i++)
        out[i] = signature[i] == '.' ? '/' : signature[i];
    out[i] = 0;
}

/* Unity 2022 routes AndroidJavaObject/AndroidJavaClass lookups through its
 * Java ReflectionHelper before converting the reflected handle back into a
 * JNI method/field ID.  Preserve that native Android sequence: the reflected
 * object and the eventual ID share the stable registry entry, exactly as in
 * the proven Terraria/Horizon bridge. */
static int64_t j_Reflection_getConstructorID(jctx *c)
{
    jobj *target = jarg_obj(c);
    jobj *signature = jarg_obj(c);
    const char *cls = target && target->cls ? target->cls : NULL;
    const char *sig = signature && signature->str ? signature->str : "";
    char normalized[512];
    normalize_reflection_signature(normalized, sizeof normalized, sig);
    JT("ReflectionHelper.getConstructorID(%s, %s)", cls ? cls : "?", sig);
    return (int64_t)(uintptr_t)method_id(cls, "<init>", normalized);
}

static int64_t j_Reflection_getMethodID(jctx *c)
{
    jobj *target = jarg_obj(c);
    jobj *name = jarg_obj(c);
    jobj *signature = jarg_obj(c);
    int is_static = jarg_int(c);
    const char *cls = target && target->cls ? target->cls : NULL;
    const char *method = name && name->str ? name->str : "";
    const char *sig = signature && signature->str ? signature->str : "";
    char normalized[512];
    normalize_reflection_signature(normalized, sizeof normalized, sig);
    JT("ReflectionHelper.getMethodID(%s.%s, %s, static=%d)",
       cls ? cls : "?", method, sig, is_static);
    return (int64_t)(uintptr_t)method_id(cls, method, normalized);
}

static int64_t j_Reflection_getFieldID(jctx *c)
{
    jobj *target = jarg_obj(c);
    jobj *name = jarg_obj(c);
    jobj *signature = jarg_obj(c);
    int is_static = jarg_int(c);
    const char *cls = target && target->cls ? target->cls : NULL;
    const char *field = name && name->str ? name->str : "";
    const char *sig = signature && signature->str ? signature->str : "";
    char normalized[512];
    normalize_reflection_signature(normalized, sizeof normalized, sig);
    JT("ReflectionHelper.getFieldID(%s.%s, %s, static=%d)",
       cls ? cls : "?", field, sig, is_static);
    return (int64_t)(uintptr_t)method_id(cls, field, normalized);
}

static int64_t j_Reflection_getFieldSignature(jctx *c)
{
    void *field = jarg_obj(c);
    const jmethod *entry = by_id(field);
    return (int64_t)(uintptr_t)mk_string(entry && entry->sig
                                             ? entry->sig : "");
}

static int64_t j_Unity_currentActivity(jctx *c)
{
    (void)c;
    return (int64_t)(uintptr_t)armory_activity_object;
}

/* Kairosoft's native-dialog utility.  Unhandled, it used to return 0 with no
 * trace; six frames later Unity's nativeRender returned keep=0 (quit).  Log
 * the dialog payload so we can see what the game is asking/telling, and keep
 * the return observable.  GDS_DIALOG_RESULT=n overrides the answer for
 * experiments. */
/* Utility.getScaleRatio(w,h,gameW,gameH): Kairosoft's letterbox scale.  The
 * unhandled 0.0f was followed by their generic "An error has occurred." dialog
 * (type 0) and a quit -- on a device this returns ~1-2.  Compute it like the
 * plugin does (fit ratio) and log the args once for verification. */
static int64_t float_bits(float f);
static int64_t j_Kairo_getScaleRatio(jctx *c)
{
    int gw = jarg_int(c), gh = jarg_int(c), m1 = jarg_int(c), m2 = jarg_int(c);
    /* 2.6.9 dex ground truth:
     *   w = display.getWidth();  h = display.getHeight();
     *   if (w > h) h = realSize.y;                 // landscape raw height
     *   if (getSystemBarHeight() > 0) h -= bar;    // 0 here (fullscreen)
     *   ratio = min((w+m1)*100/gw, (h+m2)*100/gh)
     * The magic 1120403456 in the dex is the float 100.0 -- the result is a
     * PERCENTAGE.  Unhandled 0.0f / 1.0f made the Kairosoft canvas compute
     * ~0/1% scale -> degenerate layout -> "An error has occurred." (run28).
     * Real window here: 640x480 (w>h branch -> h stays 480). */
    float w = 640.0f, h = 480.0f;
    float ratio = 100.0f;
    if (gw > 0 && gh > 0) {
        float rw = (w + m1) * 100.0f / (float)gw;
        float rh = (h + m2) * 100.0f / (float)gh;
        ratio = rw < rh ? rw : rh;
    }
    JT("getScaleRatio(%d,%d,%d,%d) -> %f", gw, gh, m1, m2, ratio);
    return float_bits(ratio);
}

/* Utility.getNotificationData(): Kairosoft's pending-notification payload.
 * APK decompile (2.6.9 classes.dex, kairo/android/plugin/Utility):
 *   Preference p = Preference.get(currentActivity,
 *                                 "_plugin_notification_data");
 *   if (p != null) { ...parse lines... return String[n]; }
 *   else           { return new String[0]; }
 * It NEVER returns null.  Our unhandled NULL made the managed boot NRE and
 * pop the generic "An error has occurred." dialog, then nativeRender keep=0
 * quit at frame 6 (qemu run13).  Return a real empty String array. */
static int64_t j_Utility_getNotificationData(jctx *c)
{
    (void)c;
    jobj *arr = j_NewObjectArray(NULL, 0, mk_class("java/lang/String"), NULL);
    if (arr)
        arr->cls = "[Ljava/lang/String;";
    JT("Utility.getNotificationData() -> String[0] (empty)");
    return (int64_t)(uintptr_t)arr;
}

/* Utility.getNotificationFilter(ctx): APK decompile shows the preference
 * "_plugin_notification_level" defaults to 3 when absent (also 3 on
 * exception).  Our old unhandled 0 was off-spec. */
static int64_t j_Utility_getNotificationFilter(jctx *c)
{
    (void)c;
    JT("Utility.getNotificationFilter() -> 3 (plugin default)");
    return 3;
}

/* Utility.getPackageName(): plugin decompile --
 *   return UnityPlayer.currentActivity.getPackageName();
 * Returning NULL (old unhandled path) is what a null Context would give and
 * made the managed side NRE during KairoPlugin.Init (run14/23). */
static int64_t j_Utility_getPackageName(jctx *c)
{
    (void)c;
    JT("Utility.getPackageName() -> net.kairosoft.android.gamedev3en");
    return (int64_t)(uintptr_t)mk_string(
        "net.kairosoft.android.gamedev3en");
}

/* Utility.getAppWidth()/getAppHeight(): plugin decompile --
 *   wm.getDefaultDisplay().getMetrics(displayMetrics_);
 *   return displayMetrics_.widthPixels / .heightPixels;
 * Unhandled, both returned 0 -- observed flowing straight into
 * getScaleRatio(240,240,0,0) and the Kairosoft app-size math broke (run23).
 * The real window on the target is 640x480 landscape (matches the
 * DisplayMetrics chain we already serve). */
static int64_t j_Utility_getAppWidth(jctx *c)
{
    (void)c;
    JT("Utility.getAppWidth() -> 640");
    return 640;
}

/* Utility.isTablet(): kairo.unity.ui.IApplication caches IsTablet()&1 into a
 * static and only offers the System->Options orientation toggle (rotation /
 * landscape layout) when it's 1 (libil2cpp: KairoPlugin::_isTablet @0x17f8084
 * via literal #8017 'isTablet'; GetRotateCheck site @0x175b628).  Unbound,
 * our bridge returned 0 -> the game locked itself into the portrait layout
 * on the R36S's 640x480 4:3 panel.  The panel is landscape: report tablet=1
 * so the game enables its landscape/rotation path like the Steam build. */
static int64_t j_Utility_isTablet(jctx *c)
{
    /* GDS_TABLET env knob (gds_env.cfg on device): default 1 = landscape
     * toggle verified since 0.82.  Set GDS_TABLET=0 to test whether tablet
     * mode is what shrinks the tabs/dialog buttons -- with the IsSide
     * patch owning landscape, tablet=0 should keep the landscape layout
     * but re-enable whatever phone-scale UI the game has. */
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("GDS_TABLET");
        v = e ? !!atoi(e) : 1;
        fprintf(stderr, "[jni] Utility.isTablet() -> %d (GDS_TABLET=%s)\n",
                v, e ? e : "unset, default 1");
        fflush(stderr);
    }
    (void)c;
    JT("Utility.isTablet() -> %d", v);
    return v;
}

/* Utility.getSystemBarHeight(): queried by the dex getScaleRatio chain
 * ("if (getSystemBarHeight() > 0) h -= bar").  Fullscreen port: no system
 * bar, 0.  Previously unhandled (returned 0 anyway, but logged as unbound);
 * bind it so the JNI log stays meaningful. */
static int64_t j_Utility_getSystemBarHeight(jctx *c)
{
    (void)c;
    JT("Utility.getSystemBarHeight() -> 0");
    return 0;
}
static int64_t j_Utility_getAppHeight(jctx *c)
{
    (void)c;
    JT("Utility.getAppHeight() -> 480");
    return 480;
}

/* Plugin also exposes getWidth()/getHeight() (width==appWidth; height ==
 * appHeight - statusBar(0 for the fullscreen game)).  Same emulated values. */
static int64_t j_Utility_getWidth(jctx *c)
{
    (void)c;
    JT("Utility.getWidth() -> 640");
    return 640;
}
static int64_t j_Utility_getHeight(jctx *c)
{
    (void)c;
    JT("Utility.getHeight() -> 480");
    return 480;
}

/* inventory callback helpers for gds_managed_stacktrace */
static const char *(*g_inv_cgn)(void *);
static const char *(*g_inv_cgns)(void *);
static void *(*g_inv_cgm)(void *, void **);
static const char *(*g_inv_mgn)(void *);
static int g_inv_shown;

static void gds_inv_report(void *klass, void *user)
{
    (void)user;
    if (!klass || g_inv_shown >= 220)
        return;
    const char *ns = g_inv_cgns(klass);
    const char *name = g_inv_cgn(klass);
    if (!ns || !name)
        return;
    if (!strstr(ns, "kairo") && !strstr(ns, "Kairo"))
        return;
    fprintf(stderr, "  class %s.%s:\n    ", ns, name);
    g_inv_shown++;
    if (g_inv_cgm && g_inv_mgn) {
        void *iter = NULL, *mi;
        int shown = 0;
        while (shown < 40 && (mi = g_inv_cgm(klass, &iter)) != NULL) {
            fprintf(stderr, "%s%s", shown ? ", " : "", g_inv_mgn(mi));
            shown++;
        }
    }
    fprintf(stderr, "\n");
    fflush(stderr);
}

/* ---- managed stack trace via the exported il2cpp embedding API ----------
 * libil2cpp.so exports il2cpp_domain_get / class_from_name / runtime_invoke
 * (241 il2cpp_* symbols).  When the game pops its generic error dialog we
 * invoke System.Environment.get_StackTrace() to learn the managed call
 * chain that led there; otherwise the cause is invisible in a release build
 * that never writes logcat. */
static void gds_managed_stacktrace(const char *why)
{
    nx_mod *il2cpp = nx_find_mod("libil2cpp.so");
    if (!il2cpp)
        return;
    void *(*domain_get)(void) = nx_lookup_in(il2cpp, "il2cpp_domain_get");
    void **(*domain_get_assemblies)(void *, size_t *) =
        nx_lookup_in(il2cpp, "il2cpp_domain_get_assemblies");
    void *(*assembly_get_image)(void *) =
        nx_lookup_in(il2cpp, "il2cpp_assembly_get_image");
    void *(*class_from_name)(void *, const char *, const char *) =
        nx_lookup_in(il2cpp, "il2cpp_class_from_name");
    void *(*class_get_method_from_name)(void *, const char *, int) =
        nx_lookup_in(il2cpp, "il2cpp_class_get_method_from_name");
    void *(*runtime_invoke)(void *, void *, void **, void **) =
        nx_lookup_in(il2cpp, "il2cpp_runtime_invoke");
    uint16_t *(*string_chars)(void *) =
        nx_lookup_in(il2cpp, "il2cpp_string_chars");
    if (!domain_get || !domain_get_assemblies || !assembly_get_image ||
        !class_from_name || !class_get_method_from_name || !runtime_invoke ||
        !string_chars) {
        fprintf(stderr, "[gds] stacktrace: il2cpp API incomplete\n");
        return;
    }
    void *domain = domain_get();
    size_t count = 0;
    void **assemblies = domain_get_assemblies(domain, &count);
    fprintf(stderr, "[gds] managed stack (%s), %zu assemblies:\n",
            why ? why : "?", count);

    /* find System.Diagnostics.StackTrace in any image */
    void *st_class = NULL;
    for (size_t i = 0; i < count && !st_class; i++) {
        void *image = assembly_get_image(assemblies[i]);
        if (image)
            st_class = class_from_name(image, "System.Diagnostics",
                                       "StackTrace");
    }
    if (!st_class) {
        fprintf(stderr, "[gds] stacktrace: System.Diagnostics.StackTrace"
                        " class not found\n");
        return;
    }
    /* One-time method inventory: the exact member names differ across Unity
     * versions and the class metadata is all we have to go on. */
    {
        void *(*class_get_methods)(void *, void **) =
            nx_lookup_in(il2cpp, "il2cpp_class_get_methods");
        const char *(*method_get_name)(void *) =
            nx_lookup_in(il2cpp, "il2cpp_method_get_name");
        static int dumped;
        if (class_get_methods && method_get_name && !dumped) {
            dumped = 1;
            void *iter = NULL;
            void *mi;
            fprintf(stderr, "[gds] stacktrace: StackTrace methods:");
            int shown = 0;
            while (shown < 64 &&
                   (mi = class_get_methods(st_class, &iter)) != NULL) {
                fprintf(stderr, "%s%s", shown ? ", " : "\n  ",
                        method_get_name(mi));
                shown++;
            }
            fprintf(stderr, "\n");
            fflush(stderr);
        }
    }
    /* runtime_invoke on StackTrace's ctor segfaults inside libil2cpp
     * (both overloads, both arg sets -- observed).  Inventory instead:
     * enumerate kairo.unity* classes and their methods; the class list tells
     * us which subsystem could have raised the error dialog. */
    void (*class_for_each)(void (*)(void *, void *), void *) =
        nx_lookup_in(il2cpp, "il2cpp_class_for_each");
    const char *(*class_get_name)(void *) =
        nx_lookup_in(il2cpp, "il2cpp_class_get_name");
    const char *(*class_get_namespace)(void *) =
        nx_lookup_in(il2cpp, "il2cpp_class_get_namespace");
    (void)st_class;
    if (!class_for_each || !class_get_name || !class_get_namespace) {
        fprintf(stderr, "[gds] stacktrace: class_for_each unavailable\n");
        return;
    }
    static int inventoried;
    if (getenv("GDS_KAIRO_INV") && !inventoried) {
        inventoried = 1;
        void *(*class_get_methods)(void *, void **) =
            nx_lookup_in(il2cpp, "il2cpp_class_get_methods");
        const char *(*method_get_name)(void *) =
            nx_lookup_in(il2cpp, "il2cpp_method_get_name");
        fprintf(stderr, "[gds] kairo.unity class inventory:\n");
        g_inv_cgn = class_get_name;
        g_inv_cgns = class_get_namespace;
        g_inv_cgm = class_get_methods;
        g_inv_mgn = method_get_name;
        g_inv_shown = 0;
        class_for_each(gds_inv_report, NULL);
        fprintf(stderr, "[gds] kairo.unity inventory end (%d classes)\n",
                g_inv_shown);
        fflush(stderr);
    }
    (void)class_for_each;

    /* Proper managed stack trace.  Earlier attempts invoked the StackTrace
     * ctor with obj=NULL -- ctors need a real instance -- which segfaulted in
     * libil2cpp twice.  This uses il2cpp_object_new + 0-arg .ctor. */
    void *(*object_new)(void *) = nx_lookup_in(il2cpp, "il2cpp_object_new");
    static int trace_done;
    if (!object_new || trace_done)
        return;
    trace_done = 1;
    void *ctor0 = class_get_method_from_name(st_class, ".ctor", 0);
    void *get_frame = class_get_method_from_name(st_class, "GetFrame", 1);
    void *get_frame_count =
        class_get_method_from_name(st_class, "get_FrameCount", 0);
    if (!ctor0 || !get_frame || !get_frame_count) {
        fprintf(stderr, "[gds] stacktrace: missing members (ctor0=%p"
                        " GetFrame=%p frame_count=%p)\n",
                ctor0, get_frame, get_frame_count);
        return;
    }
    void *st = object_new(st_class);
    void *exc = NULL;
    runtime_invoke(ctor0, st, NULL, &exc);
    if (exc) {
        fprintf(stderr, "[gds] stacktrace: ctor threw %p\n", exc);
        return;
    }
    exc = NULL;
    void *fc_box = runtime_invoke(get_frame_count, st, NULL, &exc);
    if (!fc_box || exc) {
        fprintf(stderr, "[gds] stacktrace: frame_count failed (exc=%p)\n",
                exc);
        return;
    }
    int frames = *(int *)((char *)fc_box + 0x18);
    if (frames < 0 || frames > 96)
        frames = 96;
    void *sf_class = NULL;
    for (size_t i = 0; i < count && !sf_class; i++) {
        void *image = assembly_get_image(assemblies[i]);
        if (image)
            sf_class = class_from_name(image, "System.Diagnostics",
                                       "StackFrame");
    }
    void *get_method = sf_class
        ? class_get_method_from_name(sf_class, "GetMethod", 0) : NULL;
    void *sf_to_string = sf_class
        ? class_get_method_from_name(sf_class, "ToString", 0) : NULL;
    static void *get_decltype, *get_name_m, *type_get_fullname;
    if (!get_name_m) {
        for (size_t i = 0; i < count; i++) {
            void *image = assembly_get_image(assemblies[i]);
            if (!image)
                continue;
            void *mc = class_from_name(image, "System.Reflection",
                                       "MemberInfo");
            if (mc && !get_name_m)
                get_name_m = class_get_method_from_name(mc, "get_Name", 0);
            void *tc = class_from_name(image, "System", "Type");
            if (tc && !type_get_fullname)
                type_get_fullname =
                    class_get_method_from_name(tc, "get_FullName", 0);
            void *mbc = class_from_name(image, "System.Reflection",
                                        "MethodBase");
            if (mbc && !get_decltype)
                get_decltype = class_get_method_from_name(
                    mbc, "get_DeclaringType", 0);
        }
    }
    for (int f = 0; f < frames; f++) {
        int idx = f;
        void *fa[1] = { &idx };
        exc = NULL;
        void *frame = runtime_invoke(get_frame, st, fa, &exc);
        if (!frame || exc)
            break;
        /* StackFrame.ToString() */
        exc = NULL;
        void *ts = sf_to_string
            ? runtime_invoke(sf_to_string, frame, NULL, &exc) : NULL;
        exc = NULL;
        void *mb = get_method
            ? runtime_invoke(get_method, frame, NULL, &exc) : NULL;
        char nbuf[256] = "", tbuf[512] = "", sbuf[1024] = "";
        uint16_t *ch;
        size_t n;
        ch = ts ? string_chars(ts) : NULL;
        for (n = 0; ch && ch[n] && n + 1 < sizeof sbuf; n++)
            sbuf[n] = ch[n] < 0x80 ? (char)ch[n] : '?';
        sbuf[n] = 0;
        if (mb) {
            exc = NULL;
            void *name_s = get_name_m
                ? runtime_invoke(get_name_m, mb, NULL, &exc) : NULL;
            exc = NULL;
            void *dt = get_decltype
                ? runtime_invoke(get_decltype, mb, NULL, &exc) : NULL;
            exc = NULL;
            void *full_s = (dt && type_get_fullname)
                ? runtime_invoke(type_get_fullname, dt, NULL, &exc) : NULL;
            ch = name_s ? string_chars(name_s) : NULL;
            for (n = 0; ch && ch[n] && n + 1 < sizeof nbuf; n++)
                nbuf[n] = ch[n] < 0x80 ? (char)ch[n] : '?';
            nbuf[n] = 0;
            ch = full_s ? string_chars(full_s) : NULL;
            for (n = 0; ch && ch[n] && n + 1 < sizeof tbuf; n++)
                tbuf[n] = ch[n] < 0x80 ? (char)ch[n] : '?';
            tbuf[n] = 0;
        }
        fprintf(stderr, "  at %s.%s ()%s%s\n", tbuf, nbuf,
                sbuf[0] ? "  [" : "", sbuf[0] ? sbuf : "");
        if (sbuf[0])
            fprintf(stderr, "     toString: %s\n", sbuf);
    }
    fflush(stderr);
}

/* String.getBytes([charset]): the Kairosoft plugin's Preference class calls
 * String.valueOf(byte).getBytes() and getBytes(charset) when (de)serializing
 * tiny prefs blobs; NULL (the old unhandled answer) NPEs inside their code.
 * Charset name is ignored deliberately: ASCII/UTF-8 identity mapping covers
 * everything the plugin encodes (Base64 alphabet, digits). */
static int64_t j_String_getBytes(jctx *c)
{
    const char *lp = strchr(c->m->sig, 'L');
    if (lp && lp < strchr(c->m->sig, ')'))
        (void)jarg_obj(c);   /* charset name arg of the (String)[B overload */
    const char *s = c->self && c->self->str ? c->self->str : "";
    jobj *arr = j_NewByteArray(NULL, (int32_t)strlen(s));
    memcpy(arr->data, s, (size_t)arr->len);
    JT("String.getBytes(\"%s\") -> [%d bytes]", s, arr->len);
    return (int64_t)(uintptr_t)arr;
}

/* ---- kairo-state sweep: find the exception behind the error dialog ----
 * The managed exception text never reaches logcat (release build strips
 * Log.GetThrowableLog output to "").  But the thrower almost always keeps
 * state: sweep every kairo.unity.* class' static fields for objects whose
 * class name contains "Exception" and print their message via the exported
 * il2cpp API.  Also print static string fields named like error/message. */
typedef struct {
    void *mod;
    void *(*object_get_class)(void *);
    const char *(*class_get_name)(void *);
    const char *(*class_get_namespace)(void *);
    void *(*class_get_fields)(void *, void **);
    const char *(*field_get_name)(void *);
    int (*field_get_flags)(void *);
    void *(*field_get_type)(void *);
    void (*field_static_get_value)(void *, void *);
    void *(*class_get_method_from_name)(void *, const char *, int);
    void *(*runtime_invoke)(void *, void *, void **, void **);
    uint16_t *(*string_chars)(void *);
    int dumped;
} sweep_ctx;
static sweep_ctx g_sweep;

static void sweep_invoke_string(void *obj, void *klass, const char *label,
                                const char *fname)
{
    const char *cands[] = { "get_DisplayMessage", "get_Message",
                            "ToString", NULL };
    for (int i = 0; cands[i]; i++) {
        void *m = g_sweep.class_get_method_from_name(klass, cands[i], 0);
        if (!m)
            continue;
        void *exc = NULL;
        void *s = g_sweep.runtime_invoke(m, obj, NULL, &exc);
        if (!s || exc)
            continue;
        uint16_t *ch = g_sweep.string_chars(s);
        char buf[2048];
        size_t n;
        for (n = 0; ch && ch[n] && n + 1 < sizeof buf; n++)
            buf[n] = ch[n] < 0x80 ? (char)ch[n] : '?';
        buf[n] = 0;
        fprintf(stderr, "    %s.%s: %s():\n    %s\n", label, fname,
                cands[i], buf);
    }
}

static void sweep_report_class(void *klass, void *user)
{
    (void)user;
    if (!klass || g_sweep.dumped > 400)
        return;
    const char *ns = g_sweep.class_get_namespace(klass);
    const char *cname = g_sweep.class_get_name(klass);
    if (!ns || !cname || !strstr(ns, "kairo"))
        return;
    void *iter = NULL, *fld;
    int shown = 0;
    while ((fld = g_sweep.class_get_fields(klass, &iter)) != NULL &&
           shown++ < 80) {
        int flags = g_sweep.field_get_flags(fld);
        if (!(flags & 0x10))   /* FIELD_ATTRIBUTE_STATIC */
            continue;
        const char *fname = g_sweep.field_get_name(fld);
        void *ftype = g_sweep.field_get_type(fld);
        unsigned tbyte = ftype ? *((const unsigned char *)ftype + 10) : 0;
        /* 0x0e STRING, 0x12 CLASS, 0x1c OBJECT, 0x1d SZARRAY, 0x15 GENINST */
        if (tbyte != 0x0e && tbyte != 0x12 && tbyte != 0x1c &&
            tbyte != 0x1d && tbyte != 0x15)
            continue;
        void *val = NULL;
        g_sweep.field_static_get_value(fld, &val);
        if (!val)
            continue;
        if (tbyte == 0x0e) {
            /* static string: only print error/message-ish names */
            if (fname && (strstr(fname, "rror") || strstr(fname, "essa") ||
                          strstr(fname, "xception"))) {
                uint16_t *ch = g_sweep.string_chars(val);
                char buf[256];
                size_t n;
                for (n = 0; ch && ch[n] && n + 1 < sizeof buf; n++)
                    buf[n] = ch[n] < 0x80 ? (char)ch[n] : '?';
                buf[n] = 0;
                fprintf(stderr, "    STR %s.%s.%s = \"%s\"\n", ns, cname,
                        fname, buf);
                g_sweep.dumped++;
            }
            continue;
        }
        void *vklass = g_sweep.object_get_class(val);
        if (!vklass)
            continue;
        const char *vname = g_sweep.class_get_name(vklass);
        if (!vname)
            continue;
        if (strstr(vname, "Exception") || strstr(vname, "Error")) {
            fprintf(stderr, "  EXC-OBJ %s.%s.%s -> %s.%s@%p\n", ns, cname,
                    fname ? fname : "?",
                    g_sweep.class_get_namespace(vklass) ?: "", vname, val);
            sweep_invoke_string(val, vklass, vname, fname ? fname : "?");
            g_sweep.dumped++;
        }
    }
}

static void dump_field_value(void *klass, const char *ns, const char *cname,
                             void *fld, void *obj, int is_static)
{
    const char *fname = g_sweep.field_get_name(fld);
    void *ftype = g_sweep.field_get_type(fld);
    unsigned tbyte = ftype ? *((const unsigned char *)ftype + 10) : 0;
    int flags = g_sweep.field_get_flags(fld);
    union { int64_t i; double d; void *p; } v;
    memset(&v, 0, sizeof v);
    if (is_static)
        g_sweep.field_static_get_value(fld, &v);
    else {
        void *(*field_get_value)(void *, void *, void *) =
            nx_lookup_in(g_sweep.mod, "il2cpp_field_get_value");
        if (!field_get_value)
            return;
        field_get_value(obj, fld, &v);
    }
    char prefix[320];
    snprintf(prefix, sizeof prefix, "  %s%s.%s.%s",
             (flags & 0x10) ? "S " : "I ", ns, cname,
             fname ? fname : "?");
    switch (tbyte) {
    case 0x02: /* BOOLEAN */
        fprintf(stderr, "%s = %s (bool)\n", prefix, v.i ? "true" : "false");
        break;
    case 0x04: case 0x05: case 0x06: case 0x07:
    case 0x08: case 0x09: case 0x0a: case 0x0b:
        fprintf(stderr, "%s = %lld (int)\n", prefix, (long long)v.i);
        break;
    case 0x0c: /* R4 */
        fprintf(stderr, "%s = %f (float)\n", prefix, (double)*(float *)&v.i);
        break;
    case 0x0d: /* R8 */
        fprintf(stderr, "%s = %f (double)\n", prefix, v.d);
        break;
    case 0x0e: {/* STRING */
        char buf[160] = "(null)";
        if (v.p) {
            uint16_t *ch = g_sweep.string_chars(v.p);
            size_t n;
            for (n = 0; ch && ch[n] && n + 1 < sizeof buf; n++)
                buf[n] = ch[n] < 0x80 ? (char)ch[n] : '?';
            buf[n] = 0;
        }
        fprintf(stderr, "%s = \"%s\"\n", prefix, buf);
        break; }
    case 0x11: /* VALUETYPE/enum -- print raw 32 bits */
        fprintf(stderr, "%s = %u (vt/enum)\n", prefix, (unsigned)v.i);
        break;
    default: { /* references */
        if (!v.p) {
            fprintf(stderr, "%s = null (obj t%02x)\n", prefix, tbyte);
            break;
        }
        void *vk = g_sweep.object_get_class(v.p);
        const char *vn = vk ? g_sweep.class_get_name(vk) : "?";
        fprintf(stderr, "%s = obj %s@%p\n", prefix, vn ? vn : "?", v.p);
        break; }
    }
}

static void sweep_dump_class(void *klass, void *user)
{
    (void)user;
    if (!klass || g_sweep.dumped > 160)
        return;
    const char *ns = g_sweep.class_get_namespace(klass);
    const char *cname = g_sweep.class_get_name(klass);
    if (!cname)
        return;
    ns = ns ? ns : "";
    static const char *needles[] = {
        "ApplicationManager", "KairoBase", "IApplication", "Language",
        "Canvas", "KairoPlugin", NULL
    };
    int hit = 0;
    for (int i = 0; needles[i]; i++)
        if (strcmp(cname, needles[i]) == 0) {
            hit = 1;
            break;
        }
    if (!hit)
        return;
    fprintf(stderr, " CLASS %s.%s:\n", ns, cname);
    g_sweep.dumped++;
    void *iter = NULL, *fld;
    int n = 0;
    while ((fld = g_sweep.class_get_fields(klass, &iter)) != NULL && n++ < 80)
        dump_field_value(klass, ns, cname, fld, NULL, 1);
    /* dump the live singleton too, when the class exposes get_Current */
    void *getcur = g_sweep.class_get_method_from_name(klass, "get_Current", 0);
    void *getinst = getcur
        ? NULL
        : g_sweep.class_get_method_from_name(klass, "GetInstance", 0);
    void *m = getcur ? getcur : getinst;
    if (m) {
        void *exc = NULL;
        void *inst = g_sweep.runtime_invoke(m, NULL, NULL, &exc);
        if (inst && !exc) {
            fprintf(stderr, "  INSTANCE %s.%s@%p:\n", ns, cname, inst);
            void *iter2 = NULL, *f2;
            int k = 0;
            while ((f2 = g_sweep.class_get_fields(klass, &iter2)) != NULL &&
                   k++ < 80) {
                int fl = g_sweep.field_get_flags(f2);
                if (fl & 0x10)
                    continue;   /* statics already printed */
                if (fl & 0x40)
                    continue;   /* literal: no per-object storage (crashes
                                   il2cpp_field_get_value at off -1) */
                dump_field_value(klass, ns, cname, f2, inst, 0);
            }
            /* also dump BASE CLASS instance fields (one level, covers
             * KairoBase-derived Application classes) */
            void *parent = NULL;
            void *(*cgp)(void *) =
                nx_lookup_in(g_sweep.mod, "il2cpp_class_get_parent");
            if (cgp)
                parent = cgp(klass);
            if (parent) {
                const char *pn = g_sweep.class_get_name(parent);
                const char *pns = g_sweep.class_get_namespace(parent);
                if (pn && strstr(pn, "airo")) {
                    fprintf(stderr, "  BASE %s.%s:\n", pns ? pns : "", pn);
                    void *iter3 = NULL, *f3;
                    int j = 0;
                    while ((f3 = g_sweep.class_get_fields(parent, &iter3))
                               != NULL && j++ < 80) {
                        int fl = g_sweep.field_get_flags(f3);
                        dump_field_value(parent, pns ? pns : "", pn, f3,
                                         inst, !(fl & 0x10));
                    }
                }
            }
        }
    }
}

static void gds_inspect_kairo_state(void)
{
    static int done;
    if (done || !getenv("GDS_SWEEP"))
        return;
    done = 1;
    nx_mod *il2cpp = nx_find_mod("libil2cpp.so");
    if (!il2cpp)
        return;
    void (*class_for_each)(void (*)(void *, void *), void *) =
        nx_lookup_in(il2cpp, "il2cpp_class_for_each");
    memset(&g_sweep, 0, sizeof g_sweep);
    g_sweep.mod = il2cpp;
    g_sweep.object_get_class = nx_lookup_in(il2cpp, "il2cpp_object_get_class");
    g_sweep.class_get_name = nx_lookup_in(il2cpp, "il2cpp_class_get_name");
    g_sweep.class_get_namespace =
        nx_lookup_in(il2cpp, "il2cpp_class_get_namespace");
    g_sweep.class_get_fields = nx_lookup_in(il2cpp, "il2cpp_class_get_fields");
    g_sweep.field_get_name = nx_lookup_in(il2cpp, "il2cpp_field_get_name");
    g_sweep.field_get_flags = nx_lookup_in(il2cpp, "il2cpp_field_get_flags");
    g_sweep.field_get_type = nx_lookup_in(il2cpp, "il2cpp_field_get_type");
    g_sweep.field_static_get_value =
        nx_lookup_in(il2cpp, "il2cpp_field_static_get_value");
    g_sweep.class_get_method_from_name =
        nx_lookup_in(il2cpp, "il2cpp_class_get_method_from_name");
    g_sweep.runtime_invoke = nx_lookup_in(il2cpp, "il2cpp_runtime_invoke");
    g_sweep.string_chars = nx_lookup_in(il2cpp, "il2cpp_string_chars");
    if (!class_for_each || !g_sweep.object_get_class ||
        !g_sweep.class_get_fields || !g_sweep.field_static_get_value) {
        fprintf(stderr, "[gds] sweep: il2cpp field API incomplete\n");
        return;
    }
    fprintf(stderr, "[gds] kairo static-field sweep (exception hunt):\n");
    class_for_each(sweep_report_class, NULL);
    fprintf(stderr, "[gds] sweep done (%d hits)\n", g_sweep.dumped);
    fprintf(stderr, "[gds] kairo decision-class state dump:\n");
    g_sweep.dumped = 0;
    class_for_each(sweep_dump_class, NULL);
    fprintf(stderr, "[gds] state dump done\n");
    fflush(stderr);
}

/* The 4th showDialog parameter may be a RAW il2cpp object (il2cpp passes
 * managed objects straight through as jobjects).  If it is, its class name
 * and -- for kairo.unity.util.KairoException -- its DisplayMessage tell us
 * exactly which managed exception produced the error dialog. */
static void gds_probe_managed_object(void *obj, const char *why)
{
    if (!obj || (uintptr_t)obj < 0x10000)
        return;
    nx_mod *il2cpp = nx_find_mod("libil2cpp.so");
    if (!il2cpp)
        return;
    void *(*object_get_class)(void *) =
        nx_lookup_in(il2cpp, "il2cpp_object_get_class");
    const char *(*class_get_name)(void *) =
        nx_lookup_in(il2cpp, "il2cpp_class_get_name");
    const char *(*class_get_namespace)(void *) =
        nx_lookup_in(il2cpp, "il2cpp_class_get_namespace");
    void *(*class_get_method_from_name)(void *, const char *, int) =
        nx_lookup_in(il2cpp, "il2cpp_class_get_method_from_name");
    void *(*runtime_invoke)(void *, void *, void **, void **) =
        nx_lookup_in(il2cpp, "il2cpp_runtime_invoke");
    uint16_t *(*string_chars)(void *) =
        nx_lookup_in(il2cpp, "il2cpp_string_chars");
    if (!object_get_class || !class_get_name)
        return;
    void *klass = object_get_class(obj);
    if (!klass)
        return;
    const char *ns = class_get_namespace
        ? class_get_namespace(klass) : NULL;
    const char *name = class_get_name(klass);
    fprintf(stderr, "[gds] probe(%s): object %p -> class %s.%s\n",
            why ? why : "?", obj, ns ? ns : "", name ? name : "?");
    if (!class_get_method_from_name || !runtime_invoke)
        return;
    /* KairoException exposes get_DisplayMessage; Message/get_Message is the
     * System.Exception base.  Try both, and ToString as fallback. */
    const char *candidates[] = { "get_DisplayMessage", "get_Message",
                                 "ToString", "GetThrowableLog", NULL };
    for (int i = 0; candidates[i]; i++) {
        void *m = class_get_method_from_name(klass, candidates[i], 0);
        if (!m)
            continue;
        void *exc = NULL;
        void *s = runtime_invoke(m, obj, NULL, &exc);
        if (!s || exc)
            continue;
        uint16_t *ch = string_chars ? string_chars(s) : NULL;
        char buf[2048];
        size_t n;
        for (n = 0; ch && ch[n] && n + 1 < sizeof buf; n++)
            buf[n] = ch[n] < 0x80 ? (char)ch[n] : '?';
        buf[n] = 0;
        fprintf(stderr, "[gds] probe(%s): %s():\n%s\n", why,
                candidates[i], buf);
    }
    fflush(stderr);
}

/* ---- PackageManager / PackageInfo / Signature ---------------------------
 * Boot evidence (run31 state sweep): KairoPlugin.signature_ was NULL and
 * KairoBase.illegalPackage_ was TRUE -- the anti-tamper signature check had
 * failed because Context.getPackageManager() returned NULL and the game
 * could never read PackageInfo.signatures.  A null signature -> "illegal
 * package" -> generic "An error has occurred." dialog -> quit. */
static jobj *package_manager_object;
static jobj *signature_object;
static jobj *signatures_array;

static int64_t j_Context_getPackageManager(jctx *c)
{
    (void)c;
    if (!package_manager_object)
        package_manager_object = mk_object("android/content/pm/PackageManager");
    JT("getPackageManager() -> %p", (void *)package_manager_object);
    return (int64_t)(uintptr_t)package_manager_object;
}

static int64_t j_PM_getPackageInfo(jctx *c)
{
    jobj *pkg = jarg_obj(c);
    int flags = jarg_int(c);
    jobj *info = mk_object("android/content/pm/PackageInfo");
    info->str = strdup(pkg && pkg->str
                           ? pkg->str
                           : "net.kairosoft.android.gamedev3en");
    info->prim = flags;
    JT("PackageManager.getPackageInfo(\"%s\", %d) -> info",
       info->str, flags);
    return (int64_t)(uintptr_t)info;
}

static int64_t j_PackageInfo_getLong(jctx *c)
{
    return 0L;   /* versionCode / lastUpdateTime -- unused by the game */
}
static int64_t j_PackageInfo_getInt(jctx *c)
{
    return 0;
}
static int64_t j_PackageInfo_versionName(jctx *c)
{
    return (int64_t)(uintptr_t)mk_string("2.6.9");
}
static int64_t j_PackageInfo_packageName(jctx *c)
{
    return (int64_t)(uintptr_t)mk_string(
        "net.kairosoft.android.gamedev3en");
}
static int64_t j_PackageInfo_signatures(jctx *c)
{
    (void)c;
    if (!signature_object) {
        signature_object = mk_object("android/content/pm/Signature");
        jobj *der = j_NewByteArray(NULL, GDS_CERT_DER_LEN);
        memcpy(der->data, gds_cert_der, GDS_CERT_DER_LEN);
        signature_object->data = der;   /* DER bytes, for toByteArray() */
        signatures_array =
            j_NewObjectArray(NULL, 1, mk_class("android/content/pm/Signature"),
                             signature_object);
        if (signatures_array)
            signatures_array->cls = "[Landroid/content/pm/Signature;";
    }
    JT("PackageInfo.signatures -> [1 signature]");
    return (int64_t)(uintptr_t)signatures_array;
}

static int64_t j_Signature_hashCode(jctx *c)
{
    (void)c;
    JT("Signature.hashCode() -> %d", GDS_CERT_DER_JHASH);
    return GDS_CERT_DER_JHASH;
}

static int64_t j_Signature_toByteArray(jctx *c)
{
    jobj *der = j_NewByteArray(NULL, GDS_CERT_DER_LEN);
    memcpy(der->data, gds_cert_der, GDS_CERT_DER_LEN);
    JT("Signature.toByteArray() -> [%d bytes]", GDS_CERT_DER_LEN);
    return (int64_t)(uintptr_t)der;
}

/* ---- managed call-stack dump via il2cpp frame-at --------------------------
 * The stock capture path (libgcc _Unwind across frame-pointer-less code and
 * dl_iterate_phdr for .eh_frame_hdr) can't see our self-loaded libil2cpp, so
 * get_frame_at() comes up empty.  il2cpp_override_stack_backtrace(func) lets
 * us supply the native backtrace ourselves: func(void **buf, int max) fills
 * raw return addresses, il2cpp resolves them to MethodInfo* itself.  Walk the
 * x29 frame chain -- il2cpp codegen/libunity/our loader all keep frame
 * pointers on arm64. */
static int gds_addr_mapped(uintptr_t a);
static int gds_fp_backtrace(void **buf, int max)
{
    /* The x29 chain dies inside libunity's -fomit-frame-pointer frames, so
     * instead scan the raw stack for return addresses that land inside
     * libil2cpp's code: those are the managed frames' return sites (all
     * game logic is compiled into libil2cpp).  il2cpp resolves each IP to a
     * MethodInfo itself (garbage candidates resolve to nothing and are
     * skipped by its visitor), which gives us the full managed call chain. */
    static uint8_t *code_lo, *code_hi;
    if (!code_lo) {
        nx_mod *il2cpp = nx_find_mod("libil2cpp.so");
        if (!il2cpp)
            return 0;
        /* executable program headers cover the generated managed code;
         * use the module span (conservative superset) as the filter */
        code_lo = il2cpp->base;
        code_hi = il2cpp->base + il2cpp->span;
    }
    uintptr_t *sp = (uintptr_t *)__builtin_frame_address(0);
    uintptr_t here = (uintptr_t)sp;
    /* stack grows down: walk upward; stay well inside this thread's stack
     * (the dialog site is deep beneath Unity, plenty of mapped room) */
    uintptr_t limit = (here + 0x30000) & ~7UL;
    int n = 0;
    for (uintptr_t p = here & ~7UL; p < limit && n < max; p += 8) {
        if (!gds_addr_mapped(p + 8))
            break;
        uintptr_t v = *(uintptr_t *)p;
        if (v >= (uintptr_t)code_lo && v < (uintptr_t)code_hi)
            buf[n++] = (void *)v;
    }
    static int bt_dbg = 3;
    if (bt_dbg-- > 0) {
        nx_mod *il2cpp = nx_find_mod("libil2cpp.so");
        int inited = -1;
        if (il2cpp)
            inited = *(volatile uint8_t *)((uint8_t *)il2cpp->base +
                                           0x20077e0);
        fprintf(stderr,
                "[gds] backtrace: %d candidates in [%p,%p) sp=%p max=%d "
                "init=%d first=%p last=%p\n",
                n, code_lo, code_hi, (void *)here, max, inited,
                n ? buf[0] : 0, n ? buf[n - 1] : 0);
    }
    return n;
}

static int gds_bt_override_installed;

static struct {
    const char *(*method_get_name)(void *);
    void *(*method_get_class)(void *);
    const char *(*class_get_name)(void *);
    const char *(*class_get_namespace)(void *);
    int n, cap;
} g_framecb;

static void gds_framecb(const void *entry, void *ud)
{
    (void)ud;
    if (!entry || g_framecb.n >= g_framecb.cap)
        return;
    void *mi = *(void * const *)entry;
    if ((uintptr_t)mi < 0x10000)
        return;
    void *kl = g_framecb.method_get_class
                   ? g_framecb.method_get_class(mi)
                   : NULL;
    const char *ns = kl ? g_framecb.class_get_namespace(kl) : "?";
    const char *cn = kl ? g_framecb.class_get_name(kl) : "?";
    const char *mn = g_framecb.method_get_name(mi);
    fprintf(stderr, "  P%02d %s%s%s:%s\n", g_framecb.n++,
            ns ? ns : "", (ns && *ns) ? "." : "", cn ? cn : "?",
            mn ? mn : "?");
}

static void gds_install_bt_override(nx_mod *il2cpp)
{
    if (gds_bt_override_installed)
        return;
    void (*override_bt)(void *) =
        nx_lookup_in(il2cpp, "il2cpp_override_stack_backtrace");
    if (override_bt) {
        override_bt((void *)gds_fp_backtrace);
        gds_bt_override_installed = 1;
        fprintf(stderr, "[gds] installed frame-pointer backtrace override\n");
    }
}

/* ---- managed exception sniffing ------------------------------------------
 * Scan raw memory (current stack) for words whose [0] is an Il2CppClass*
         * derived from System.Exception.  The klass pointer is validated by
     * bouncing through the metadata: object_get_class gives the same class for
     * live objects; IsAssignableFrom does the rest.  Yesterday's dialog
     * handlers catch the exception in managed space right before us, so a live
     * copy is almost always still on the stack. */
static int gds_addr_mapped(uintptr_t a)
{
    static struct {
        uintptr_t lo, hi;
    } ranges[320];
    static int nranges = -1;
    if (nranges < 0) {
        nranges = 0;
        FILE *f = fopen("/proc/self/maps", "r");
        if (f) {
            char line[512];
            while (fgets(line, sizeof line, f) && nranges < 320) {
                unsigned long long lo, hi;
                if (sscanf(line, "%llx-%llx", &lo, &hi) == 2) {
                    ranges[nranges].lo = lo;
                    ranges[nranges].hi = hi;
                    nranges++;
                }
            }
            fclose(f);
        }
    }
    for (int i = 0; i < nranges; i++)
        if (a >= ranges[i].lo && a < ranges[i].hi)
            return 1;
    return 0;
}

static int gds_is_exception_object(nx_mod *il2cpp, uintptr_t cand,
                                   void *exc_class)
{
    if (!gds_addr_mapped(cand))
        return 0;
    static void *(*object_get_class)(const void *);
    static int (*class_is_assignable_from)(void *, const void *);
    if (!object_get_class)
        object_get_class = nx_lookup_in(il2cpp, "il2cpp_object_get_class");
    if (!class_is_assignable_from)
        class_is_assignable_from =
            nx_lookup_in(il2cpp, "il2cpp_class_is_assignable_from");
    if (!object_get_class || !class_is_assignable_from)
        return 0;
    void *klass = *(void * const *)cand; /* validated mapped by caller */
    if ((uintptr_t)klass < 0x10000)
        return 0;
    /* cheap pre-filter: klass must come from libil2cpp's data/rodata span */
    if ((uintptr_t)klass < (uintptr_t)il2cpp->base ||
        (uintptr_t)klass >= (uintptr_t)(il2cpp->base + il2cpp->span))
        return 0;
    return class_is_assignable_from(exc_class, (const void *)cand);
}

static void gds_sniff_exceptions(const char *why)
{
    nx_mod *il2cpp = nx_find_mod("libil2cpp.so");
    if (!il2cpp)
        return;
    void *(*get_corlib)(void) = nx_lookup_in(il2cpp, "il2cpp_get_corlib");
    void *(*class_from_name)(const void *, const char *, const char *) =
        nx_lookup_in(il2cpp, "il2cpp_class_from_name");
    void *(*image_get_class_count)(const void *) =
        nx_lookup_in(il2cpp, "il2cpp_image_get_class_count");
    void *(*image_get_class)(const void *, size_t) =
        nx_lookup_in(il2cpp, "il2cpp_image_get_class");
    void *(*class_get_parent)(void *) =
        nx_lookup_in(il2cpp, "il2cpp_class_get_parent");
    const char *(*class_get_name)(void *) =
        nx_lookup_in(il2cpp, "il2cpp_class_get_name");
    if (!get_corlib || !class_from_name || !class_get_parent ||
        !class_get_name)
        return;
    struct Il2CppStringHead {
        void *klass;
        void *monitor;
        int32_t length;
        uint16_t chars[1];
    };
    void *(*object_get_class)(const void *) =
        nx_lookup_in(il2cpp, "il2cpp_object_get_class");
    const void *corlib = get_corlib();
    void *exc = corlib ? class_from_name(corlib, "System", "Exception")
                       : NULL;
    if (!exc)
        return;
    uintptr_t *sp = (uintptr_t *)__builtin_frame_address(0);
    uintptr_t here = (uintptr_t)sp;
    uintptr_t limit = (here + 0x60000) & ~7UL;
    int hits = 0;
    fprintf(stderr, "[gds] exception sniff (%s):\n", why ? why : "?");
    for (uintptr_t p = here & ~7UL; p < limit && hits < 12; p += 8) {
        if (!gds_addr_mapped(p + 8))
            break; /* end of the contiguous stack mapping */
        uintptr_t cand = *(uintptr_t *)p;
        if (cand < 0x10000 || cand > 0x800000000000 || (cand & 7))
            continue;
        if (!gds_is_exception_object(il2cpp, cand, exc))
            continue;
        /* found an exception object (or a stack slot that aliases one);
         * walk up its class chain for the concrete type name */
        void *k = object_get_class((const void *)cand);
        fprintf(stderr, "  exc@%p slot=%p class=", (void *)cand, (void *)p);
        for (void *kk = k; kk; kk = class_get_parent(kk)) {
            const char *n = class_get_name(kk);
            fprintf(stderr, "%s%s", n ? n : "?",
                    class_get_parent(kk) ? " < " : "\n");
        }
        /* System.Exception.message lives right past the object header:
         * className at +16, message/DisplayMessage at +24..+32 range */
        for (int off = 16; off <= 48; off += 8) {
            struct Il2CppStringHead *msg =
                *(struct Il2CppStringHead **)(cand + off);
            if ((uintptr_t)msg <= 0x10000 ||
                (uintptr_t)msg > 0x800000000000)
                continue;
            void *mk = object_get_class(msg);
            const char *mn = mk ? class_get_name(mk) : NULL;
            if (!mn || strcmp(mn, "String") != 0 || msg->length < 0 ||
                msg->length > 4000)
                continue;
            char b[120];
            int o = 0;
            while (o < 119 && o < msg->length) {
                uint16_t ch = msg->chars[o];
                b[o] = (ch >= 32 && ch < 127) ? (char)ch : '.';
                o++;
            }
            b[o] = 0;
            fprintf(stderr, "    [+%d] \"%s\"\n", off, b);
        }
        hits++;
    }
    fprintf(stderr, "[gds] exception sniff done, hits=%d\n", hits);
}

/* The two catch sites that call IApplication.Error (in Update at 0x1756330
 * and in OnGUI at 0x1758de8) both compare the caught exception against the
 * type in static slot [base+0x1ebf330].  Read the runtime value and print the
 * compared type name -- it tells us which exception family their error
 * reporter expects here. */
static void gds_dump_catch_type(nx_mod *il2cpp)
{
    void *(*class_from_type)(const void *) =
        nx_lookup_in(il2cpp, "il2cpp_class_from_type");
    const char *(*class_get_name)(void *) =
        nx_lookup_in(il2cpp, "il2cpp_class_get_name");
    const char *(*class_get_namespace)(void *) =
        nx_lookup_in(il2cpp, "il2cpp_class_get_namespace");
    void **slot = (void **)((uint8_t *)il2cpp->base + 0x1ebf330);
    void *t = slot ? *slot : NULL;
    fprintf(stderr, "[gds] catch-compare slot[0x1ebf330] = %p", t);
    if (t && class_from_type && class_get_name) {
        /* maybe it IS an Il2CppType* */
        uint8_t tb = ((uint8_t *)t)[10];
        fprintf(stderr, " (typebyte=%u)", tb);
        void *k = class_from_type(t);
        fprintf(stderr, " -> klass:%s.%s",
                k && class_get_namespace ? class_get_namespace(k) : "?",
                k ? class_get_name(k) : "?");
        /* maybe it's a live System.Type object instead */
        void *(*object_get_class)(const void *) =
            nx_lookup_in(il2cpp, "il2cpp_object_get_class");
        if (object_get_class) {
            void *ok = object_get_class(t);
            fprintf(stderr, " / as-object klass:%s.%s",
                    ok && class_get_namespace ? class_get_namespace(ok)
                                              : "?",
                    ok ? class_get_name(ok) : "?");
            /* System.Type.Name via runtime invoke */
            void *(*class_get_method_from_name)(void *, const char *,
                                                int) =
                nx_lookup_in(il2cpp, "il2cpp_class_get_method_from_name");
            void *(*runtime_invoke)(const void *, void *, void **,
                                    void **) =
                nx_lookup_in(il2cpp, "il2cpp_runtime_invoke");
            void *(*object_get_class2)(const void *) = object_get_class;
            if (class_get_method_from_name && runtime_invoke && k) {
                void *mi =
                    class_get_method_from_name(ok, "get_Name", 0);
                if (mi) {
                    void *exc2 = NULL;
                    void *r =
                        runtime_invoke(mi, t, NULL, &exc2);
                    if (r && !exc2) {
                        const uint16_t *(*chars)(const void *) =
                            nx_lookup_in(il2cpp, "il2cpp_string_chars");
                        if (chars) {
                            const uint16_t *u = chars(r);
                            char b[64];
                            int i = 0;
                            while (i < 63 && u[i]) {
                                b[i] = u[i] < 128 ? (char)u[i] : '?';
                                i++;
                            }
                            b[i] = 0;
                            fprintf(stderr, " / Name=\"%s\"", b);
                        }
                    } else if (exc2) {
                        fprintf(stderr, " / get_Name threw");
                    }
                }
            }
            (void)object_get_class2;
        }
    }
    fprintf(stderr, "\n");
}

static void gds_dump_managed_frames(const char *why)
{
    nx_mod *il2cpp = nx_find_mod("libil2cpp.so");
    if (!il2cpp)
        return;
    int (*get_frame_at)(int, void **) =
        nx_lookup_in(il2cpp, "il2cpp_current_thread_get_frame_at");
    int (*get_stack_depth)(void) =
        nx_lookup_in(il2cpp, "il2cpp_current_thread_get_stack_depth");
    const char *(*method_get_name)(void *) =
        nx_lookup_in(il2cpp, "il2cpp_method_get_name");
    void *(*method_get_class)(void *) =
        nx_lookup_in(il2cpp, "il2cpp_method_get_class");
    const char *(*class_get_name)(void *) =
        nx_lookup_in(il2cpp, "il2cpp_class_get_name");
    const char *(*class_get_namespace)(void *) =
        nx_lookup_in(il2cpp, "il2cpp_class_get_namespace");
    if (!get_frame_at || !method_get_name || !method_get_class ||
        !class_get_name || !class_get_namespace) {
        fprintf(stderr, "[gds] frame dump: il2cpp frame API missing\n");
        return;
    }
    gds_install_bt_override(il2cpp);
    int depth = get_stack_depth ? get_stack_depth() : -1;
    fprintf(stderr, "[gds] managed frames (%s), pushed-depth=%d:\n",
            why ? why : "?", depth);
    /* The per-thread pushed-frame array (32-byte entries, MethodInfo* at
     * +0) covers the il2cpp frames nearest the native boundary -- exactly
     * the frames libunwind-blind get_frame_at misses. */
    if (depth > 0) {
        void (*walk)(void (*)(const void *, void *), void *) =
            nx_lookup_in(il2cpp, "il2cpp_current_thread_walk_frame_stack");
        if (walk) {
            g_framecb.method_get_name = method_get_name;
            g_framecb.method_get_class = method_get_class;
            g_framecb.class_get_name = class_get_name;
            g_framecb.class_get_namespace = class_get_namespace;
            g_framecb.n = 0;
            g_framecb.cap = depth + 2;
            walk(gds_framecb, &g_framecb);
        }
    }
    for (int i = 0; i < 60; i++) {
        void *mi = NULL;
        int ok = get_frame_at(i, &mi);
        if (!ok || !mi)
            break;
        void *kl = method_get_class(mi);
        const char *ns = kl ? class_get_namespace(kl) : "?";
        const char *cn = kl ? class_get_name(kl) : "?";
        fprintf(stderr, "  #%02d %s%s%s:%s\n", i,
                ns ? ns : "", (ns && *ns) ? "." : "",
                cn ? cn : "?", method_get_name(mi) ? method_get_name(mi)
                                                  : "?");
    }
    fprintf(stderr, "[gds] end of frames\n");
}

static void gds_jni_ring_dump(void);

static int64_t j_Kairo_showDialog(jctx *c)
{
    int type = jarg_int(c);
    jobj *title = jarg_obj(c);
    jobj *message = jarg_obj(c);
    jobj *audience = jarg_obj(c);
    int result = 0;
    const char *v = getenv("GDS_DIALOG_RESULT");
    if (v && *v)
        result = atoi(v);
    fprintf(stderr, "[jni] KairoshowDialog type=%d title=\"%s\" message=\"%s\" "
            "audience=%s -> %d%s\n", type,
            title && title->str ? title->str : "(null)",
            message && message->str ? message->str : "(null)",
            audience && audience->cls ? audience->cls : "(none)",
            result, (v && *v) ? " (GDS_DIALOG_RESULT)" : "");
    fflush(stderr);
    /* 0.86: on the generic error dialog, dump the recently-dispatched JNI
     * calls so the failing contract is visible without GDS_JNILOG=1. */
    {
        static int ring_dumped;
        const char *tt = title && title->str ? title->str : "";
        const char *mm = message && message->str ? message->str : "";
        if (!ring_dumped && (strstr(tt, "rror") || strstr(mm, "rror"))) {
            ring_dumped = 1;
            gds_jni_ring_dump();
        }
    }
    /* Zombie archaeology probes (audience/catch-type sniff, frame walker)
     * kill the render thread with SEGVs (gds_is_exception_object funnel) and
     * have never produced evidence.  Off by default; GDS_PROBE_JUNK=1 re-arms.
     * The brk catcher in main.c (GDS_TRAP_AT) replaced them. */
    if (getenv("GDS_PROBE_JUNK")) {
        gds_probe_managed_object(audience, "showDialog audience");
        gds_sniff_exceptions("Kairosoft showDialog");
        {
            nx_mod *il2cpp = nx_find_mod("libil2cpp.so");
            if (il2cpp)
                gds_dump_catch_type(il2cpp);
        }
        gds_dump_managed_frames("Kairosoft showDialog");
        if (getenv("GDS_STACKTRACE"))
            gds_managed_stacktrace("Kairosoft showDialog");
    }
    return result;
}

/* ------------------------------------------------ kairo FEP input panel ----
 * The game's own text-entry contract (name entry etc.), recovered from
 * global-metadata string tables + libil2cpp disasm:
 *   managed kairo.unity.native.KairoPlugin        dex kairo/android/plugin/Utility
 *     ShowFepPanel   (fepTitle_,fepText_,fepMode_,fepPositive_,fepNegative_,fepMaxLength_)
 *                    -> showInputPanel
 *     StartFepPanel  -> startInputPanel
 *     IsFepPanelFinish (polled each frame) -> isEndInputPanel
 *     GetFepPanelResult                    -> getResultInputPanel
 * kairo.unity.panel.FepPanel::Update @0x17f4974 (disasm-verified) consumes
 * the result as a 2-element String array: Length must be >= 2 (else their
 * bounds check throws), array[1] is stored to the TextBox display text, and
 * array[0] selects the listener branch: non-NULL = positive (OK), NULL =
 * negative (cancel).  We answer all four with the Terraria-style gamepad
 * OSK in osk.c.  Routed by NAME so any dex signature works on first boot. */
static int64_t j_kairo_InputPanel(jctx *c)
{
    const jmethod *m = c->m;
    const char *sig = m && m->sig ? m->sig : "()V";
    /* Walk the args by the signature's parameter types.  Unity's managed
     * AndroidJavaObject.CallStatic always arrives as the *A variant, so
     * c->args is a proper jvalue array we can index safely. */
    jobj *objs[8];
    long long nums[8];
    int no = 0, ni = 0;
    const char *p = strchr(sig, '(');
    for (p = p ? p + 1 : sig; *p && *p != ')'; ) {
        if (no >= 8 || ni >= 8) break;
        if (*p == '[') {
            p++;
            continue;
        } else if (*p == 'L') {
            objs[no++] = jarg_obj(c);
            while (*p && *p != ';') p++;
            if (*p == ';') p++;
        } else if (*p == 'F') {
            nums[ni++] = (long long)jarg_float(c);
            p++;
        } else if (*p == 'J') {
            nums[ni++] = jarg_long(c);
            p++;
        } else {
            nums[ni++] = jarg_int(c);
            p++;
        }
    }
    const char *name = m ? m->name : "?";
    /* Device-proven dex surface (0.84.0 run): the panel OPENS via
     * startInputPanel(String title, String text, int mode,
     *                 String positive, String negative, int maxLength)
     * and POLLS via isInputPanelFinish()Z.  The metadata-guessed
     * showInputPanel/isEndInputPanel names never fire; keep both handled
     * (other kairo builds may use them). */
    int is_show  = !strcmp(name, "startInputPanel") ||
                   !strcmp(name, "showInputPanel");
    int is_poll  = !strcmp(name, "isInputPanelFinish") ||
                   !strcmp(name, "isEndInputPanel");
    int is_fetch = !strcmp(name, "getResultInputPanel") ||
                   !strcmp(name, "getInputPanelResult");
    if (is_show) {
        /* strings arrive in dex declaration order (title, text, positive,
         * negative); the ints are mode + maxLength (the LAST small
         * positive is maxLength). */
        const char *title = (no >= 1 && objs[0] && objs[0]->str) ? objs[0]->str : "";
        const char *text  = (no >= 2 && objs[1] && objs[1]->str) ? objs[1]->str : "";
        int maxlen = 16;
        for (int i = 0; i < ni; i++)
            if (nums[i] > 0 && nums[i] <= 64)
                maxlen = (int)nums[i];
        fprintf(stderr, "[osk] Utility.%s%s title=\"%s\" text=\"%s\""
                        " nstr=%d nnum=%d max=%d\n",
                name, sig, title, text, no, ni, maxlen);
        fflush(stderr);
        gds_osk_open(title, text, maxlen);
        return 1;
    }
    if (is_poll) {
        int done = gds_osk_done();
        static unsigned seen_notdone = 6;
        if (done || seen_notdone) {
            if (!done) seen_notdone--;
            fprintf(stderr, "[osk] Utility.%s%s -> %d\n", name, sig, done);
            fflush(stderr);
        }
        return done;
    }
    if (is_fetch) {
        /* Return the shape the CALLER asked for.  The device-proven dex
         * surface (0.85.0) requests getInputPanelResult()Ljava/lang/String;
         * -- a single string (null = canceled).  0.84.x answered with a
         * String[2] to a ()Ljava/lang/String; requester; the mismatch is
         * the prime suspect for the DONE-crash (signal 11 inside loader2,
         * pc in ReflectionHelper.getConstructorID while the managed side
         * marshaled the result).  Keep the array form for any caller that
         * actually declares ()[Ljava/lang/String;. */
        int want_array = sig && strstr(sig, "[Ljava/lang/String;");
        if (want_array) {
            jobj *arr = j_NewObjectArray(NULL, 2, mk_class("java/lang/String"), NULL);
            if (arr) {
                arr->cls = "[Ljava/lang/String;";
                if (gds_osk_result_ok()) {
                    /* non-NULL [0] = positive branch (FepPanel disasm: cbz
                     * on [0x20] picks the negative listener) */
                    arr->elems[0] = mk_string("positive");
                }
                arr->elems[1] = mk_string(gds_osk_text());
            }
            fprintf(stderr, "[osk] Utility.%s%s -> { %s, \"%s\" }\n",
                    name, sig, gds_osk_result_ok() ? "\"positive\"" : "null",
                    gds_osk_text());
            fflush(stderr);
            return (int64_t)(uintptr_t)arr;
        }
        jobj *s = gds_osk_result_ok() ? mk_string(gds_osk_text()) : NULL;
        fprintf(stderr, "[osk] Utility.%s%s -> %s\"%s\"\n", name, sig,
                gds_osk_result_ok() ? "" : "(null) ",
                gds_osk_result_ok() ? gds_osk_text() : "canceled");
        fflush(stderr);
        return (int64_t)(uintptr_t)s;
    }
    /* One log per unknown member name (the isInputPanelFinish-per-frame
     * spam of 0.84.0 drowned the real evidence). */
    static char seen_names[4][64];
    static int seen_n;
    int seen = 0;
    for (int i = 0; i < seen_n; i++)
        if (!strcmp(seen_names[i], name)) seen = 1;
    if (!seen) {
        if (seen_n < 4) {
            snprintf(seen_names[seen_n], 64, "%s", name);
            seen_n++;
        }
        fprintf(stderr, "[osk] Utility.%s%s -> 0 (unhandled InputPanel member)\n",
                name, sig);
        fflush(stderr);
    }
    return 0;
}

/* ---- display metrics chain (Kairosoft kairo.unity.util.DisplayMetrics) ----
 * Boot evidence (qemu NullGL run6): the game called
 *   UnityPlayer.getWindowManager()Ljava/lang/Object;   -> unhandled -> NULL
 * and the managed side immediately NRE'd in DisplayMetrics.Update(); the
 * exception cascaded through Canvas and left IApplication.Awake() dead while
 * frames kept rendering.  Emulate the Android side exactly as the game uses
 * it: getWindowManager -> WindowManager.getDefaultDisplay -> Display,
 * Display.getMetrics(metrics), then field reads on the metrics object. */
static jobj *window_manager_object;
static jobj *display_object;

extern int egl_shim_screen_w(void);
extern int egl_shim_screen_h(void);

/* Defaults match the R36S panel (640x480, ~230dpi) and remain sane even if a
 * field is read before getMetrics ran.  GDS_DPI overrides every density
 * value at once (DisplayMetrics densityDpi + density + x/ydpi, and Unity's
 * Screen.dpi in input.c reads the same knob) for the tab-size experiment:
 * the game's fixed little bottom tabs on a 640x480 panel look like a
 * density decision, and Screen.dpi=160 vs densityDpi=240 didn't agree. */
static struct {
    int w, h, dpi;
    float density, scaled, xdpi, ydpi;
} g_metrics = { 640, 480, 240, 1.5f, 1.5f, 240.0f, 240.0f };

int gds_dpi_override(void)
{
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("GDS_DPI");
        v = e ? atoi(e) : 0;
        if (v > 0) {
            g_metrics.dpi = v;
            g_metrics.density = g_metrics.scaled = v / 160.0f;
            g_metrics.xdpi = g_metrics.ydpi = (float)v;
            fprintf(stderr, "[jni] GDS_DPI=%d -> density=%.2f (tabs/UI scale test)\n",
                    v, g_metrics.density);
            fflush(stderr);
        }
    }
    return v;
}

static int64_t j_Unity_getWindowManager(jctx *c)
{
    (void)c;
    if (!window_manager_object)
        window_manager_object = mk_object("android/view/WindowManager");
    return (int64_t)(uintptr_t)window_manager_object;
}

static int64_t j_Wmgr_getDefaultDisplay(jctx *c)
{
    (void)c;
    if (!display_object)
        display_object = mk_object("android/view/Display");
    return (int64_t)(uintptr_t)display_object;
}

static int64_t j_Display_getMetrics(jctx *c)
{
    (void)jarg_obj(c);           /* the android.util.DisplayMetrics instance */
    int w = egl_shim_screen_w(), h = egl_shim_screen_h();
    if (w > 0) g_metrics.w = w;
    if (h > 0) g_metrics.h = h;
    (void)gds_dpi_override();
    JT("Display.getMetrics -> %dx%d dpi=%d", g_metrics.w, g_metrics.h,
       g_metrics.dpi);
    return 0;
}

/* android/media/AudioManager chain -- libunity's FMOD OpenSL output queries
 * the platform's preferred output rate/chunk BEFORE creating the OpenSL
 * engine.  The literal JNI names are in libunity.so: "AUDIO_SERVICE",
 * "getSystemService", "android.media.property.OUTPUT_SAMPLE_RATE",
 * "android.media.property.OUTPUT_FRAMES_PER_BUFFER",
 * "android/media/AudioManager".  Unserved, the audio init folded in on
 * itself before ever dlsym'ing slCreateEngine -- that is why 0.79 logged
 * zero [SL] lines and total silence. */
static jobj *audio_manager_object;
static int64_t j_Context_AUDIO_SERVICE(jctx *c)
{
    (void)c;
    return (int64_t)(uintptr_t)mk_string("audio");
}
/* Static String fields on android/media/AudioManager (libunity rodata holds
 * the literal Java field NAMES "PROPERTY_OUTPUT_SAMPLE_RATE" (@0xc81f3) and
 * "PROPERTY_OUTPUT_FRAMES_PER_BUFFER" (@0x10e70a) next to the class string
 * @0xb6b0f).  FMOD does GetStaticFieldID + GetStaticObjectField to fetch the
 * PROPERTY KEY before calling getProperty(key).  Unbound, the key reached
 * getProperty as NULL/"" -- that is exactly what the 0.82 device log showed
 * (three getProperty("") calls) -- and FMOD bailed before ever calling
 * dlopen("libOpenSLES.so").  Hand back the real key constants Android
 * defines. */
static int64_t j_AudioManager_PROP_SAMPLE_RATE(jctx *c)
{
    (void)c;
    fprintf(stderr, "[audio] static field AudioManager.PROPERTY_OUTPUT_SAMPLE_RATE"
                    " -> \"android.media.property.OUTPUT_SAMPLE_RATE\"\n");
    fflush(stderr);
    return (int64_t)(uintptr_t)mk_string(
        "android.media.property.OUTPUT_SAMPLE_RATE");
}
static int64_t j_AudioManager_PROP_FRAMES_PER_BUFFER(jctx *c)
{
    (void)c;
    fprintf(stderr, "[audio] static field AudioManager.PROPERTY_OUTPUT_FRAMES_PER_BUFFER"
                    " -> \"android.media.property.OUTPUT_FRAMES_PER_BUFFER\"\n");
    fflush(stderr);
    return (int64_t)(uintptr_t)mk_string(
        "android.media.property.OUTPUT_FRAMES_PER_BUFFER");
}
static int64_t j_AudioManager_getProperty(jctx *c)
{
    jobj *key = jarg_obj(c);
    const char *k = key && key->str ? key->str : "";
    /* 0.82: the 0.81 device log showed the key string unreadable ("") -- so
     * FMOD got "44100" for BOTH properties.  fmod_output_opensl.cpp queries
     * OUTPUT_SAMPLE_RATE first, then OUTPUT_FRAMES_PER_BUFFER; when the key
     * cannot be read, fall back to the call-order answer.  Log EVERY call
     * unconditionally: this is the last working breadcrumb before FMOD
     * should dlopen libOpenSLES.so and dlsym slCreateEngine. */
    static unsigned prop_calls;
    const char *v;
    if (strstr(k, "FRAMES_PER_BUFFER")) v = "256";
    else if (strstr(k, "SAMPLE_RATE"))   v = "44100";
    else if (*k)                          v = "44100";
    else                                  v = (prop_calls % 2 == 0) ? "44100" : "256";
    prop_calls++;
    fprintf(stderr, "[audio] AudioManager.getProperty(\"%s\") -> \"%s\" (#%u)\n",
            k, v, prop_calls);
    fflush(stderr);
    return (int64_t)(uintptr_t)mk_string(v);
}

static int64_t j_Context_getSystemService(jctx *c)
{
    jobj *name = jarg_obj(c);
    const char *s = name && name->str ? name->str : "(null)";
    if (strcmp(s, "window") == 0) {
        JT("getSystemService(\"window\") -> WindowManager");
        return j_Unity_getWindowManager(c);
    }
    if (strcmp(s, "audio") == 0) {
        if (!audio_manager_object)
            audio_manager_object = mk_object("android/media/AudioManager");
        JT("getSystemService(\"audio\") -> AudioManager");
        {
            static int seen;
            if (!seen) {
                seen = 1;
                fprintf(stderr, "[audio] getSystemService(\"audio\") -> AudioManager\n");
            }
        }
        return (int64_t)(uintptr_t)audio_manager_object;
    }
    /* Every other service stays NULL, exactly as before this binding: callers
     * already handled the missing-service path. */
    JT("getSystemService(%s) -> null", s);
    return 0;
}

/* android/opengl/GLES20 Java bridge: KairoPlugin.SupportsDepth24() calls
 * GLES20.glGetString(GLES20.GL_EXTENSIONS) through JNI and parses it; unhandled
 * it returned NULL and the managed .Contains() NRE'd.  Answers come from the
 * strings captured on the real context (device) or the NullGL set (headless). */
extern const char *gds_gl_string_for_jni(unsigned name);
static int64_t j_GLES20_GL_EXTENSIONS(jctx *c) { (void)c; return 0x1F03; }
static int64_t j_GLES20_glGetString(jctx *c)
{
    int name = jarg_int(c);
    const char *s = gds_gl_string_for_jni((unsigned)name);
    JT("GLES20.glGetString(0x%x) -> \"%.*s%s\"", name,
       70, s ? s : "", s && strlen(s) > 70 ? "..." : "");
    return (int64_t)(uintptr_t)(s ? mk_string(s) : NULL);
}

/* Display.getRealSize(Point): pack w/h into the Point's prim slot; the x/y
 * field getters unpack it.  Points built elsewhere keep prim=0 -> x=y=0,
 * i.e. exactly the old unhandled->0 behaviour for every other Point. */
extern int egl_shim_screen_w(void);
extern int egl_shim_screen_h(void);
static int64_t j_Display_getRealSize(jctx *c)
{
    jobj *point = jarg_obj(c);
    int64_t w = egl_shim_screen_w(), h = egl_shim_screen_h();
    if (w <= 0) w = 640;
    if (h <= 0) h = 480;
    if (point)
        point->prim = (w << 32) | (h & 0xffffffff);
    JT("Display.getRealSize -> %lldx%lld", (long long)w, (long long)h);
    return 0;
}
static int64_t j_Point_x(jctx *c) { return (int32_t)(c->self ? (c->self->prim >> 32) : 0); }
static int64_t j_Point_y(jctx *c) { return (int32_t)(c->self ? (c->self->prim & 0xffffffff) : 0); }

/* android/os/Message.what: obtainMessage already stores what in prim. */
static int64_t j_Message_what(jctx *c) { return c->self ? c->self->prim : 0; }

/* Handler.postDelayed: claiming the post FAILED (old unhandled->0) makes the
 * caller retry/error; our fake looper accepts and reports success. */
static int64_t j_Handler_postDelayed(jctx *c) { (void)c; return 1; }

static int64_t j_DM_width(jctx *c)  { (void)c; return g_metrics.w; }
static int64_t j_DM_height(jctx *c) { (void)c; return g_metrics.h; }
static int64_t j_DM_dpi(jctx *c)    { (void)c; return g_metrics.dpi; }
static int64_t float_bits(float f) { int32_t b; memcpy(&b, &f, sizeof b); return b; }
static int64_t j_DM_fdensity(jctx *c) { (void)c; return float_bits(g_metrics.density); }
static int64_t j_DM_fscaled(jctx *c)  { (void)c; return float_bits(g_metrics.scaled); }
static int64_t j_DM_fxdpi(jctx *c)    { (void)c; return float_bits(g_metrics.xdpi); }
static int64_t j_DM_fydpi(jctx *c)    { (void)c; return float_bits(g_metrics.ydpi); }

static int64_t j_Armory_getUnityPlayer(jctx *c)
{
    (void)c;
    return (int64_t)(uintptr_t)permission_plugin_object;
}

static int64_t j_Armory_getOsVersion(jctx *c)
{
    (void)c;
    return 33;
}

static int64_t j_Armory_getTargetSdkVersion(jctx *c)
{
    (void)c;
    return 36;
}

static int64_t j_Armory_hasPermission(jctx *c)
{
    jobj *permission = jarg_obj(c);
    JT("Armory permission %s -> granted",
       permission && permission->str ? permission->str : "(unknown)");
    return 1;
}

static int64_t j_Permission_canRequest(jctx *c)
{
    (void)c;
    return 1;
}

static int64_t j_Permission_noRationale(jctx *c)
{
    (void)jarg_obj(c);
    return 0;
}

static int64_t j_Permission_noop(jctx *c)
{
    if (strstr(c->m->sig, "Ljava/lang/String;"))
        (void)jarg_obj(c);
    return 0;
}

static int64_t j_AssetManager_open(jctx *c)
{
    jobj *name = jarg_obj(c);
    const char *rel = name && name->str ? name->str : "";
    while (*rel == '/')
        rel++;
    if (strncmp(rel, "assets/", 7) == 0)
        rel += 7;

    char path[1280];
    snprintf(path, sizeof path, "%s/%s", gds_datadir, rel);
    struct stat st;
    if (stat(path, &st) != 0) {
        JT("AssetManager.open(\"%s\") -> NULL", rel);
        return 0;
    }
    jobj *stream = mk_object("java/io/InputStream");
    stream->str = strdup(path);
    stream->len = (int)st.st_size;
    JT("AssetManager.open(\"%s\") -> %s", rel, path);
    return (int64_t)(uintptr_t)stream;
}

static int64_t j_Scanner_next(jctx *c)
{
    const char *path = c->self ? c->self->str : NULL;
    if (!path)
        return (int64_t)(uintptr_t)mk_string("");
    FILE *f = fopen(path, "rb");
    if (!f)
        return (int64_t)(uintptr_t)mk_string("");
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    rewind(f);
    if (n < 0 || n > 1024 * 1024) {
        fclose(f);
        return (int64_t)(uintptr_t)mk_string("");
    }
    char *buf = calloc((size_t)n + 1, 1);
    if (buf && n)
        (void)fread(buf, 1, (size_t)n, f);
    fclose(f);
    jobj *s = mk_string(buf ? buf : "");
    free(buf);
    return (int64_t)(uintptr_t)s;
}

static int64_t j_Environment_mounted(jctx *c)
{
    (void)c;
    return (int64_t)(uintptr_t)mk_string("mounted");
}

static int64_t j_String_equals(jctx *c)
{
    jobj *other = jarg_obj(c);
    const char *a = c->self && c->self->str ? c->self->str : "";
    const char *b = other && other->str ? other->str : "";
    return strcmp(a, b) == 0;
}

static int64_t j_StringBuilder_toString(jctx *c)
{
    return (int64_t)(uintptr_t)mk_string(
        c->self && c->self->str ? c->self->str : "");
}

static int uri_unreserved(unsigned char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '-' || c == '_' ||
           c == '.' || c == '~';
}

static int64_t j_Uri_encode(jctx *c)
{
    jobj *input = jarg_obj(c);
    const char *text = input && input->str ? input->str : "";
    size_t length = strlen(text);
    char *encoded = malloc(length * 3 + 1);
    static const char hex[] = "0123456789ABCDEF";
    size_t out = 0;
    for (size_t i = 0; i < length; i++) {
        unsigned char byte = (unsigned char)text[i];
        if (uri_unreserved(byte)) {
            encoded[out++] = (char)byte;
        } else {
            encoded[out++] = '%';
            encoded[out++] = hex[byte >> 4];
            encoded[out++] = hex[byte & 15];
        }
    }
    encoded[out] = '\0';
    jobj *result = mk_string(encoded);
    free(encoded);
    return (int64_t)(uintptr_t)result;
}

static int uri_hex(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int64_t j_Uri_decode(jctx *c)
{
    jobj *input = jarg_obj(c);
    const char *text = input && input->str ? input->str : "";
    size_t length = strlen(text);
    char *decoded = malloc(length + 1);
    size_t out = 0;
    for (size_t i = 0; i < length; i++) {
        int high, low;
        if (text[i] == '%' && i + 2 < length &&
            (high = uri_hex(text[i + 1])) >= 0 &&
            (low = uri_hex(text[i + 2])) >= 0) {
            decoded[out++] = (char)((high << 4) | low);
            i += 2;
        } else {
            decoded[out++] = text[i];
        }
    }
    decoded[out] = '\0';
    jobj *result = mk_string(decoded);
    free(decoded);
    return (int64_t)(uintptr_t)result;
}

/* Unity reads android.os.Build to pick device-specific workarounds, and the
 * first thing it does with MANUFACTURER is strcasecmp(field, "Amazon") -- with
 * the field absent that faults inside strcasecmp before anything is drawn.
 *
 * These values describe this device, and deliberately match no vendor Unity
 * has a workaround for: a name borrowed from a real manufacturer would switch
 * on a code path written for someone else's hardware.  The field name arrives
 * in the context, so one handler serves the whole class.
 */
static const struct { const char *name, *value; } build_fields[] = {
    { "MANUFACTURER", "NextOS" },
    { "BRAND",        "NextOS" },
    { "MODEL",        "NextOS-Retro-Elite" },
    { "PRODUCT",      "nextos" },
    { "DEVICE",       "amlogic" },
    { "BOARD",        "amlogic" },
    { "HARDWARE",     "amlogic" },
    { "ID",           "NextOS" },
    { "TYPE",         "user" },
    { "TAGS",         "release-keys" },
    { "FINGERPRINT",  "NextOS/nextos/amlogic:13/NextOS/4.8.2:user/release-keys" },
    { "RELEASE",      "13" },
    { "CODENAME",     "REL" },
    { "INCREMENTAL",  "4.8.2" },
    { "SERIAL",       "unknown" },
};

static int64_t j_Build_string(jctx *c)
{
    const char *name = c->m ? c->m->name : NULL;
    if (name)
        for (size_t i = 0; i < sizeof build_fields / sizeof *build_fields; i++)
            if (strcmp(build_fields[i].name, name) == 0)
                return (int64_t)(uintptr_t)mk_string(build_fields[i].value);
    /* Never NULL: the caller passes these straight to string functions. */
    return (int64_t)(uintptr_t)mk_string("");
}

/* API 33 is the platform this 2022.3 player was built against.  Zero would read
 * as older than every gate Unity checks and take paths this build never
 * compiled. */
static int64_t j_Build_SDK_INT(jctx *c)
{
    (void)c;
    return 33;
}

/* Unity's preferences, enough for the questions it asks before the first frame.
 *
 * The one that matters is the GLES level warning.  Before showing it, Unity asks
 * `getPreferences(0).getBoolean("gles-api-check", false)` -- "has the user
 * already ticked do-not-show-again?".  Answer false and it builds an
 * AlertDialog, hands it to Activity.runOnUiThread and blocks the render thread
 * on a condition variable waiting for a button click.  There is no Java UI
 * thread here to deliver one, so frame 1 never returns.
 *
 * Answering true is not a trick to dodge a check: this port runs GLES2 on
 * Mali-450 deliberately, so the warning is one we have already read.
 */
enum pref_type {
    PREF_INT = 1,
    PREF_FLOAT,
    PREF_STRING,
    PREF_BOOL,
    PREF_BYTES,             /* kairo Utility.putPreference byte[] payloads */
};

typedef struct {
    char *key;
    enum pref_type type;
    int32_t integer;
    float real;
    char *string;
    unsigned char *blob;
    uint32_t blob_len;
} pref_entry;

#define MAX_PREFS 1024
static pref_entry preferences[MAX_PREFS];
static int preferences_count;
static int preferences_loaded;
static pthread_mutex_t preferences_lock = PTHREAD_MUTEX_INITIALIZER;

static pref_entry *pref_find_locked(const char *key)
{
    for (int i = 0; i < preferences_count; i++)
        if (strcmp(preferences[i].key, key) == 0)
            return &preferences[i];
    return NULL;
}

static pref_entry *pref_set_locked(const char *key, enum pref_type type)
{
    pref_entry *entry = pref_find_locked(key);
    if (!entry) {
        if (preferences_count >= MAX_PREFS)
            return NULL;
        entry = &preferences[preferences_count++];
        memset(entry, 0, sizeof *entry);
        entry->key = strdup(key);
    }
    if (entry->type == PREF_STRING) {
        free(entry->string);
        entry->string = NULL;
    }
    if (entry->type == PREF_BYTES) {
        free(entry->blob);
        entry->blob = NULL;
        entry->blob_len = 0;
    }
    entry->type = type;
    return entry;
}

static int pref_remove_locked(const char *key)
{
    for (int i = 0; i < preferences_count; i++) {
        if (strcmp(preferences[i].key, key) == 0) {
            free(preferences[i].key);
            if (preferences[i].type == PREF_STRING)
                free(preferences[i].string);
            if (preferences[i].type == PREF_BYTES)
                free(preferences[i].blob);
            preferences[i] = preferences[--preferences_count];
            memset(&preferences[preferences_count], 0, sizeof(pref_entry));
            return 1;
        }
    }
    return 0;
}

static int pref_write(FILE *file, const void *data, size_t bytes)
{
    return fwrite(data, 1, bytes, file) == bytes;
}

static int pref_read(FILE *file, void *data, size_t bytes)
{
    return fread(data, 1, bytes, file) == bytes;
}

static int preferences_save_locked(void)
{
    char path[1200];
    char temporary[1200];
    snprintf(path, sizeof path, "%s/shared-preferences.bin", gds_home);
    snprintf(temporary, sizeof temporary,
             "%s/shared-preferences.bin.tmp", gds_home);
    FILE *file = fopen(temporary, "wb");
    if (!file)
        return 0;

    static const unsigned char magic[8] =
        { 'H', 'G', 'O', 'P', 'R', 'E', 'F', '1' };
    uint32_t count = (uint32_t)preferences_count;
    int ok = pref_write(file, magic, sizeof magic) &&
             pref_write(file, &count, sizeof count);
    for (int i = 0; ok && i < preferences_count; i++) {
        pref_entry *entry = &preferences[i];
        uint8_t type = (uint8_t)entry->type;
        uint32_t key_length = (uint32_t)strlen(entry->key);
        uint32_t value_length =
            entry->type == PREF_STRING
                ? (uint32_t)strlen(entry->string ? entry->string : "")
                : entry->type == PREF_BYTES
                    ? entry->blob_len
                    : (uint32_t)sizeof(uint32_t);
        ok = pref_write(file, &type, sizeof type) &&
             pref_write(file, &key_length, sizeof key_length) &&
             pref_write(file, &value_length, sizeof value_length) &&
             pref_write(file, entry->key, key_length);
        if (!ok)
            break;
        if (entry->type == PREF_STRING)
            ok = pref_write(file, entry->string ? entry->string : "",
                            value_length);
        else if (entry->type == PREF_BYTES)
            ok = value_length == 0 ||
                 pref_write(file, entry->blob, value_length);
        else if (entry->type == PREF_FLOAT)
            ok = pref_write(file, &entry->real, sizeof entry->real);
        else
            ok = pref_write(file, &entry->integer, sizeof entry->integer);
    }
    if (fflush(file) != 0 || fsync(fileno(file)) != 0)
        ok = 0;
    if (fclose(file) != 0)
        ok = 0;
    if (ok && rename(temporary, path) != 0)
        ok = 0;
    if (!ok)
        unlink(temporary);
    return ok;
}

static void preferences_load_locked(void)
{
    if (preferences_loaded)
        return;
    preferences_loaded = 1;

    char path[1200];
    snprintf(path, sizeof path, "%s/shared-preferences.bin", gds_home);
    FILE *file = fopen(path, "rb");
    if (!file)
        return;

    unsigned char magic[8];
    static const unsigned char expected[8] =
        { 'H', 'G', 'O', 'P', 'R', 'E', 'F', '1' };
    uint32_t count;
    if (!pref_read(file, magic, sizeof magic) ||
        memcmp(magic, expected, sizeof magic) != 0 ||
        !pref_read(file, &count, sizeof count) ||
        count > MAX_PREFS) {
        fclose(file);
        nx_log("prefs: ignored invalid %s", path);
        return;
    }

    for (uint32_t i = 0; i < count; i++) {
        uint8_t type;
        uint32_t key_length;
        uint32_t value_length;
        if (!pref_read(file, &type, sizeof type) ||
            !pref_read(file, &key_length, sizeof key_length) ||
            !pref_read(file, &value_length, sizeof value_length) ||
            key_length == 0 || key_length > 4096 ||
            value_length > 1024 * 1024 ||
            type < PREF_INT || type > PREF_BYTES)
            break;
        char *key = calloc((size_t)key_length + 1, 1);
        char *value = calloc((size_t)value_length + 1, 1);
        if (!key || !value ||
            !pref_read(file, key, key_length) ||
            !pref_read(file, value, value_length)) {
            free(key);
            free(value);
            break;
        }
        pref_entry *entry = pref_set_locked(key, (enum pref_type)type);
        if (entry) {
            if (type == PREF_STRING) {
                entry->string = value;
                value = NULL;
            } else if (type == PREF_BYTES) {
                entry->blob = (unsigned char *)value;
                entry->blob_len = value_length;
                value = NULL;
            } else if (value_length == sizeof(uint32_t)) {
                if (type == PREF_FLOAT)
                    memcpy(&entry->real, value, sizeof entry->real);
                else
                    memcpy(&entry->integer, value, sizeof entry->integer);
            }
        }
        free(key);
        free(value);
    }
    fclose(file);
    nx_log("prefs: loaded %d persistent entries", preferences_count);
}

static int64_t j_getPreferences(jctx *c)
{
    (void)c;
    return (int64_t)(uintptr_t)preferences_object;
}

/* ---- kairo.android.plugin.Utility preference family ----------------------
 * kairo.unity.io.RecordStore keeps every save slot as a preference entry, so
 * these five calls are the game's whole persistence layer.  None of them was
 * bound: Utility.getPreferenceKeys() answered NULL through the generic path,
 * the managed reader maps empty/null to a null String[] and
 * KairoPlugin.GetPreferenceKeys(array) raises NRE at il2cpp+0x17fc300 ->
 * AppData.LoadSystem rethrows at il2cpp+0xe78210 -> Main catch ->
 * "An error has occurred." (trapD12).  Behaviours below are 1:1 with
 * classes.dex (via the kairovm reference model):
 *   putPreference(String,byte[])  store bytes under key
 *   getPreference(String)         -> byte[] / null when absent
 *   existPreference(String)       -> 1/0
 *   removePreference(String)
 *   getPreferenceKeys()           -> join(',', StringUtil.escape(key))
 *      escape = '"' + key with \ before , & @ \ + '"'   (never null)
 * Also real: setNotificationFilter/setNotificationBackground -- on a device
 * THESE are the writes that keep the store non-empty at boot, which the
 * shipped reader depends on (empty string -> null -> Enumerable NRE). */

static void pref_put_blob_locked(const char *key, const void *data,
                                 uint32_t len)
{
    pref_entry *entry = pref_set_locked(key, PREF_BYTES);
    if (!entry)
        return;
    entry->blob = malloc(len ? len : 1);
    entry->blob_len = len;
    if (len)
        memcpy(entry->blob, data, len);
    preferences_save_locked();
}

static const unsigned char *pref_find_blob_locked(const char *key,
                                                  uint32_t *len)
{
    pref_entry *entry = pref_find_locked(key);
    if (!entry || entry->type != PREF_BYTES)
        return NULL;
    *len = entry->blob_len;
    return entry->blob;
}

static int64_t j_Utility_putPreference(jctx *c)
{
    jobj *key = jarg_obj(c);
    jobj *bytes = jarg_obj(c);
    if (!key || !key->str)
        return 0;
    pthread_mutex_lock(&preferences_lock);
    preferences_load_locked();
    pref_put_blob_locked(key->str, bytes ? bytes->data : NULL,
                         bytes ? (uint32_t)bytes->len : 0);
    pthread_mutex_unlock(&preferences_lock);
    JT("Utility.putPreference(\"%s\", %d bytes)", key->str,
       bytes ? bytes->len : 0);
    return 0;
}

static int64_t j_Utility_getPreference(jctx *c)
{
    jobj *key = jarg_obj(c);
    const char *k = key && key->str ? key->str : "";
    pthread_mutex_lock(&preferences_lock);
    preferences_load_locked();
    uint32_t len = 0;
    const unsigned char *blob = pref_find_blob_locked(k, &len);
    jobj *out = NULL;
    if (blob) {
        out = j_NewByteArray(NULL, (int32_t)len);
        if (len)
            memcpy(out->data, blob, len);
    }
    pthread_mutex_unlock(&preferences_lock);
    JT("Utility.getPreference(\"%s\") -> %s", k,
       out ? "byte[]" : "null");
    return (int64_t)(uintptr_t)out;
}

static int64_t j_Utility_existPreference(jctx *c)
{
    jobj *key = jarg_obj(c);
    const char *k = key && key->str ? key->str : "";
    pthread_mutex_lock(&preferences_lock);
    preferences_load_locked();
    int found = pref_find_locked(k) != NULL;
    pthread_mutex_unlock(&preferences_lock);
    JT("Utility.existPreference(\"%s\") -> %d", k, found);
    return found;
}

static int64_t j_Utility_removePreference(jctx *c)
{
    jobj *key = jarg_obj(c);
    if (!key || !key->str)
        return 0;
    pthread_mutex_lock(&preferences_lock);
    preferences_load_locked();
    int removed = pref_remove_locked(key->str);
    if (removed)
        preferences_save_locked();
    pthread_mutex_unlock(&preferences_lock);
    JT("Utility.removePreference(\"%s\") -> %d", key->str, removed);
    return 0;
}

static int key_index_cmp(const void *a, const void *b)
{
    return strcmp(preferences[*(const int *)a].key,
                  preferences[*(const int *)b].key);
}

static int64_t j_Utility_getPreferenceKeys(jctx *c)
{
    (void)c;
    pthread_mutex_lock(&preferences_lock);
    preferences_load_locked();
    int idx[MAX_PREFS];
    for (int i = 0; i < preferences_count; i++)
        idx[i] = i;
    qsort(idx, (size_t)preferences_count, sizeof(int), key_index_cmp);
    size_t cap = 64;
    for (int i = 0; i < preferences_count; i++)
        cap += strlen(preferences[i].key) * 2 + 4;
    char *out = malloc(cap);
    size_t n = 0;
    for (int i = 0; i < preferences_count; i++) {
        const char *key = preferences[idx[i]].key;
        if (i)
            out[n++] = ',';
        out[n++] = '"';
        for (const char *p = key; *p; p++) {
            if (*p == ',' || *p == '&' || *p == '@' || *p == '\\')
                out[n++] = '\\';
            out[n++] = *p;
        }
        out[n++] = '"';
    }
    out[n] = 0;
    pthread_mutex_unlock(&preferences_lock);
    JT("Utility.getPreferenceKeys() -> \"%s\"", out);
    jobj *s = mk_string(out);
    free(out);
    return (int64_t)(uintptr_t)s;
}

/* the `_plugin_*` keys kairo.android.plugin.Utility maintains itself */
static void pref_put_text(const char *key, const char *text)
{
    pthread_mutex_lock(&preferences_lock);
    preferences_load_locked();
    pref_put_blob_locked(key, text, (uint32_t)strlen(text));
    pthread_mutex_unlock(&preferences_lock);
}

static int pref_read_text_int(const char *key, int dflt)
{
    char buf[32];
    uint32_t len = 0;
    pthread_mutex_lock(&preferences_lock);
    preferences_load_locked();
    const unsigned char *blob = pref_find_blob_locked(key, &len);
    if (blob && len < sizeof buf) {
        memcpy(buf, blob, len);
        buf[len] = 0;
        int v = atoi(buf);
        pthread_mutex_unlock(&preferences_lock);
        return v;
    }
    pthread_mutex_unlock(&preferences_lock);
    return dflt;
}

static int64_t j_Utility_setNotificationFilter(jctx *c)
{
    int32_t level = jarg_int(c);
    char text[16];
    snprintf(text, sizeof text, "%d", level);
    pref_put_text("_plugin_notification_level", text);
    JT("Utility.setNotificationFilter(%d)", level);
    return 0;
}

static int64_t j_Utility_getNotificationFilter_impl(jctx *c)
{
    (void)c;
    int v = pref_read_text_int("_plugin_notification_level", 3);
    JT("Utility.getNotificationFilter() -> %d", v);
    return v;
}

static int64_t j_Utility_setNotificationBackground(jctx *c)
{
    int32_t on = jarg_int(c) != 0;
    pref_put_text("_plugin_notification_background", on ? "1" : "0");
    JT("Utility.setNotificationBackground(%d)", on);
    return 0;
}

static int64_t j_Utility_getNotificationBackground(jctx *c)
{
    (void)c;
    int v = pref_read_text_int("_plugin_notification_background", 0) == 1;
    JT("Utility.getNotificationBackground() -> %d", v);
    return v;
}

static int64_t j_Prefs_getBoolean(jctx *c)
{
    jobj *key = jarg_obj(c);
    int32_t dflt = jarg_int(c);
    const char *k = key && key->str ? key->str : "";
    if (strcmp(k, "gles-api-check") == 0)
        return 1;
    pthread_mutex_lock(&preferences_lock);
    preferences_load_locked();
    pref_entry *entry = pref_find_locked(k);
    int value = entry && entry->type == PREF_BOOL
        ? entry->integer != 0 : dflt != 0;
    pthread_mutex_unlock(&preferences_lock);
    JT("SharedPreferences.getBoolean(\"%s\", %d) -> %d", k, dflt, value);
    return value;
}

static int64_t j_Prefs_getInt(jctx *c)
{
    jobj *key = jarg_obj(c);
    int32_t dflt = jarg_int(c);
    const char *k = key && key->str ? key->str : "";
    pthread_mutex_lock(&preferences_lock);
    preferences_load_locked();
    pref_entry *entry = pref_find_locked(k);
    int32_t value = entry && entry->type == PREF_INT
        ? entry->integer : dflt;
    pthread_mutex_unlock(&preferences_lock);
    JT("SharedPreferences.getInt(\"%s\", %d) -> %d", k, dflt, value);
    return value;
}

static int64_t j_Prefs_getFloat(jctx *c)
{
    jobj *key = jarg_obj(c);
    float dflt = jarg_float(c);
    const char *k = key && key->str ? key->str : "";
    pthread_mutex_lock(&preferences_lock);
    preferences_load_locked();
    pref_entry *entry = pref_find_locked(k);
    float value = entry && entry->type == PREF_FLOAT
        ? entry->real : dflt;
    pthread_mutex_unlock(&preferences_lock);
    JT("SharedPreferences.getFloat(\"%s\", %.3f) -> %.3f", k, dflt, value);
    return jfloat_result(value);
}

static int64_t j_Prefs_getString(jctx *c)
{
    jobj *key = jarg_obj(c);
    jobj *dflt = jarg_obj(c);
    const char *k = key && key->str ? key->str : "";
    pthread_mutex_lock(&preferences_lock);
    preferences_load_locked();
    pref_entry *entry = pref_find_locked(k);
    jobj *value = entry && entry->type == PREF_STRING
        ? mk_string(entry->string ? entry->string : "") : dflt;
    pthread_mutex_unlock(&preferences_lock);
    return (int64_t)(uintptr_t)value;
}

static int64_t j_Prefs_contains(jctx *c)
{
    jobj *key = jarg_obj(c);
    const char *k = key && key->str ? key->str : "";
    pthread_mutex_lock(&preferences_lock);
    preferences_load_locked();
    int value = pref_find_locked(k) != NULL;
    pthread_mutex_unlock(&preferences_lock);
    JT("SharedPreferences.contains(\"%s\") -> %d", k, value);
    return value;
}

static int64_t j_Prefs_edit(jctx *c)
{
    (void)c;
    return (int64_t)(uintptr_t)preferences_editor;
}

static int64_t j_Prefs_putInt(jctx *c)
{
    jobj *key = jarg_obj(c);
    int32_t value = jarg_int(c);
    const char *k = key && key->str ? key->str : "";
    pthread_mutex_lock(&preferences_lock);
    preferences_load_locked();
    pref_entry *entry = pref_set_locked(k, PREF_INT);
    if (entry)
        entry->integer = value;
    pthread_mutex_unlock(&preferences_lock);
    JT("SharedPreferences.putInt(\"%s\", %d)", k, value);
    return (int64_t)(uintptr_t)preferences_editor;
}

static int64_t j_Prefs_putFloat(jctx *c)
{
    jobj *key = jarg_obj(c);
    float value = jarg_float(c);
    const char *k = key && key->str ? key->str : "";
    pthread_mutex_lock(&preferences_lock);
    preferences_load_locked();
    pref_entry *entry = pref_set_locked(k, PREF_FLOAT);
    if (entry)
        entry->real = value;
    pthread_mutex_unlock(&preferences_lock);
    JT("SharedPreferences.putFloat(\"%s\", %.3f)", k, value);
    return (int64_t)(uintptr_t)preferences_editor;
}

static int64_t j_Prefs_putString(jctx *c)
{
    jobj *key = jarg_obj(c);
    jobj *value = jarg_obj(c);
    const char *k = key && key->str ? key->str : "";
    const char *v = value && value->str ? value->str : "";
    pthread_mutex_lock(&preferences_lock);
    preferences_load_locked();
    pref_entry *entry = pref_set_locked(k, PREF_STRING);
    if (entry)
        entry->string = strdup(v);
    pthread_mutex_unlock(&preferences_lock);
    JT("SharedPreferences.putString(\"%s\", %d bytes)", k, (int)strlen(v));
    return (int64_t)(uintptr_t)preferences_editor;
}

static int64_t j_Prefs_putBoolean(jctx *c)
{
    jobj *key = jarg_obj(c);
    int32_t value = jarg_int(c);
    const char *k = key && key->str ? key->str : "";
    pthread_mutex_lock(&preferences_lock);
    preferences_load_locked();
    pref_entry *entry = pref_set_locked(k, PREF_BOOL);
    if (entry)
        entry->integer = value != 0;
    pthread_mutex_unlock(&preferences_lock);
    return (int64_t)(uintptr_t)preferences_editor;
}

static int64_t j_Prefs_remove(jctx *c)
{
    jobj *key = jarg_obj(c);
    const char *k = key && key->str ? key->str : "";
    pthread_mutex_lock(&preferences_lock);
    preferences_load_locked();
    for (int i = 0; i < preferences_count; i++) {
        if (strcmp(preferences[i].key, k) != 0)
            continue;
        free(preferences[i].key);
        free(preferences[i].string);
        memmove(&preferences[i], &preferences[i + 1],
                (size_t)(preferences_count - i - 1) *
                    sizeof preferences[0]);
        preferences_count--;
        break;
    }
    pthread_mutex_unlock(&preferences_lock);
    return (int64_t)(uintptr_t)preferences_editor;
}

static int64_t j_Prefs_clear(jctx *c)
{
    (void)c;
    pthread_mutex_lock(&preferences_lock);
    preferences_load_locked();
    for (int i = 0; i < preferences_count; i++) {
        free(preferences[i].key);
        free(preferences[i].string);
    }
    preferences_count = 0;
    pthread_mutex_unlock(&preferences_lock);
    return (int64_t)(uintptr_t)preferences_editor;
}

static int64_t j_Prefs_apply(jctx *c)
{
    (void)c;
    pthread_mutex_lock(&preferences_lock);
    preferences_load_locked();
    int ok = preferences_save_locked();
    pthread_mutex_unlock(&preferences_lock);
    if (!ok)
        nx_log("prefs: cannot persist settings under %s", gds_home);
    return 0;
}

static int64_t j_Prefs_commit(jctx *c)
{
    j_Prefs_apply(c);
    return 1;
}

static int64_t j_Object_getClass(jctx *c)
{
    return (int64_t)(uintptr_t)mk_class(
        c->self && c->self->cls ? c->self->cls : "java/lang/Object");
}

static int64_t j_getClassLoader(jctx *c)
{
    (void)c;
    return (int64_t)(uintptr_t)mk_object("java/lang/ClassLoader");
}

static int64_t j_ClassLoader_findLibrary(jctx *c)
{
    jobj *name = jarg_obj(c);
    const char *lib = name && name->str ? name->str : "";
    const char *file = NULL;

    if (strcmp(lib, "il2cpp") == 0 || strcmp(lib, "libil2cpp.so") == 0)
        file = "libil2cpp.so";
    else if (strcmp(lib, "main") == 0 || strcmp(lib, "libmain.so") == 0)
        file = "libmain.so";
    else if (strcmp(lib, "unity") == 0 || strcmp(lib, "libunity.so") == 0)
        file = "libunity.so";

    if (!file) {
        JT("ClassLoader.findLibrary(\"%s\") -> empty", lib);
        return (int64_t)(uintptr_t)mk_string("");
    }

    char path[1280];
    snprintf(path, sizeof path, "%s/%s", gds_gamedir, file);
    JT("ClassLoader.findLibrary(\"%s\") -> %s", lib, path);
    return (int64_t)(uintptr_t)mk_string(path);
}

static int64_t j_System_loadLibrary(jctx *c)
{
    jobj *name = jarg_obj(c);
    const char *requested = name && name->str ? name->str : "";
    const char *base = strrchr(requested, '/');
    base = base ? base + 1 : requested;

    char soname[128];
    if (strstr(base, ".so"))
        snprintf(soname, sizeof soname, "%s", base);
    else if (strncmp(base, "lib", 3) == 0)
        snprintf(soname, sizeof soname, "%s.so", base);
    else
        snprintf(soname, sizeof soname, "lib%s.so", base);

    nx_mod *module = nx_find_mod(soname);
    if (!module) {
        JT("System.%s(\"%s\") -> module unavailable",
           c->m && c->m->name ? c->m->name : "loadLibrary", requested);
        return 0;
    }

    /* 0.60.0-ref: GDS's il2cpp metadata path template is
     * <data_dir>/Metadata/global-metadata.dat (no "Managed/"), so data_dir must
     * be "data/Managed" for it to find data/Managed/Metadata/global-metadata.dat
     * (bench-proven).  Set it before libil2cpp's init_array runs its il2cpp_init. */
    if (strcmp(soname, "libil2cpp.so") == 0) {
        void *(*set_data)(const char *) = nx_lookup_in(module, "il2cpp_set_data_dir");
        if (set_data) {
            extern char gds_datadir[];
            char md[1024];
            snprintf(md, sizeof md, "%s/Managed", gds_datadir);
            set_data(md);
            fprintf(stderr, "[gds] il2cpp_set_data_dir(\"%s\")\n", md);
        } else {
            fprintf(stderr, "[gds] WARNING: libil2cpp has no il2cpp_set_data_dir export\n");
        }
    }

    nx_run_init(module);
    if (strcmp(soname, "libFirebaseCppApp-12_10_1.so") == 0) {
        static int firebase_onloaded;
        if (!firebase_onloaded) {
            typedef int (*onload_fn)(void *, void *);
            onload_fn onload =
                (onload_fn)nx_lookup_in(module, "JNI_OnLoad");
            if (!onload)
                nx_die("%s has no JNI_OnLoad", soname);
            int version = onload(gds_jni_vm(), NULL);
            if (version < 0)
                nx_die("JNI_OnLoad(%s) failed: %#x", soname, version);
            firebase_onloaded = 1;
            nx_log("JNI_OnLoad(%s) -> %#x", soname, version);
        }
    }
    JT("System.%s(\"%s\") -> %s initialized",
       c->m && c->m->name ? c->m->name : "loadLibrary", requested,
       soname);
    return 0;
}

static int64_t j_forName(jctx *c)
{
    jobj *name = jarg_obj(c);
    (void)jarg_int(c);
    (void)jarg_obj(c);
    const char *n = name && name->str ? name->str : "java/lang/Object";
    char cls[256];
    snprintf(cls, sizeof cls, "%s", n);
    for (char *p = cls; *p; p++)
        if (*p == '.')
            *p = '/';
    return (int64_t)(uintptr_t)mk_class(cls);
}

static int64_t j_newInterfaceProxy(jctx *c)
{
    int64_t handle = jarg_long(c);
    jobj *interfaces = jarg_obj(c);
    jobj *proxy = mk_object("bitter/jnibridge/Proxy");
    proxy->prim = handle;
    proxy->len = (int)proxy_interfaces(interfaces);
    JT("newInterfaceProxy handle=%#llx flags=%#x -> %p",
       (unsigned long long)handle, proxy->len, (void *)proxy);
    if ((proxy->len & (PROXY_HANDLER_CALLBACK | PROXY_FRAME_CALLBACK)) ==
        (PROXY_HANDLER_CALLBACK | PROXY_FRAME_CALLBACK)) {
        __atomic_store_n(&choreo_proxy, proxy, __ATOMIC_RELEASE);
        nx_log("jni: UnityChoreographer proxy captured handle=%#llx",
               (unsigned long long)handle);
    }
    return (int64_t)(uintptr_t)proxy;
}

static int64_t j_Thread_start(jctx *c)
{
    if (!c->self || !c->self->cls ||
        strcmp(c->self->cls, "android/os/HandlerThread") != 0 ||
        !c->self->str || strcmp(c->self->str, "UnityChoreographer") != 0)
        return 0;

    int expected = 0;
    if (!__atomic_compare_exchange_n(&choreo_thread_started, &expected, 1, 0,
                                     __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        return 0;

    pthread_t thread;
    int rc = pthread_create(&thread, NULL, choreographer_driver, NULL);
    if (rc != 0) {
        __atomic_store_n(&choreo_thread_started, 0, __ATOMIC_RELEASE);
        nx_log("jni: cannot start UnityChoreographer HandlerThread: %s",
               strerror(rc));
        return 0;
    }
    pthread_detach(thread);
    nx_log("jni: HandlerThread.start(\"UnityChoreographer\")");
    return 0;
}

static int64_t j_FMOD_start(jctx *c)
{
    if (c->self)
        fmod_device_object = c->self;
    __atomic_store_n(&fmod_should_run, 1, __ATOMIC_RELEASE);
    static int seen;
    if (seen < 3) {
        seen++;
        fprintf(stderr, "[audio] FMODAudioDevice.start%s -> native pump ON\n",
                c->m && c->m->sig ? c->m->sig : "");
        fflush(stderr);
    }
    return 0;
}

static int64_t j_FMOD_stop(jctx *c)
{
    __atomic_store_n(&fmod_should_run, 0, __ATOMIC_RELEASE);
    static int seen;
    if (seen < 3) {
        seen++;
        fprintf(stderr, "[audio] FMODAudioDevice.%s -> native pump OFF\n",
                c->m && c->m->name ? c->m->name : "stop");
        fflush(stderr);
    }
    return 0;
}

static int64_t j_FMOD_isRunning(jctx *c)
{
    (void)c;
    /* poll-rate evidence: how hard does the C++ side watch the device? */
    static unsigned calls;
    calls++;
    if (calls == 1 || calls % 600 == 0) {
        fprintf(stderr, "[audio] FMODAudioDevice.isRunning() #%u -> %d\n",
                calls, gds_jni_fmod_should_run());
        fflush(stderr);
    }
    return gds_jni_fmod_should_run() ? 1 : 0;
}

static int64_t j_FMOD_startAudioDevice(jctx *c)
{
    int rate = jarg_int(c);
    int channels = jarg_int(c);
    int blockframes = jarg_int(c);
    fprintf(stderr, "[audio] FMODAudioDevice.startAudioDevice(%d, %d, %d)\n",
            rate, channels, blockframes);
    fflush(stderr);
    gds_audio_fmod_config(rate, channels, blockframes);
    gds_audio_fmod_thread_start();
    __atomic_store_n(&fmod_should_run, 1, __ATOMIC_RELEASE);
    return 1;
}

static int64_t j_FMOD_stopAudioDevice(jctx *c)
{
    (void)c;
    fprintf(stderr, "[audio] FMODAudioDevice.stopAudioDevice\n");
    fflush(stderr);
    __atomic_store_n(&fmod_should_run, 0, __ATOMIC_RELEASE);
    return 0;
}

static int64_t j_HandlerThread_getLooper(jctx *c)
{
    (void)c;
    return (int64_t)(uintptr_t)looper_object;
}

static int64_t j_Looper_get(jctx *c)
{
    (void)c;
    return (int64_t)(uintptr_t)looper_object;
}

static int64_t j_Handler_obtainMessage(jctx *c)
{
    (void)c;
    message_what = jarg_int(c);
    message_object->prim = message_what;
    JT("Handler.obtainMessage(%d) -> %p", message_what,
       (void *)message_object);
    return (int64_t)(uintptr_t)message_object;
}

static int64_t j_Message_sendToTarget(jctx *c)
{
    (void)c;
    __atomic_store_n(&message_pending, 1, __ATOMIC_RELEASE);
    nx_log("jni: Message.sendToTarget queued for UnityChoreographer");
    return 0;
}

static int64_t j_System_nanoTime(jctx *c)
{
    (void)c;
    return monotonic_nanos();
}

static int64_t j_Choreographer_getInstance(jctx *c)
{
    (void)c;
    return (int64_t)(uintptr_t)choreographer_object;
}

static int64_t j_Choreographer_postFrameCallback(jctx *c)
{
    /* doFrame is paced by choreographer_driver.  The callback remains the same
     * Java proxy; posting is one-shot on Android and the driver likewise calls
     * it once per tick. */
    jobj *callback = jarg_obj(c);
    if (callback && (callback->len & PROXY_FRAME_CALLBACK))
        __atomic_store_n(&choreo_proxy, callback, __ATOMIC_RELEASE);
    return 0;
}

static int64_t j_Method_getName(jctx *c)
{
    return (int64_t)(uintptr_t)mk_string(
        c->self && c->self->str ? c->self->str : "");
}

static int64_t j_runOnUiThread(jctx *c)
{
    jobj *runnable = jarg_obj(c);
    if (!runnable || !(runnable->len & PROXY_RUNNABLE))
        return 0;
    jobj *iface = mk_class("java/lang/Runnable");
    JT("runOnUiThread: invoking proxy=%p handle=%#llx", (void *)runnable,
       (unsigned long long)runnable->prim);
    (void)invoke_proxy(runnable, iface, run_method, empty_args);
    return 0;
}

static int64_t j_Unity_showSoftInput(jctx *c)
{
    if (c->self)
        unity_player_object = c->self;
    jobj *initial = jarg_obj(c);
    int keyboard_type = jarg_int(c);
    int autocorrection = jarg_int(c);
    int multiline = jarg_int(c);
    int secure = jarg_int(c);
    int alert = jarg_int(c);
    jobj *placeholder = jarg_obj(c);
    int character_limit = jarg_int(c);
    int hide_input = jarg_int(c);
    int close_on_outside_tap = jarg_int(c);
    (void)autocorrection;
    (void)multiline;
    (void)secure;
    (void)alert;
    (void)placeholder;
    (void)hide_input;
    (void)close_on_outside_tap;

    nx_log("jni: showSoftInput(type=%d limit=%d)", keyboard_type,
           character_limit);
    gds_input_keyboard_open(
        initial && initial->str ? initial->str : "", character_limit);
    return 0;
}

static int64_t j_Unity_setSoftInputStr(jctx *c)
{
    jobj *text = jarg_obj(c);
    gds_input_keyboard_set(text && text->str ? text->str : "");
    return 0;
}

static int64_t j_Unity_hideSoftInput(jctx *c)
{
    (void)c;
    gds_input_keyboard_hide();
    return 0;
}

/* shared with the Kairosoft showDialog handler for the error dump */
static char g_jni_ring[32][112];
static unsigned g_jni_ring_pos;

static void gds_jni_ring_dump(void)
{
    fprintf(stderr, "[jni] last dispatched JNI calls (oldest first):\n");
    for (int k = 0; k < 32; k++) {
        unsigned i = (g_jni_ring_pos + k) & 31;
        if (g_jni_ring[i][0])
            fprintf(stderr, "[jni]   %s\n", g_jni_ring[i]);
    }
    fflush(stderr);
}

static int64_t dispatch(void *e, jobj *self, void *mid, va_list *ap,
                        const uint64_t *args)
{
    const jmethod *m = by_id(mid);
    if (!m) {
        JT("call on unknown method id %p", mid);
        return 0;
    }
    /* 0.86 forensics: the kairo "An error has occurred." dialog fires with
     * no JNI evidence in a non-verbose log.  Keep the last 32 dispatched
     * methods in a tiny ring and dump it when that dialog opens. */
    {
        static char ring[32][112];
        static unsigned ring_pos;
        snprintf(ring[ring_pos], sizeof ring[0], "%s.%s%s",
                 m->cls ? m->cls : "?", m->name ? m->name : "?",
                 m->sig ? m->sig : "");
        ring_pos = (ring_pos + 1) & 31;
        memcpy(g_jni_ring, ring, sizeof ring);
        g_jni_ring_pos = ring_pos;
    }
    /* Reflected Field/Method handles are our numeric registry ids, not jobj
     * pointers: ReflectionHelper.getFieldID returns the id and Unity then
     * calls methods ON it (java/lang/reflect/Field.getDeclaringClass etc).
     * Treating the id as a jobj* dereferences garbage at self->boxed and
     * segfaults -- observed where DisplayMetrics.density (id 39) met
     * Field.getDeclaringClass (id 334). */
    if ((uintptr_t)self >= 1 && (uintptr_t)self <= 0x10000 && m->cls &&
        (strcmp(m->cls, "java/lang/reflect/Field") == 0 ||
         strcmp(m->cls, "java/lang/reflect/Method") == 0 ||
         strcmp(m->cls, "java/lang/reflect/AccessibleObject") == 0)) {
        const jmethod *fld = by_id(self);
        if (fld) {
            if (strcmp(m->name, "getDeclaringClass") == 0)
                return (int64_t)(uintptr_t)mk_class(
                    fld->cls ? fld->cls : "java/lang/Object");
            if (strcmp(m->name, "getName") == 0)
                return (int64_t)(uintptr_t)mk_string(
                    fld->name ? fld->name : "");
            JT("reflect %s.%s on field %s.%s -> 0", m->cls, m->name,
               fld->cls ? fld->cls : "?", fld->name ? fld->name : "?");
            return 0;
        }
    }
    if (self && self->boxed && is_unbox(m->name)) {
        JT("unboxed %s.%s -> %lld", self->cls, m->name, (long long)self->prim);
        return self->prim;
    }
    /* kairo FEP text-entry panel: routed by name regardless of the dex
     * signature (showInputPanel / startInputPanel / isEndInputPanel /
     * getResultInputPanel drive osk.c; see j_kairo_InputPanel). */
    if (m->cls && strcmp(m->cls, "kairo/android/plugin/Utility") == 0 &&
        strstr(m->name, "InputPanel")) {
        jctx c = {
            .env = e,
            .self = self,
            .ap = ap,
            .args = args,
            .arg_index = 0,
            .m = m,
        };
        return j_kairo_InputPanel(&c);
    }
    if (m->handler) {
        jctx c = {
            .env = e,
            .self = self,
            .ap = ap,
            .args = args,
            .arg_index = 0,
            .m = m,
        };
        return ((gds_jni_handler)m->handler)(&c);
    }
    /* Java inheritance is implicit on Android, while this deliberately small
     * registry keys reflected methods by the concrete runtime class.  Unity's
     * AndroidJavaObject signature builder calls Object.getClass() for every
     * reference argument and Class.getName() on the result.  Reproduce those
     * two inherited contracts generically so a ContentResolver (or any later
     * Android object) reports its real runtime type instead of becoming NULL. */
    if (self && self->cls && strcmp(m->name, "getClass") == 0)
        return (int64_t)(uintptr_t)mk_class(self->cls);
    if (self && self->type == O_CLASS &&
        (strcmp(m->name, "getName") == 0 ||
         strcmp(m->name, "getCanonicalName") == 0 ||
         strcmp(m->name, "getTypeName") == 0)) {
        char name[512];
        snprintf(name, sizeof name, "%s", self->cls ? self->cls
                                                       : "java/lang/Object");
        for (char *p = name; *p; p++)
            if (*p == '/')
                *p = '.';
        return (int64_t)(uintptr_t)mk_string(name);
    }
    /* A method we do not implement whose return type is the receiver's own
     * class is a builder -- Intent.putExtra, setPackage, setFlags, setData all
     * return the Intent so calls can be chained.  Returning NULL breaks the
     * chain mid-expression and every later link then operates on nothing;
     * returning the receiver is what the platform does. */
    if (self && self->cls) {
        const char *ret = strchr(m->sig, ')');
        size_t n = strlen(self->cls);
        if (ret && ret[1] == 'L' && strncmp(ret + 2, self->cls, n) == 0 &&
            ret[2 + n] == ';') {
            JT("builder %s.%s -> self", self->cls, m->name);
            return (int64_t)(uintptr_t)self;
        }
    }
    JT("unhandled %s.%s%s -> 0", m->cls ? m->cls : "?", m->name, m->sig);
    return 0;
}

/* Argument readers for handlers. */
jobj *gds_jarg_obj(jctx *c) { return jarg_obj(c); }
int32_t gds_jarg_int(jctx *c) { return jarg_int(c); }
int64_t gds_jarg_long(jctx *c) { return jarg_long(c); }
const char *gds_jarg_str(jctx *c)
{
    jobj *o = jarg_obj(c);
    return o && o->str ? o->str : NULL;
}
void *gds_jret_str(const char *s) { return mk_string(s); }
void *gds_jret_class(const char *s) { return mk_class(s); }
void *gds_jret_obj(const char *cls) { return new_obj(O_OBJECT, cls); }
void *gds_jni_activity(void) { return armory_activity_object; }
void *gds_jret_bytes(const void *p, int n)
{
    jobj *o = j_NewByteArray(NULL, n);
    if (p && n > 0)
        memcpy(o->data, p, (size_t)n);
    return o;
}

#define CALLV(nm, rt, cast) \
    static rt nm(void *e, void *o, void *mid, va_list ap) \
    { return (rt)(cast)dispatch(e, o, mid, &ap, NULL); }
#define CALL(nm, vnm, rt) \
    static rt nm(void *e, void *o, void *mid, ...) \
    { va_list ap; va_start(ap, mid); rt r = vnm(e, o, mid, ap); va_end(ap); return r; }
#define CALLA(nm, rt) \
    static rt nm(void *e, void *o, void *mid, void *args) \
    { return (rt)dispatch(e, o, mid, NULL, (const uint64_t *)args); }

CALLV(v_obj,  void *,   uintptr_t)
CALLV(v_bool, uint8_t,  uint8_t)
CALLV(v_byte, int8_t,   int8_t)
CALLV(v_char, uint16_t, uint16_t)
CALLV(v_short,int16_t,  int16_t)
CALLV(v_int,  int32_t,  int32_t)
CALLV(v_long, int64_t,  int64_t)

static float v_float(void *e, void *o, void *mid, va_list ap)
{
    int64_t r = dispatch(e, o, mid, &ap, NULL);
    float f;
    memcpy(&f, &r, sizeof f);
    return f;
}
static double v_double(void *e, void *o, void *mid, va_list ap)
{
    int64_t r = dispatch(e, o, mid, &ap, NULL);
    double d;
    memcpy(&d, &r, sizeof d);
    return d;
}
static void v_void(void *e, void *o, void *mid, va_list ap)
{
    dispatch(e, o, mid, &ap, NULL);
}

CALL(c_obj, v_obj, void *)
CALL(c_bool, v_bool, uint8_t)
CALL(c_byte, v_byte, int8_t)
CALL(c_char, v_char, uint16_t)
CALL(c_short, v_short, int16_t)
CALL(c_int, v_int, int32_t)
CALL(c_long, v_long, int64_t)
CALL(c_float, v_float, float)
CALL(c_double, v_double, double)
static void c_void(void *e, void *o, void *mid, ...)
{
    va_list ap;
    va_start(ap, mid);
    v_void(e, o, mid, ap);
    va_end(ap);
}

CALLA(a_obj, void *)
CALLA(a_bool, uint8_t)
CALLA(a_byte, int8_t)
CALLA(a_char, uint16_t)
CALLA(a_short, int16_t)
CALLA(a_int, int32_t)
CALLA(a_long, int64_t)
static float a_float(void *e, void *o, void *m, void *v)
{
    int64_t r = dispatch(e, o, m, NULL, (const uint64_t *)v);
    float f;
    memcpy(&f, &r, sizeof f);
    return f;
}
static double a_double(void *e, void *o, void *m, void *v)
{
    int64_t r = dispatch(e, o, m, NULL, (const uint64_t *)v);
    double d;
    memcpy(&d, &r, sizeof d);
    return d;
}
static void a_void(void *e, void *o, void *m, void *v)
{
    (void)dispatch(e, o, m, NULL, (const uint64_t *)v);
}

static void *j_GetMethodID(void *e, jobj *c, const char *n, const char *s)
{
    (void)e;
    return method_id(c ? c->cls : NULL, n, s);
}
static void *j_GetFieldID(void *e, jobj *c, const char *n, const char *s)
{
    (void)e;
    return method_id(c ? c->cls : NULL, n, s);
}

static jobj *j_AllocObject(void *e, jobj *c)
{
    (void)e;
    if (c && c->cls &&
        strcmp(c->cls, "org/fmod/FMODAudioDevice") == 0)
        return fmod_device_object;
    return new_obj(O_OBJECT, c ? c->cls : NULL);
}

/* Boxed JNI primitives have to retain their value for later longValue(),
 * intValue() and related calls. */
static int box_kind(const char *cls)
{
    if (!cls)
        return 0;
    if (strcmp(cls, "java/lang/Long") == 0)      return 'J';
    if (strcmp(cls, "java/lang/Integer") == 0)   return 'I';
    if (strcmp(cls, "java/lang/Short") == 0)     return 'S';
    if (strcmp(cls, "java/lang/Byte") == 0)      return 'B';
    if (strcmp(cls, "java/lang/Character") == 0) return 'C';
    if (strcmp(cls, "java/lang/Boolean") == 0)   return 'Z';
    if (strcmp(cls, "java/lang/Float") == 0)     return 'F';
    if (strcmp(cls, "java/lang/Double") == 0)    return 'D';
    return 0;
}

static jobj *box_from(jobj *c, void *mid, va_list *ap)
{
    if (c && c->cls &&
        strcmp(c->cls, "org/fmod/FMODAudioDevice") == 0)
        return fmod_device_object;
    jobj *o = new_obj(O_OBJECT, c ? c->cls : NULL);
    int k = box_kind(o->cls);
    if (!k) {
        /* Not a boxed primitive.  Keep a single String argument: java/io/File
         * and android/content/Intent are both built from one and then asked
         * about it, and an object that forgot its own path is indistinguishable
         * from a file that does not exist. */
        const jmethod *m = by_id(mid);
        if (m && strcmp(m->sig, "(Ljava/lang/String;)V") == 0) {
            jobj *s = va_arg(*ap, jobj *);
            if (s && s->str) {
                o->str = strdup(s->str);
                o->len = (int)strlen(o->str);
            }
        } else if (m && o->cls &&
                   strcmp(o->cls, "java/lang/String") == 0 &&
                   (strcmp(m->sig, "([BLjava/lang/String;)V") == 0 ||
                    strcmp(m->sig, "([B)V") == 0)) {
            /* Unity marshals managed UTF-8 strings by constructing a Java
             * String from byte[] plus the "UTF-8" charset name.  Dropping
             * that byte array turns every PlayerPrefs key into "", causing
             * all settings -- including the four audio volumes -- to alias. */
            jobj *bytes = va_arg(*ap, jobj *);
            if (strcmp(m->sig, "([BLjava/lang/String;)V") == 0)
                (void)va_arg(*ap, jobj *);
            if (bytes && bytes->data && bytes->len >= 0) {
                o->str = calloc((size_t)bytes->len + 1, 1);
                memcpy(o->str, bytes->data, (size_t)bytes->len);
                o->len = bytes->len;
            }
        } else if (m && o->cls &&
                   strcmp(o->cls, "java/util/Scanner") == 0 &&
                   strcmp(m->sig,
                          "(Ljava/io/InputStream;Ljava/lang/String;)V") == 0) {
            jobj *stream = va_arg(*ap, jobj *);
            (void)va_arg(*ap, jobj *);
            if (stream && stream->str) {
                o->str = strdup(stream->str);
                o->len = (int)strlen(o->str);
            }
        }
        JT("new %s(\"%s\")", o->cls ? o->cls : "?", o->str ? o->str : "");
        return o;
    }
    o->boxed = k;
    switch (k) {
    case 'J': o->prim = va_arg(*ap, int64_t); break;
    case 'I': o->prim = va_arg(*ap, int32_t); break;
    /* short, byte, char and boolean are promoted to int in a varargs call. */
    case 'S': o->prim = (int16_t)va_arg(*ap, int32_t); break;
    case 'B': o->prim = (int8_t)va_arg(*ap, int32_t); break;
    case 'C': o->prim = (uint16_t)va_arg(*ap, int32_t); break;
    case 'Z': o->prim = va_arg(*ap, int32_t) ? 1 : 0; break;
    case 'F': { double f = va_arg(*ap, double); float g = (float)f;
                memcpy(&o->prim, &g, sizeof g); break; }
    case 'D': { double f = va_arg(*ap, double);
                memcpy(&o->prim, &f, sizeof f); break; }
    default: break;
    }
    JT("boxed %s = %lld", o->cls, (long long)o->prim);
    return o;
}

static jobj *v_NewObject(void *e, jobj *c, void *mid, va_list ap)
{
    (void)e;
    return box_from(c, mid, &ap);
}
static jobj *j_NewObject(void *e, jobj *c, void *mid, ...)
{
    (void)e;
    va_list ap;
    va_start(ap, mid);
    jobj *o = box_from(c, mid, &ap);
    va_end(ap);
    return o;
}
static jobj *a_NewObject(void *e, jobj *c, void *mid, void *v)
{
    /* The A form passes a jvalue array, one slot per argument.  The VM builds
     * its java/io/File through this form and its Intent through the varargs
     * one, so the String argument has to be kept here too -- covering only the
     * varargs path leaves the File with no path at all. */
    (void)e;
    if (c && c->cls &&
        strcmp(c->cls, "org/fmod/FMODAudioDevice") == 0)
        return fmod_device_object;
    jobj *o = new_obj(O_OBJECT, c ? c->cls : NULL);
    int k = box_kind(o->cls);
    if (!k) {
        const jmethod *m = by_id(mid);
        if (m && v && strcmp(m->sig, "(Ljava/lang/String;)V") == 0) {
            jobj *s = *(jobj **)v;
            if (s && s->str) {
                o->str = strdup(s->str);
                o->len = (int)strlen(o->str);
            }
        } else if (m && v && o->cls &&
                   strcmp(o->cls, "java/lang/String") == 0 &&
                   (strcmp(m->sig, "([BLjava/lang/String;)V") == 0 ||
                    strcmp(m->sig, "([B)V") == 0)) {
            jobj *bytes = *(jobj **)v;
            if (bytes && bytes->data && bytes->len >= 0) {
                o->str = calloc((size_t)bytes->len + 1, 1);
                memcpy(o->str, bytes->data, (size_t)bytes->len);
                o->len = bytes->len;
            }
        }
        JT("new %s(\"%s\") [A]", o->cls ? o->cls : "?", o->str ? o->str : "");
        return o;
    }
    if (k && v) {
        o->boxed = k;
        o->prim = *(const int64_t *)v;
        if (k == 'I') o->prim = (int32_t)o->prim;
        if (k == 'S') o->prim = (int16_t)o->prim;
        if (k == 'B') o->prim = (int8_t)o->prim;
        if (k == 'C') o->prim = (uint16_t)o->prim;
        if (k == 'Z') o->prim = o->prim ? 1 : 0;
    }
    return o;
}

/* Fields all go through the same dispatch, so a getter can be bound the same
 * way a method is; anything unbound reads as zero/NULL. */
static void *f_obj(void *e, jobj *o, void *fid)
{
    return (void *)(uintptr_t)dispatch(e, o, fid, NULL, NULL);
}
static uint8_t f_bool(void *e, jobj *o, void *f) { return (uint8_t)(uintptr_t)f_obj(e, o, f); }
static int8_t f_byte(void *e, jobj *o, void *f) { return (int8_t)(uintptr_t)f_obj(e, o, f); }
static uint16_t f_char(void *e, jobj *o, void *f) { return (uint16_t)(uintptr_t)f_obj(e, o, f); }
static int16_t f_short(void *e, jobj *o, void *f) { return (int16_t)(uintptr_t)f_obj(e, o, f); }
static int32_t f_int(void *e, jobj *o, void *f) { return (int32_t)(uintptr_t)f_obj(e, o, f); }
static int64_t f_long(void *e, jobj *o, void *f) { return (int64_t)(uintptr_t)f_obj(e, o, f); }
static float f_float(void *e, jobj *o, void *f)
{
    /* float fields used to be unconditional 0.0f; DisplayMetrics wants real
     * values, so dispatch and reinterpret the raw bits like f_int does. */
    int64_t v = dispatch(e, o, f, NULL, NULL);
    int32_t bits = (int32_t)v;
    float out;
    memcpy(&out, &bits, sizeof out);
    return out;
}
static double f_double(void *e, jobj *o, void *f)
{
    int64_t v = dispatch(e, o, f, NULL, NULL);
    double out;
    memcpy(&out, &v, sizeof out);
    return out;
}
static void f_set(void *e, jobj *o, void *f, ...) { (void)e; (void)o; (void)f; }

static int32_t j_RegisterNatives(void *e, jobj *c, const void *m, int32_t n)
{
    (void)e;
    const struct { const char *name; const char *sig; void *fn; } *r = m;
    for (int32_t i = 0; i < n; i++) {
        if (natives_n == MAX_NATIVES)
            nx_die("native table full");
        jnative *k = &natives[natives_n++];
        snprintf(k->cls, sizeof k->cls, "%s", c && c->cls ? c->cls : "?");
        snprintf(k->name, sizeof k->name, "%s", r[i].name);
        snprintf(k->sig, sizeof k->sig, "%s", r[i].sig);
        k->fn = r[i].fn;
        nx_log("RegisterNatives %s.%s%s -> %p", k->cls, k->name, k->sig, k->fn);
        if (strstr(k->cls, "fmod") || strstr(k->cls, "FMOD")) {
            fprintf(stderr, "[audio] RegisterNatives %s.%s%s -> %p\n",
                    k->cls, k->name, k->sig, k->fn);
            fflush(stderr);
        }
    }
    return 0;
}
static int32_t j_UnregisterNatives(void *e, jobj *c) { (void)e; (void)c; return 0; }

static void *j_FromReflected(void *e, void *o)
{
    (void)e;
    if (o == doframe_method)
        return doframe_mid;
    if (o == handlemsg_method)
        return handlemsg_mid;
    if (o == run_method)
        return run_mid;
    return o;
}
static void *j_ToReflected(void *e, jobj *c, void *m, uint8_t st)
{
    (void)e; (void)c; (void)st;
    return m;
}
static jobj *j_DefineClass(void *e, const char *n, void *l, const void *b, int32_t sz)
{
    (void)e; (void)l; (void)b; (void)sz;
    return mk_class(n ? n : "?");
}

/* --- JavaVM ----------------------------------------------------------- */

static int32_t vm_GetEnv(void *vm, void **out, int32_t ver)
{
    (void)vm; (void)ver;
    *out = gds_jni_env();
    return 0;
}
static int32_t vm_AttachCurrentThread(void *vm, void **out, void *args)
{
    (void)vm; (void)args;
    *out = gds_jni_env();
    return 0;
}
static int32_t vm_DetachCurrentThread(void *vm) { (void)vm; return 0; }
static int32_t vm_DestroyJavaVM(void *vm) { (void)vm; return 0; }

/* ------------------------------------------------------------------- build */

void gds_jni_init(void)
{
    for (int i = 0; i < JNI_SLOT_COUNT; i++)
        vt[i] = NULL;

    vt[JNI_GetVersion] = j_GetVersion;
    vt[JNI_DefineClass] = j_DefineClass;
    vt[JNI_FindClass] = j_FindClass;
    vt[JNI_FromReflectedMethod] = j_FromReflected;
    vt[JNI_FromReflectedField] = j_FromReflected;
    vt[JNI_ToReflectedMethod] = j_ToReflected;
    vt[JNI_GetSuperclass] = j_GetSuperclass;
    vt[JNI_IsAssignableFrom] = j_IsAssignableFrom;
    vt[JNI_ToReflectedField] = j_ToReflected;
    vt[JNI_Throw] = j_Throw;
    vt[JNI_ThrowNew] = j_ThrowNew;
    vt[JNI_ExceptionOccurred] = j_ExceptionOccurred;
    vt[JNI_ExceptionDescribe] = j_ExceptionDescribe;
    vt[JNI_ExceptionClear] = j_ExceptionClear;
    vt[JNI_FatalError] = j_FatalError;
    vt[JNI_PushLocalFrame] = j_PushLocalFrame;
    vt[JNI_PopLocalFrame] = j_PopLocalFrame;
    vt[JNI_NewGlobalRef] = j_NewGlobalRef;
    vt[JNI_DeleteGlobalRef] = j_DeleteRef;
    vt[JNI_DeleteLocalRef] = j_DeleteRef;
    vt[JNI_IsSameObject] = j_IsSameObject;
    vt[JNI_NewLocalRef] = j_NewLocalRef;
    vt[JNI_EnsureLocalCapacity] = j_EnsureLocalCapacity;
    vt[JNI_AllocObject] = j_AllocObject;
    vt[JNI_NewObject] = j_NewObject;
    vt[JNI_NewObjectV] = v_NewObject;
    vt[JNI_NewObjectA] = a_NewObject;
    vt[JNI_GetObjectClass] = j_GetObjectClass;
    vt[JNI_IsInstanceOf] = j_IsInstanceOf;
    vt[JNI_GetMethodID] = j_GetMethodID;

    /* Call<Type>Method / ...V / ...A, then the Nonvirtual and Static blocks:
     * same handlers, the dispatcher does not care which flavour was used. */
    static void *const call3[10][3] = {
        { c_obj, v_obj, a_obj },       { c_bool, v_bool, a_bool },
        { c_byte, v_byte, a_byte },    { c_char, v_char, a_char },
        { c_short, v_short, a_short }, { c_int, v_int, a_int },
        { c_long, v_long, a_long },    { c_float, v_float, a_float },
        { c_double, v_double, a_double }, { c_void, v_void, a_void },
    };
    for (int t = 0; t < 10; t++)
        for (int k = 0; k < 3; k++) {
            vt[JNI_CallObjectMethod + t * 3 + k] = call3[t][k];
            vt[JNI_CallNonvirtualObjectMethod + t * 3 + k] = call3[t][k];
            vt[JNI_CallStaticObjectMethod + t * 3 + k] = call3[t][k];
        }

    vt[JNI_GetFieldID] = j_GetFieldID;
    void *const getf[9] = { f_obj, f_bool, f_byte, f_char, f_short, f_int,
                            f_long, f_float, f_double };
    for (int i = 0; i < 9; i++) {
        vt[JNI_GetObjectField + i] = getf[i];
        vt[JNI_GetStaticObjectField + i] = getf[i];
        vt[JNI_SetObjectField + i] = f_set;
        vt[JNI_SetStaticObjectField + i] = f_set;
    }
    vt[JNI_GetStaticMethodID] = j_GetMethodID;
    vt[JNI_GetStaticFieldID] = j_GetFieldID;

    vt[JNI_NewString] = j_NewString;
    vt[JNI_GetStringLength] = j_GetStringLength;
    vt[JNI_GetStringChars] = j_GetStringChars;
    vt[JNI_ReleaseStringChars] = j_ReleaseStringChars;
    vt[JNI_NewStringUTF] = j_NewStringUTF;
    vt[JNI_GetStringUTFLength] = j_GetStringUTFLength;
    vt[JNI_GetStringUTFChars] = j_GetStringUTFChars;
    vt[JNI_ReleaseStringUTFChars] = j_ReleaseStringUTFChars;
    vt[JNI_GetStringRegion] = j_GetStringUTFRegion;
    vt[JNI_GetStringUTFRegion] = j_GetStringUTFRegion;
    vt[JNI_GetStringCritical] = j_GetStringChars;
    vt[JNI_ReleaseStringCritical] = j_ReleaseStringChars;

    vt[JNI_GetArrayLength] = j_GetArrayLength;
    vt[JNI_NewObjectArray] = j_NewObjectArray;
    vt[JNI_GetObjectArrayElement] = j_GetObjectArrayElement;
    vt[JNI_SetObjectArrayElement] = j_SetObjectArrayElement;
    for (int i = 0; i < 8; i++) {
        vt[JNI_NewBooleanArray + i] = j_NewByteArray;
        vt[JNI_GetBooleanArrayElements + i] = j_GetByteArrayElements;
        vt[JNI_ReleaseBooleanArrayElements + i] = j_ReleaseArrayElements;
        vt[JNI_GetBooleanArrayRegion + i] = j_GetByteArrayRegion;
        vt[JNI_SetBooleanArrayRegion + i] = j_SetByteArrayRegion;
    }
    /* Primitive-array slots are adjacent, but element width is not.  The
     * gamepad enumerator returns int[], so give that type its real byte stride. */
    vt[JNI_NewBooleanArray + 4] = j_NewIntArray;
    vt[JNI_GetBooleanArrayRegion + 4] = j_GetIntArrayRegion;
    vt[JNI_SetBooleanArrayRegion + 4] = j_SetIntArrayRegion;
    vt[JNI_GetPrimitiveArrayCritical] = j_GetPrimitiveArrayCritical;
    vt[JNI_ReleasePrimitiveArrayCritical] = j_ReleasePrimitiveArrayCritical;

    vt[JNI_RegisterNatives] = j_RegisterNatives;
    vt[JNI_UnregisterNatives] = j_UnregisterNatives;
    vt[JNI_MonitorEnter] = j_MonitorEnter;
    vt[JNI_MonitorExit] = j_MonitorExit;
    vt[JNI_GetJavaVM] = j_GetJavaVM;
    vt[JNI_NewWeakGlobalRef] = j_NewWeakGlobalRef;
    vt[JNI_DeleteWeakGlobalRef] = j_DeleteRef;
    vt[JNI_ExceptionCheck] = j_ExceptionCheck;
    vt[JNI_NewDirectByteBuffer] = j_NewDirectByteBuffer;
    vt[JNI_GetDirectBufferAddress] = j_GetDirectBufferAddress;
    vt[JNI_GetDirectBufferCapacity] = j_GetDirectBufferCapacity;
    vt[JNI_GetObjectRefType] = j_GetObjectRefType;

    jvm_vt[3] = vm_DestroyJavaVM;
    jvm_vt[4] = vm_AttachCurrentThread;
    jvm_vt[5] = vm_DetachCurrentThread;
    jvm_vt[6] = vm_GetEnv;
    jvm_vt[7] = vm_AttachCurrentThread;   /* AttachCurrentThreadAsDaemon */

    looper_object = mk_object("android/os/Looper");
    choreographer_object = mk_object("android/view/Choreographer");
    message_object = mk_object("android/os/Message");

    doframe_method = mk_object("java/lang/reflect/Method");
    doframe_method->str = strdup("doFrame");
    handlemsg_method = mk_object("java/lang/reflect/Method");
    handlemsg_method->str = strdup("handleMessage");
    run_method = mk_object("java/lang/reflect/Method");
    run_method->str = strdup("run");

    frame_time_box = mk_object("java/lang/Long");
    frame_time_box->boxed = 'J';
    doframe_args = j_NewObjectArray(NULL, 1, mk_class("java/lang/Object"),
                                    frame_time_box);
    handlemsg_args = j_NewObjectArray(NULL, 1, mk_class("java/lang/Object"),
                                     message_object);
    empty_args = j_NewObjectArray(NULL, 0, mk_class("java/lang/Object"), NULL);

    /* The reflected methods handed to JNIBridge.invoke must map to the same
     * stable IDs that the native proxy obtained from these interfaces. */
    doframe_mid = method_id("android/view/Choreographer$FrameCallback",
                            "doFrame", "(J)V");
    handlemsg_mid = method_id("android/os/Handler$Callback", "handleMessage",
                              "(Landroid/os/Message;)Z");
    run_mid = method_id("java/lang/Runnable", "run", "()V");

    /* Bound up front rather than on demand so the VM's GetMethodID finds these
     * already carrying a handler; method_id matches on name and signature, so a
     * late bind would create a second, handler-less entry. */
    gds_jni_bind("bitter/jnibridge/JNIBridge", "newInterfaceProxy",
                 "(J[Ljava/lang/Class;)Ljava/lang/Object;",
                 (void *)j_newInterfaceProxy);
    gds_jni_bind("com/unity3d/player/ReflectionHelper", "getConstructorID",
                 "(Ljava/lang/Class;Ljava/lang/String;)"
                 "Ljava/lang/reflect/Constructor;",
                 (void *)j_Reflection_getConstructorID);
    gds_jni_bind("com/unity3d/player/ReflectionHelper", "getMethodID",
                 "(Ljava/lang/Class;Ljava/lang/String;Ljava/lang/String;Z)"
                 "Ljava/lang/reflect/Method;",
                 (void *)j_Reflection_getMethodID);
    gds_jni_bind("com/unity3d/player/ReflectionHelper", "getFieldID",
                 "(Ljava/lang/Class;Ljava/lang/String;Ljava/lang/String;Z)"
                 "Ljava/lang/reflect/Field;",
                 (void *)j_Reflection_getFieldID);
    gds_jni_bind("com/unity3d/player/ReflectionHelper", "getFieldSignature",
                 "(Ljava/lang/reflect/Field;)Ljava/lang/String;",
                 (void *)j_Reflection_getFieldSignature);
    gds_jni_bind("com/unity3d/player/UnityPlayer", "currentActivity",
                 "Landroid/app/Activity;", (void *)j_Unity_currentActivity);
    /* AndroidJavaObject erases reference return/field types to Object in the
     * ReflectionHelper signature.  Keep the typed aliases above for direct
     * JNI users and publish the erased forms used by Game Dev Story's managed
     * Armory layer. */
    gds_jni_bind("com/unity3d/player/UnityPlayer", "currentActivity",
                 "Ljava/lang/Object;", (void *)j_Unity_currentActivity);
    gds_jni_bind("java/lang/Thread", "start", "()V",
                 (void *)j_Thread_start);
    gds_jni_bind("org/fmod/FMODAudioDevice", "start", "()V",
                 (void *)j_FMOD_start);
    gds_jni_bind("org/fmod/FMODAudioDevice", "stop", "()V",
                 (void *)j_FMOD_stop);
    gds_jni_bind("org/fmod/FMODAudioDevice", "close", "()V",
                 (void *)j_FMOD_stop);
    gds_jni_bind("org/fmod/FMODAudioDevice", "isRunning", "()Z",
                 (void *)j_FMOD_isRunning);
    gds_jni_bind("org/fmod/FMODAudioDevice", "startAudioDevice", "(III)V",
                 (void *)j_FMOD_startAudioDevice);
    gds_jni_bind("org/fmod/FMODAudioDevice", "startAudioDevice", "(IIII)V",
                 (void *)j_FMOD_startAudioDevice);
    gds_jni_bind("org/fmod/FMODAudioDevice", "stopAudioDevice", "()V",
                 (void *)j_FMOD_stopAudioDevice);
    gds_jni_bind("org/fmod/FMODAudioDevice", "stopAudioDevice", "()Z",
                 (void *)j_FMOD_stopAudioDevice);
    gds_jni_bind("android/os/HandlerThread", "getLooper",
                 "()Landroid/os/Looper;", (void *)j_HandlerThread_getLooper);
    gds_jni_bind("android/os/Looper", "getMainLooper",
                 "()Landroid/os/Looper;", (void *)j_Looper_get);
    gds_jni_bind("android/os/Looper", "myLooper",
                 "()Landroid/os/Looper;", (void *)j_Looper_get);
    gds_jni_bind("android/os/Handler", "obtainMessage",
                 "(I)Landroid/os/Message;", (void *)j_Handler_obtainMessage);
    gds_jni_bind("android/os/Message", "sendToTarget", "()V",
                 (void *)j_Message_sendToTarget);
    gds_jni_bind("java/lang/System", "nanoTime", "()J",
                 (void *)j_System_nanoTime);
    gds_jni_bind("android/view/Choreographer", "getInstance",
                 "()Landroid/view/Choreographer;",
                 (void *)j_Choreographer_getInstance);
    gds_jni_bind("android/view/Choreographer", "postFrameCallback",
                 "(Landroid/view/Choreographer$FrameCallback;)V",
                 (void *)j_Choreographer_postFrameCallback);
    gds_jni_bind("java/lang/reflect/Method", "getName",
                 "()Ljava/lang/String;", (void *)j_Method_getName);
    gds_jni_bind("android/app/Activity", "runOnUiThread",
                 "(Ljava/lang/Runnable;)V", (void *)j_runOnUiThread);
    gds_jni_bind("com/unity3d/player/UnityPlayer", "showSoftInput",
                 "(Ljava/lang/String;IZZZZLjava/lang/String;IZZ)V",
                 (void *)j_Unity_showSoftInput);
    gds_jni_bind("com/unity3d/player/UnityPlayer", "setSoftInputStr",
                 "(Ljava/lang/String;)V",
                 (void *)j_Unity_setSoftInputStr);
    gds_jni_bind("com/unity3d/player/UnityPlayer", "hideSoftInput", "()V",
                 (void *)j_Unity_hideSoftInput);
    /* DisplayMetrics chain (see j_Context_getSystemService comment block) */
    gds_jni_bind("com/unity3d/player/UnityPlayer", "getWindowManager",
                 "()Ljava/lang/Object;", (void *)j_Unity_getWindowManager);
    gds_jni_bind("com/unity3d/player/UnityPlayer", "getWindowManager",
                 "()Landroid/view/WindowManager;", (void *)j_Unity_getWindowManager);
    gds_jni_bind("android/app/Activity", "getWindowManager",
                 "()Landroid/view/WindowManager;", (void *)j_Unity_getWindowManager);
    gds_jni_bind("android/view/WindowManager", "getDefaultDisplay",
                 "()Landroid/view/Display;", (void *)j_Wmgr_getDefaultDisplay);
    gds_jni_bind("android/view/WindowManager", "getDefaultDisplay",
                 "()Ljava/lang/Object;", (void *)j_Wmgr_getDefaultDisplay);
    gds_jni_bind("android/view/Display", "getMetrics",
                 "(Landroid/util/DisplayMetrics;)V", (void *)j_Display_getMetrics);
    gds_jni_bind("android/util/DisplayMetrics", "widthPixels", "I",
                 (void *)j_DM_width);
    gds_jni_bind("android/util/DisplayMetrics", "heightPixels", "I",
                 (void *)j_DM_height);
    gds_jni_bind("android/util/DisplayMetrics", "densityDpi", "I",
                 (void *)j_DM_dpi);
    gds_jni_bind("android/util/DisplayMetrics", "scaledDensity", "F",
                 (void *)j_DM_fscaled);
    gds_jni_bind("android/util/DisplayMetrics", "density", "F",
                 (void *)j_DM_fdensity);
    gds_jni_bind("android/util/DisplayMetrics", "xdpi", "F",
                 (void *)j_DM_fxdpi);
    gds_jni_bind("android/util/DisplayMetrics", "ydpi", "F",
                 (void *)j_DM_fydpi);
    gds_jni_bind("android/content/Context", "getSystemService",
                 "(Ljava/lang/String;)Ljava/lang/Object;",
                 (void *)j_Context_getSystemService);
    gds_jni_bind("android/app/Activity", "getSystemService",
                 "(Ljava/lang/String;)Ljava/lang/Object;",
                 (void *)j_Context_getSystemService);
    gds_jni_bind("com/unity3d/player/UnityPlayer", "getSystemService",
                 "(Ljava/lang/String;)Ljava/lang/Object;",
                 (void *)j_Context_getSystemService);
    /* FMOD OpenSL audio probe chain (see j_Context_getSystemService) */
    gds_jni_bind("android/content/Context", "AUDIO_SERVICE",
                 "Ljava/lang/String;", (void *)j_Context_AUDIO_SERVICE);
    gds_jni_bind("android/media/AudioManager", "PROPERTY_OUTPUT_SAMPLE_RATE",
                 "Ljava/lang/String;",
                 (void *)j_AudioManager_PROP_SAMPLE_RATE);
    gds_jni_bind("android/media/AudioManager",
                 "PROPERTY_OUTPUT_FRAMES_PER_BUFFER", "Ljava/lang/String;",
                 (void *)j_AudioManager_PROP_FRAMES_PER_BUFFER);
    gds_jni_bind("android/media/AudioManager", "getProperty",
                 "(Ljava/lang/String;)Ljava/lang/String;",
                 (void *)j_AudioManager_getProperty);
    gds_jni_bind("kairo/android/plugin/Utility", "showDialog",
                 "(ILjava/lang/String;Ljava/lang/String;Ljava/lang/Object;)I",
                 (void *)j_Kairo_showDialog);
    gds_jni_bind("kairo/android/plugin/Utility", "getScaleRatio",
                 "(IIII)F", (void *)j_Kairo_getScaleRatio);
    gds_jni_bind("kairo/android/plugin/Utility", "getNotificationData",
                 "()[Ljava/lang/String;",
                 (void *)j_Utility_getNotificationData);
    gds_jni_bind("kairo/android/plugin/Utility", "getNotificationData",
                 "()Ljava/lang/Object;",
                 (void *)j_Utility_getNotificationData);
    gds_jni_bind("kairo/android/plugin/Utility", "getNotificationFilter",
                 "(Ljava/lang/Object;)I",
                 (void *)j_Utility_getNotificationFilter_impl);
    gds_jni_bind("kairo/android/plugin/Utility", "getNotificationFilter",
                 "(Landroid/content/Context;)I",
                 (void *)j_Utility_getNotificationFilter_impl);
    gds_jni_bind("kairo/android/plugin/Utility", "getNotificationFilter",
                 "()I", (void *)j_Utility_getNotificationFilter_impl);
    gds_jni_bind("kairo/android/plugin/Utility", "setNotificationFilter",
                 "(I)V", (void *)j_Utility_setNotificationFilter);
    gds_jni_bind("kairo/android/plugin/Utility",
                 "getNotificationBackground", "()I",
                 (void *)j_Utility_getNotificationBackground);
    gds_jni_bind("kairo/android/plugin/Utility",
                 "getNotificationBackground", "()Z",
                 (void *)j_Utility_getNotificationBackground);
    gds_jni_bind("kairo/android/plugin/Utility",
                 "setNotificationBackground", "(I)V",
                 (void *)j_Utility_setNotificationBackground);
    gds_jni_bind("kairo/android/plugin/Utility",
                 "setNotificationBackground", "(Z)V",
                 (void *)j_Utility_setNotificationBackground);
    /* RecordStore persistence: the game's whole save system */
    gds_jni_bind("kairo/android/plugin/Utility", "putPreference",
                 "(Ljava/lang/String;[B)V", (void *)j_Utility_putPreference);
    gds_jni_bind("kairo/android/plugin/Utility", "setPreference",
                 "(Ljava/lang/String;[B)V", (void *)j_Utility_putPreference);
    gds_jni_bind("kairo/android/plugin/Utility", "getPreference",
                 "(Ljava/lang/String;)[B", (void *)j_Utility_getPreference);
    gds_jni_bind("kairo/android/plugin/Utility", "existPreference",
                 "(Ljava/lang/String;)Z", (void *)j_Utility_existPreference);
    gds_jni_bind("kairo/android/plugin/Utility", "existPreference",
                 "(Ljava/lang/String;)I", (void *)j_Utility_existPreference);
    gds_jni_bind("kairo/android/plugin/Utility", "removePreference",
                 "(Ljava/lang/String;)V", (void *)j_Utility_removePreference);
    gds_jni_bind("kairo/android/plugin/Utility", "getPreferenceKeys",
                 "()Ljava/lang/String;", (void *)j_Utility_getPreferenceKeys);
    /* PackageManager / PackageInfo / Signature (anti-tamper chain) */
    gds_jni_bind("android/content/Context", "getPackageManager",
                 "()Landroid/content/pm/PackageManager;",
                 (void *)j_Context_getPackageManager);
    gds_jni_bind("android/content/Context", "getPackageManager",
                 "()Ljava/lang/Object;",
                 (void *)j_Context_getPackageManager);
    gds_jni_bind("android/app/Activity", "getPackageManager",
                 "()Landroid/content/pm/PackageManager;",
                 (void *)j_Context_getPackageManager);
    gds_jni_bind("android/app/Activity", "getPackageManager",
                 "()Ljava/lang/Object;",
                 (void *)j_Context_getPackageManager);
    gds_jni_bind("com/unity3d/player/UnityPlayer", "getPackageManager",
                 "()Landroid/content/pm/PackageManager;",
                 (void *)j_Context_getPackageManager);
    gds_jni_bind("android/content/pm/PackageManager", "getPackageInfo",
                 "(Ljava/lang/String;I)Landroid/content/pm/PackageInfo;",
                 (void *)j_PM_getPackageInfo);
    gds_jni_bind("android/content/pm/PackageInfo", "versionCode", "I",
                 (void *)j_PackageInfo_getInt);
    gds_jni_bind("android/content/pm/PackageInfo", "longVersionCode", "J",
                 (void *)j_PackageInfo_getLong);
    gds_jni_bind("android/content/pm/PackageInfo", "versionName",
                 "Ljava/lang/String;", (void *)j_PackageInfo_versionName);
    gds_jni_bind("android/content/pm/PackageInfo", "packageName",
                 "Ljava/lang/String;", (void *)j_PackageInfo_packageName);
    gds_jni_bind("android/content/pm/PackageInfo", "signatures",
                 "[Landroid/content/pm/Signature;",
                 (void *)j_PackageInfo_signatures);
    gds_jni_bind("android/content/pm/PackageInfo", "signingInfo",
                 "Landroid/content/pm/SigningInfo;",
                 (void *)j_PackageInfo_signatures);
    gds_jni_bind("android/content/pm/Signature", "hashCode", "()I",
                 (void *)j_Signature_hashCode);
    gds_jni_bind("android/content/pm/Signature", "toByteArray", "()[B",
                 (void *)j_Signature_toByteArray);
    /* App-size / package identity (KairoPlugin.Init chain, run14/23) */
    gds_jni_bind("kairo/android/plugin/Utility", "getPackageName",
                 "()Ljava/lang/String;", (void *)j_Utility_getPackageName);
    gds_jni_bind("kairo/android/plugin/Utility", "isTablet",
                 "()Z", (void *)j_Utility_isTablet);
    gds_jni_bind("kairo/android/plugin/Utility", "isTablet",
                 "(Landroid/content/Context;)Z", (void *)j_Utility_isTablet);
    gds_jni_bind("kairo/android/plugin/Utility", "getSystemBarHeight",
                 "()I", (void *)j_Utility_getSystemBarHeight);
    gds_jni_bind("kairo/android/plugin/Utility", "getAppWidth",
                 "()I", (void *)j_Utility_getAppWidth);
    gds_jni_bind("kairo/android/plugin/Utility", "getAppHeight",
                 "()I", (void *)j_Utility_getAppHeight);
    gds_jni_bind("kairo/android/plugin/Utility", "getWidth",
                 "()I", (void *)j_Utility_getWidth);
    gds_jni_bind("kairo/android/plugin/Utility", "getHeight",
                 "()I", (void *)j_Utility_getHeight);
    /* GLES20 Java bridge (SupportsDepth24) */
    gds_jni_bind("android/opengl/GLES20", "GL_EXTENSIONS", "I",
                 (void *)j_GLES20_GL_EXTENSIONS);
    gds_jni_bind("android/opengl/GLES20", "glGetString",
                 "(I)Ljava/lang/String;", (void *)j_GLES20_glGetString);
    /* Display.getRealSize / Point / Message / Handler */
    gds_jni_bind("android/view/Display", "getRealSize",
                 "(Landroid/graphics/Point;)V", (void *)j_Display_getRealSize);
    gds_jni_bind("android/graphics/Point", "x", "I", (void *)j_Point_x);
    gds_jni_bind("android/graphics/Point", "y", "I", (void *)j_Point_y);
    gds_jni_bind("android/os/Message", "what", "I", (void *)j_Message_what);
    gds_jni_bind("android/os/Handler", "postDelayed",
                 "(Ljava/lang/Runnable;J)Z", (void *)j_Handler_postDelayed);
    gds_jni_bind("android/os/Handler", "post",
                 "(Ljava/lang/Runnable;)Z", (void *)j_Handler_postDelayed);
    gds_jni_bind("android/os/Handler", "sendEmptyMessage", "(I)Z",
                 (void *)j_Handler_postDelayed);

    input_device = mk_object("android/view/InputDevice");
    touch_device = mk_object("android/view/InputDevice");
    motion_range_list = mk_object("java/util/List");
    touch_motion_range_list = mk_object("java/util/List");
    motion_range_list->len = (int)(sizeof motion_ranges /
                                    sizeof *motion_ranges);
    motion_range_list->elems = calloc((size_t)motion_range_list->len,
                                      sizeof *motion_range_list->elems);
    for (int i = 0; i < motion_range_list->len; i++) {
        motion_ranges[i] = mk_object("android/view/InputDevice$MotionRange");
        motion_ranges[i]->prim = motion_axis_ids[i];
        motion_range_list->elems[i] = motion_ranges[i];
    }
    motion_range_iterator = mk_object("java/util/Iterator");
    key_event_object = mk_object("android/view/KeyEvent");
    motion_event_object = mk_object("android/view/MotionEvent");
    motion_event_object->data = &motion_event;
    fmod_device_object = mk_object("org/fmod/FMODAudioDevice");
    fmod_bytebuffer = j_NewDirectByteBuffer(NULL, fmod_pcm,
                                            fmod_buffer_size);
    preferences_object =
        mk_object("android/content/SharedPreferences");
    preferences_editor =
        mk_object("android/content/SharedPreferences$Editor");
    armory_activity_object =
        mk_object("com/unity3d/player/UnityPlayer");
    permission_plugin_object =
        mk_object("com/unity3d/player/UnityPlayer");
    gds_jni_bind("android/view/InputDevice", "getDeviceIds", "()[I",
                 (void *)j_InputDevice_getDeviceIds);
    gds_jni_bind("android/view/InputDevice", "getDevice",
                 "(I)Landroid/view/InputDevice;",
                 (void *)j_InputDevice_getDevice);
    gds_jni_bind("android/view/InputEvent", "getDevice",
                 "()Landroid/view/InputDevice;",
                 (void *)j_InputEvent_getDevice);
    gds_jni_bind("android/view/InputDevice", "getName",
                 "()Ljava/lang/String;", (void *)j_InputDevice_getString);
    gds_jni_bind("android/view/InputDevice", "getDescriptor",
                 "()Ljava/lang/String;", (void *)j_InputDevice_getString);
    gds_jni_bind("android/view/InputDevice", "getVendorId", "()I",
                 (void *)j_InputDevice_getInt);
    gds_jni_bind("android/view/InputDevice", "getProductId", "()I",
                 (void *)j_InputDevice_getInt);
    gds_jni_bind("android/view/InputDevice", "getSources", "()I",
                 (void *)j_InputDevice_getInt);
    gds_jni_bind("android/view/InputDevice", "getId", "()I",
                 (void *)j_InputDevice_getInt);
    gds_jni_bind("android/view/InputDevice", "getControllerNumber", "()I",
                 (void *)j_InputDevice_getInt);
    gds_jni_bind("android/view/InputDevice", "getKeyboardType", "()I",
                 (void *)j_InputDevice_getInt);
    gds_jni_bind("android/view/InputDevice", "supportsSource", "(I)Z",
                 (void *)j_InputDevice_supportsSource);
    gds_jni_bind("android/view/InputDevice", "getMotionRanges",
                 "()Ljava/util/List;", (void *)j_InputDevice_getMotionRanges);
    gds_jni_bind("android/view/InputDevice", "getMotionRange",
                 "(I)Landroid/view/InputDevice$MotionRange;",
                 (void *)j_InputDevice_getMotionRange);
    gds_jni_bind("java/util/List", "size", "()I", (void *)j_List_size);
    gds_jni_bind("java/util/List", "get", "(I)Ljava/lang/Object;",
                 (void *)j_List_get);
    gds_jni_bind("java/util/List", "iterator", "()Ljava/util/Iterator;",
                 (void *)j_List_iterator);
    gds_jni_bind("java/util/Iterator", "hasNext", "()Z",
                 (void *)j_Iterator_hasNext);
    gds_jni_bind("java/util/Iterator", "next", "()Ljava/lang/Object;",
                 (void *)j_Iterator_next);
    gds_jni_bind("android/view/InputDevice$MotionRange", "getAxis", "()I",
                 (void *)j_MotionRange_getInt);
    gds_jni_bind("android/view/InputDevice$MotionRange", "getSource", "()I",
                 (void *)j_MotionRange_getInt);
    static const char *const range_float_methods[] = {
        "getMin", "getMax", "getFlat", "getFuzz", "getResolution",
    };
    for (size_t i = 0; i < sizeof range_float_methods /
                            sizeof *range_float_methods; i++)
        gds_jni_bind("android/view/InputDevice$MotionRange",
                     range_float_methods[i], "()F",
                     (void *)j_MotionRange_getFloat);

    static const char *const key_int_methods[] = {
        "getAction", "getKeyCode", "getSource", "getDeviceId",
        "getMetaState", "getRepeatCount", "getScanCode", "getFlags",
        "getUnicodeChar",
    };
    for (size_t i = 0; i < sizeof key_int_methods / sizeof *key_int_methods; i++)
        gds_jni_bind("android/view/KeyEvent", key_int_methods[i], "()I",
                     (void *)j_KeyEvent_getInt);
    gds_jni_bind("android/view/KeyEvent", "getEventTime", "()J",
                 (void *)j_KeyEvent_getLong);
    gds_jni_bind("android/view/KeyEvent", "getDownTime", "()J",
                 (void *)j_KeyEvent_getLong);
    gds_jni_bind("android/view/KeyEvent", "isSystem", "()Z",
                 (void *)j_KeyEvent_isSystem);

    static const char *const motion_int_methods[] = {
        "getAction", "getActionMasked", "getActionIndex", "getSource",
        "getDeviceId", "getMetaState", "getButtonState", "getFlags",
        "getPointerCount", "getHistorySize", "getEdgeFlags",
        "getActionButton", "getDisplayId", "getClassification",
    };
    for (size_t i = 0; i < sizeof motion_int_methods /
                            sizeof *motion_int_methods; i++)
        gds_jni_bind("android/view/MotionEvent", motion_int_methods[i],
                     "()I", (void *)j_MotionEvent_getInt);
    static const char *const motion_int_arg_methods[] = {
        "getPointerId", "findPointerIndex", "getToolType",
    };
    for (size_t i = 0; i < sizeof motion_int_arg_methods /
                            sizeof *motion_int_arg_methods; i++)
        gds_jni_bind("android/view/MotionEvent", motion_int_arg_methods[i],
                     "(I)I", (void *)j_MotionEvent_getInt);
    gds_jni_bind("android/view/MotionEvent", "getEventTime", "()J",
                 (void *)j_MotionEvent_getLong);
    gds_jni_bind("android/view/MotionEvent", "getDownTime", "()J",
                 (void *)j_MotionEvent_getLong);
    gds_jni_bind("android/view/MotionEvent", "getHistoricalEventTime", "(I)J",
                 (void *)j_MotionEvent_getLong);
    gds_jni_bind("android/view/MotionEvent", "getAxisValue", "(I)F",
                 (void *)j_MotionEvent_getFloat);
    gds_jni_bind("android/view/MotionEvent", "getAxisValue", "(II)F",
                 (void *)j_MotionEvent_getFloat);
    gds_jni_bind("android/view/MotionEvent", "getHistoricalAxisValue",
                 "(II)F", (void *)j_MotionEvent_getFloat);
    gds_jni_bind("android/view/MotionEvent", "getHistoricalAxisValue",
                 "(III)F", (void *)j_MotionEvent_getFloat);
    static const char *const motion_xy_methods[] = {
        "getX", "getY", "getRawX", "getRawY", "getPressure", "getSize",
        "getTouchMajor", "getTouchMinor", "getToolMajor", "getToolMinor",
        "getOrientation",
    };
    for (size_t i = 0; i < sizeof motion_xy_methods /
                            sizeof *motion_xy_methods; i++) {
        gds_jni_bind("android/view/MotionEvent", motion_xy_methods[i], "()F",
                     (void *)j_MotionEvent_getFloat);
        gds_jni_bind("android/view/MotionEvent", motion_xy_methods[i], "(I)F",
                     (void *)j_MotionEvent_getFloat);
    }
    gds_jni_bind("android/view/MotionEvent", "getHistoricalX", "(I)F",
                 (void *)j_MotionEvent_getFloat);
    gds_jni_bind("android/view/MotionEvent", "getHistoricalX", "(II)F",
                 (void *)j_MotionEvent_getFloat);
    gds_jni_bind("android/view/MotionEvent", "getHistoricalY", "(I)F",
                 (void *)j_MotionEvent_getFloat);
    gds_jni_bind("android/view/MotionEvent", "getHistoricalY", "(II)F",
                 (void *)j_MotionEvent_getFloat);
    gds_jni_bind("android/view/MotionEvent", "isFromSource", "(I)Z",
                 (void *)j_MotionEvent_isFromSource);
    gds_jni_bind("android/view/MotionEvent", "obtain",
                 "(Landroid/view/MotionEvent;)Landroid/view/MotionEvent;",
                 (void *)j_MotionEvent_obtain);
    gds_jni_bind("android/view/MotionEvent", "recycle", "()V",
                 (void *)j_MotionEvent_recycle);
    gds_jni_bind("android/view/MotionEvent", "getDevice",
                 "()Landroid/view/InputDevice;",
                 (void *)j_InputEvent_getDevice);
    gds_jni_bind("android/view/InputEvent", "getSource", "()I",
                 (void *)j_MotionEvent_getInt);
    gds_jni_bind("android/view/InputEvent", "getDeviceId", "()I",
                 (void *)j_MotionEvent_getInt);
    gds_jni_bind("android/view/InputEvent", "getEventTime", "()J",
                 (void *)j_MotionEvent_getLong);

    gds_jni_bind("java/io/File", "length", "()J", (void *)j_File_length);
    gds_jni_bind("java/io/File", "exists", "()Z", (void *)j_File_exists);
    gds_jni_bind("java/io/File", "getPath", "()Ljava/lang/String;",
                 (void *)j_File_getPath);
    gds_jni_bind("java/io/File", "getAbsolutePath", "()Ljava/lang/String;",
                 (void *)j_File_getPath);
    gds_jni_bind("java/io/File", "getCanonicalPath", "()Ljava/lang/String;",
                 (void *)j_File_getPath);
    gds_jni_bind("java/io/File", "getParent", "()Ljava/lang/String;",
                 (void *)j_File_getParent);
    gds_jni_bind("java/io/File", "getParentFile", "()Ljava/io/File;",
                 (void *)j_File_getParentFile);
    gds_jni_bind("java/io/File", "isDirectory", "()Z",
                 (void *)j_File_isDirectory);
    gds_jni_bind("java/io/File", "mkdirs", "()Z", (void *)j_File_mkdirs);

    gds_jni_bind("android/content/Context", "getFilesDir",
                 "()Ljava/io/File;", (void *)j_Context_getFilesDir);
    gds_jni_bind("android/content/Context", "getExternalFilesDir",
                 "(Ljava/lang/String;)Ljava/io/File;",
                 (void *)j_Context_getFilesDir);
    gds_jni_bind("android/content/Context", "getCacheDir",
                 "()Ljava/io/File;", (void *)j_Context_getFilesDir);
    gds_jni_bind("android/content/Context", "getPackageCodePath",
                 "()Ljava/lang/String;", (void *)j_Context_getPackageCodePath);
    gds_jni_bind("android/content/Context", "getPackageName",
                 "()Ljava/lang/String;", (void *)j_Context_getPackageName);
    gds_jni_bind("android/content/Context", "getAssets",
                 "()Landroid/content/res/AssetManager;",
                 (void *)j_Context_getAssets);
    gds_jni_bind("android/content/Context", "getContentResolver",
                 "()Landroid/content/ContentResolver;",
                 (void *)j_Context_getContentResolver);
    gds_jni_bind("android/content/Context", "getContentResolver",
                 "()Ljava/lang/Object;",
                 (void *)j_Context_getContentResolver);
    /* initJni receives an Activity; Android inheritance makes these Context
     * methods, but the flat registry needs the concrete class aliases too. */
    gds_jni_bind("android/app/Activity", "getFilesDir",
                 "()Ljava/io/File;", (void *)j_Context_getFilesDir);
    gds_jni_bind("android/app/Activity", "getExternalFilesDir",
                 "(Ljava/lang/String;)Ljava/io/File;",
                 (void *)j_Context_getFilesDir);
    gds_jni_bind("android/app/Activity", "getPackageCodePath",
                 "()Ljava/lang/String;", (void *)j_Context_getPackageCodePath);
    gds_jni_bind("android/app/Activity", "getPackageName",
                 "()Ljava/lang/String;", (void *)j_Context_getPackageName);
    gds_jni_bind("android/app/Activity", "getAssets",
                 "()Landroid/content/res/AssetManager;",
                 (void *)j_Context_getAssets);
    gds_jni_bind("android/app/Activity", "getContentResolver",
                 "()Landroid/content/ContentResolver;",
                 (void *)j_Context_getContentResolver);
    gds_jni_bind("android/app/Activity", "getContentResolver",
                 "()Ljava/lang/Object;",
                 (void *)j_Context_getContentResolver);
    /* UnityPlayer extends UnityPlayerActivity/Activity.  The tiny object
     * model has no inheritance lookup, so publish the inherited Context calls
     * under the concrete launcher class as Android's GetMethodID would. */
    static const char armory_activity[] =
        "com/unity3d/player/UnityPlayer";
    gds_jni_bind(armory_activity, "getFilesDir", "()Ljava/io/File;",
                 (void *)j_Context_getFilesDir);
    gds_jni_bind(armory_activity, "getExternalFilesDir",
                 "(Ljava/lang/String;)Ljava/io/File;",
                 (void *)j_Context_getFilesDir);
    gds_jni_bind(armory_activity, "getCacheDir", "()Ljava/io/File;",
                 (void *)j_Context_getFilesDir);
    gds_jni_bind(armory_activity, "getPackageCodePath",
                 "()Ljava/lang/String;", (void *)j_Context_getPackageCodePath);
    gds_jni_bind(armory_activity, "getPackageName", "()Ljava/lang/String;",
                 (void *)j_Context_getPackageName);
    gds_jni_bind(armory_activity, "getAssets",
                 "()Landroid/content/res/AssetManager;",
                 (void *)j_Context_getAssets);
    gds_jni_bind(armory_activity, "getContentResolver",
                 "()Landroid/content/ContentResolver;",
                 (void *)j_Context_getContentResolver);
    gds_jni_bind(armory_activity, "getContentResolver",
                 "()Ljava/lang/Object;",
                 (void *)j_Context_getContentResolver);
    gds_jni_bind(armory_activity, "getClass", "()Ljava/lang/Class;",
                 (void *)j_Object_getClass);
    gds_jni_bind(armory_activity, "getClass", "()Ljava/lang/Object;",
                 (void *)j_Object_getClass);
    gds_jni_bind(armory_activity, "runOnUiThread",
                 "(Ljava/lang/Runnable;)V", (void *)j_runOnUiThread);
    gds_jni_bind(armory_activity, "getActivity",
                 "()Lcom/unity3d/player/UnityPlayer;",
                 (void *)j_Armory_getActivity);
    gds_jni_bind(armory_activity, "getActivity", "()Ljava/lang/Object;",
                 (void *)j_Armory_getActivity);
    gds_jni_bind(armory_activity, "getUnityPlayer",
                 "()Lcom/unity3d/player/UnityPlayer;",
                 (void *)j_Armory_getUnityPlayer);
    gds_jni_bind(armory_activity, "getUnityPlayer",
                 "()Ljava/lang/Object;",
                 (void *)j_Armory_getUnityPlayer);
    gds_jni_bind(armory_activity, "getOsVersion", "()I",
                 (void *)j_Armory_getOsVersion);
    gds_jni_bind(armory_activity, "getTargetSdkVersion", "()I",
                 (void *)j_Armory_getTargetSdkVersion);
    gds_jni_bind(armory_activity, "hasPermission",
                 "(Ljava/lang/String;)Z", (void *)j_Armory_hasPermission);

    static const char permission_plugin[] =
        "com/unity3d/player/UnityPlayer";
    gds_jni_bind(permission_plugin, "canRequestPermissions", "()Z",
                 (void *)j_Permission_canRequest);
    gds_jni_bind(permission_plugin, "hasPermission",
                 "(Ljava/lang/String;)Z", (void *)j_Armory_hasPermission);
    gds_jni_bind(permission_plugin, "shouldShowRequestPermissionRationale",
                 "(Ljava/lang/String;)Z", (void *)j_Permission_noRationale);
    gds_jni_bind(permission_plugin, "openApplicationSettings", "()V",
                 (void *)j_Permission_noop);
    gds_jni_bind(permission_plugin, "requestAccountPermission", "()V",
                 (void *)j_Permission_noop);
    gds_jni_bind(permission_plugin, "requestLocationPermission", "()V",
                 (void *)j_Permission_noop);
    gds_jni_bind(permission_plugin, "requestPermission",
                 "(Ljava/lang/String;)V", (void *)j_Permission_noop);
    gds_jni_bind(permission_plugin, "requestPermissions",
                 "(Ljava/lang/String;)V", (void *)j_Permission_noop);
    gds_jni_bind("android/provider/Settings$Secure", "getString",
                 "(Landroid/content/ContentResolver;Ljava/lang/String;)"
                 "Ljava/lang/String;", (void *)j_SettingsSecure_getString);
    gds_jni_bind("android/provider/Settings$Secure", "getString",
                 "(Ljava/lang/Object;Ljava/lang/String;)Ljava/lang/String;",
                 (void *)j_SettingsSecure_getString);
    gds_jni_bind("java/util/Locale", "getDefault",
                 "()Ljava/util/Locale;", (void *)j_Locale_getDefault);
    gds_jni_bind("java/util/Locale", "getDefault", "()Ljava/lang/Object;",
                 (void *)j_Locale_getDefault);
    gds_jni_bind("java/util/Locale", "getLanguage",
                 "()Ljava/lang/String;", (void *)j_Locale_getString);
    gds_jni_bind("java/util/Locale", "getCountry",
                 "()Ljava/lang/String;", (void *)j_Locale_getString);
    gds_jni_bind("java/util/Locale", "toLanguageTag",
                 "()Ljava/lang/String;", (void *)j_Locale_getString);
    gds_jni_bind("java/util/Locale", "toString", "()Ljava/lang/String;",
                 (void *)j_Locale_getString);
    gds_jni_bind("com/prime31/GoogleIABPlugin", "instance",
                 "()Ljava/lang/Object;", (void *)j_GoogleIAB_instance);
    gds_jni_bind("com/prime31/GoogleIABPlugin", "init",
                 "(Ljava/lang/String;)V", (void *)j_Permission_noop);
    gds_jni_bind("com/google/android/gms/games/PlayGamesSdk", "initialize",
                 "(Lcom/unity3d/player/UnityPlayer;)V",
                 (void *)j_PlayGames_initialize);
    gds_jni_bind("com/google/android/gms/games/PlayGames",
                 "getGamesSignInClient",
                 "(Lcom/unity3d/player/UnityPlayer;)"
                 "Ljava/lang/Object;", (void *)j_PlayGames_getSignInClient);
    gds_jni_bind("com/unity3d/player/UnityPlayer",
                 "ShowSignature", "()V", (void *)j_Permission_noop);
    gds_jni_bind("android/content/res/AssetManager", "open",
                 "(Ljava/lang/String;)Ljava/io/InputStream;",
                 (void *)j_AssetManager_open);
    gds_jni_bind("java/util/Scanner", "next", "()Ljava/lang/String;",
                 (void *)j_Scanner_next);
    gds_jni_bind("android/os/Environment", "getExternalStorageState",
                 "()Ljava/lang/String;", (void *)j_Environment_mounted);
    gds_jni_bind("android/os/Environment", "MEDIA_MOUNTED",
                 "Ljava/lang/String;", (void *)j_Environment_mounted);
    gds_jni_bind("java/lang/String", "getBytes", "(Ljava/lang/String;)[B",
                 (void *)j_String_getBytes);
    gds_jni_bind("java/lang/String", "getBytes", "()[B",
                 (void *)j_String_getBytes);
    gds_jni_bind("java/lang/String", "equals", "(Ljava/lang/Object;)Z",
                 (void *)j_String_equals);
    gds_jni_bind("java/lang/StringBuilder", "toString",
                 "()Ljava/lang/String;", (void *)j_StringBuilder_toString);
    gds_jni_bind("android/net/Uri", "encode",
                 "(Ljava/lang/String;)Ljava/lang/String;",
                 (void *)j_Uri_encode);
    gds_jni_bind("android/net/Uri", "decode",
                 "(Ljava/lang/String;)Ljava/lang/String;",
                 (void *)j_Uri_decode);

    for (size_t i = 0; i < sizeof build_fields / sizeof *build_fields; i++) {
        gds_jni_bind("android/os/Build", build_fields[i].name,
                     "Ljava/lang/String;", (void *)j_Build_string);
        gds_jni_bind("android/os/Build$VERSION", build_fields[i].name,
                     "Ljava/lang/String;", (void *)j_Build_string);
    }
    gds_jni_bind("android/os/Build$VERSION", "SDK_INT", "I",
                 (void *)j_Build_SDK_INT);

    gds_jni_bind("android/app/Activity", "getPreferences",
                 "(I)Landroid/content/SharedPreferences;",
                 (void *)j_getPreferences);
    gds_jni_bind("android/content/Context", "getSharedPreferences",
                 "(Ljava/lang/String;I)Landroid/content/SharedPreferences;",
                 (void *)j_getPreferences);
    gds_jni_bind("android/content/SharedPreferences", "getBoolean",
                 "(Ljava/lang/String;Z)Z", (void *)j_Prefs_getBoolean);
    gds_jni_bind("android/content/SharedPreferences", "getInt",
                 "(Ljava/lang/String;I)I", (void *)j_Prefs_getInt);
    gds_jni_bind("android/content/SharedPreferences", "getFloat",
                 "(Ljava/lang/String;F)F", (void *)j_Prefs_getFloat);
    gds_jni_bind("android/content/SharedPreferences", "getString",
                 "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;",
                 (void *)j_Prefs_getString);
    gds_jni_bind("android/content/SharedPreferences", "contains",
                 "(Ljava/lang/String;)Z", (void *)j_Prefs_contains);
    gds_jni_bind("android/content/SharedPreferences", "edit",
                 "()Landroid/content/SharedPreferences$Editor;",
                 (void *)j_Prefs_edit);
    gds_jni_bind("android/content/SharedPreferences$Editor", "putInt",
                 "(Ljava/lang/String;I)"
                 "Landroid/content/SharedPreferences$Editor;",
                 (void *)j_Prefs_putInt);
    gds_jni_bind("android/content/SharedPreferences$Editor", "putFloat",
                 "(Ljava/lang/String;F)"
                 "Landroid/content/SharedPreferences$Editor;",
                 (void *)j_Prefs_putFloat);
    gds_jni_bind("android/content/SharedPreferences$Editor", "putString",
                 "(Ljava/lang/String;Ljava/lang/String;)"
                 "Landroid/content/SharedPreferences$Editor;",
                 (void *)j_Prefs_putString);
    gds_jni_bind("android/content/SharedPreferences$Editor", "putBoolean",
                 "(Ljava/lang/String;Z)"
                 "Landroid/content/SharedPreferences$Editor;",
                 (void *)j_Prefs_putBoolean);
    gds_jni_bind("android/content/SharedPreferences$Editor", "remove",
                 "(Ljava/lang/String;)"
                 "Landroid/content/SharedPreferences$Editor;",
                 (void *)j_Prefs_remove);
    gds_jni_bind("android/content/SharedPreferences$Editor", "clear",
                 "()Landroid/content/SharedPreferences$Editor;",
                 (void *)j_Prefs_clear);
    gds_jni_bind("android/content/SharedPreferences$Editor", "apply",
                 "()V", (void *)j_Prefs_apply);
    gds_jni_bind("android/content/SharedPreferences$Editor", "commit",
                 "()Z", (void *)j_Prefs_commit);
    gds_jni_bind("java/lang/Object", "getClass", "()Ljava/lang/Class;",
                 (void *)j_Object_getClass);
    gds_jni_bind("java/lang/Class", "getClassLoader",
                 "()Ljava/lang/ClassLoader;", (void *)j_getClassLoader);
    gds_jni_bind("java/lang/ClassLoader", "findLibrary",
                 "(Ljava/lang/String;)Ljava/lang/String;",
                 (void *)j_ClassLoader_findLibrary);
    gds_jni_bind("java/lang/System", "loadLibrary",
                 "(Ljava/lang/String;)V", (void *)j_System_loadLibrary);
    gds_jni_bind("java/lang/System", "load",
                 "(Ljava/lang/String;)V", (void *)j_System_loadLibrary);
    gds_jni_bind("com/unity3d/player/UnityPlayer", "getClassLoader",
                 "()Ljava/lang/ClassLoader;", (void *)j_getClassLoader);
    gds_jni_bind("com/unity3d/player/UnityPlayer", "forName",
                 "(Ljava/lang/String;ZLjava/lang/ClassLoader;)Ljava/lang/Class;",
                 (void *)j_forName);

    /* Sanity: native Android objects call JNIEnv slots by their ABI offset. */
    if (JNI_NewStringUTF * 8 != 1336)
        nx_die("JNIEnv layout drift: NewStringUTF is at %d, not 1336",
               JNI_NewStringUTF * 8);
    nx_log("jni: env=%p vtable slots=%d", gds_jni_env(), JNI_SLOT_COUNT);
}

void *gds_jni_sym(const char *name)
{
    (void)name;
    return NULL;
}
