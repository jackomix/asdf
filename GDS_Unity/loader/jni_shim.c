/* jni_shim.c - minimal JNIEnv / JavaVM implementation for the native loader.
 *
 * libunity.so's only entry is JNI_OnLoad(JavaVM*, void*), which expects a real
 * Android runtime underneath.  We don't have one, so this file stands in for it
 * the way kairovm/androidjni.py did for the emulator: a JNINativeInterface
 * function table plus a JavaVMFunctionTable, a small class/method registry for
 * the classes the engine touches, and logging stubs for everything else.
 *
 * This is freestanding (no host libc) and uses the loader's own printf.
 *
 * The JNIEnv C-binding ABI: callers do (*env)->FindClass(env, name), so `env`
 * points at a JNINativeInterface (function-table) struct and the first argument
 * of every JNI function is that same `env`.
 */
#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include "kv_elf.h"
#include "kv_libc.h"

typedef void      *jobject;
typedef jobject    jclass;
typedef jobject    jstring;
typedef jobject    jthrowable;
typedef jobject    jweak;
typedef void      *jmethodID;
typedef void      *jfieldID;
typedef int        jint;
typedef long       jlong;
typedef int        jboolean;
typedef unsigned char jbyte;
typedef unsigned short jchar;
typedef short      jshort;
typedef float      jfloat;
typedef double     jdouble;
typedef struct _jvalue { union { jboolean z; jbyte b; jchar c; jshort s;
    jint i; jlong j; jfloat f; jdouble d; jobject l; } l; } jvalue;
typedef struct _jarray { void *pad; } *jarray;
typedef jarray jobjectArray;
typedef jarray jbooleanArray, jbyteArray, jcharArray, jshortArray, jintArray,
               jlongArray, jfloatArray, jdoubleArray;
typedef void *jobjectRefType;
typedef void *JavaVM;   /* opaque; callers treat it as a pointer to the VM */

/* ---- handle registry ---- */
#define KV_CLASS_MAX 128
struct kv_class { const char *name; jclass handle; };
static struct kv_class kv_classes[KV_CLASS_MAX];
static int kv_nclasses;
static jclass kv_next_class = (jclass)0x1000;

static jclass kv_find_or_add(const char *name) {
    for (int i = 0; i < kv_nclasses; i++)
        if (strcmp(kv_classes[i].name, name) == 0) return kv_classes[i].handle;
    if (kv_nclasses < KV_CLASS_MAX) {
        jclass h = kv_next_class; kv_next_class = (jclass)((uintptr_t)kv_next_class + 0x20);
        kv_classes[kv_nclasses].name = name; kv_classes[kv_nclasses].handle = h;
        kv_nclasses++;
        return h;
    }
    return (jclass)kv_next_class;
}

/* ---- forward decls of JNINativeInterface struct ---- */
struct JNINativeInterface_;
typedef const struct JNINativeInterface_ *JNIEnv;
typedef struct JNIEnv_ { const struct JNINativeInterface_ *functions; } JNIEnvS;

/* logging stub for unimplemented JNI slots. slot is 4..232 (function name). */
static void *kv_jni_stub(const char *what, JNIEnv env) {
    (void)env;
    return 0;
}

/* defined at the bottom; forward-declared for GetJavaVM */
void *kv_jni_java_vm(void);
void *kv_jni_env(void);

/* ================= implemented core JNI functions ================= */

static jint kv_GetVersion(JNIEnv env) { (void)env; return 0x00010006; } /* 1.6 */

static jclass kv_FindClass(JNIEnv env, const char *name) {
    (void)env;
    printf("[jni] FindClass(%s)\n", name ? name : "(null)");
    if (!name) return 0;
    return kv_find_or_add(name);
}

/* ---- jstring: store the real string so GetStringUTFChars returns it ---- */
#define KV_MAX_JSTR 256
static char kv_jstr_buf[KV_MAX_JSTR][256];
static int kv_jstr_n = 0;
static jstring kv_make_jstring(const char *s) {
    int i = kv_jstr_n++ % KV_MAX_JSTR;
    if (s) { unsigned n = strlen(s); if (n > 255) n = 255; memcpy(kv_jstr_buf[i], s, n); kv_jstr_buf[i][n] = 0; }
    else kv_jstr_buf[i][0] = 0;
    return (jstring)(uintptr_t)(0x4000 + (uintptr_t)i);
}
static const char *kv_resolve_jstring(jstring s) {
    unsigned long v = (unsigned long)s;
    if (v >= 0x4000 && v < 0x4000 + KV_MAX_JSTR) return kv_jstr_buf[v - 0x4000];
    return "";
}

/* ---- method registry: store id -> name/sig so Call*Method can dispatch ---- */
struct kv_method { jmethodID id; const char *name; const char *sig; };
static struct kv_method kv_methods[1024];
static int kv_nmethods = 0;
static jmethodID kv_next_mid = (jmethodID)0x2000;
static const char *kv_method_name(jmethodID m) {
    for (int i = 0; i < kv_nmethods; i++)
        if (kv_methods[i].id == m) return kv_methods[i].name;
    return 0;
}
static jmethodID kv_GetMethodID(JNIEnv env, jclass c, const char *n, const char *sig) {
    (void)env; (void)c;
    jmethodID m = kv_next_mid; kv_next_mid = (jmethodID)((uintptr_t)kv_next_mid + 8);
    if (kv_nmethods < 1024) {
        kv_methods[kv_nmethods].id = m;
        kv_methods[kv_nmethods].name = strdup(n);
        kv_methods[kv_nmethods].sig = sig;
        kv_nmethods++;
    }
    printf("[jni] GetMethodID(%s, %s) -> %p\n", n, sig, m);
    return m;
}
static jmethodID kv_GetStaticMethodID(JNIEnv env, jclass c, const char *n, const char *sig) {
    return kv_GetMethodID(env, c, n, sig);
}
static jfieldID kv_GetFieldID(JNIEnv env, jclass c, const char *n, const char *sig) {
    (void)env; (void)c; (void)n; (void)sig;
    return (jfieldID)(uintptr_t)0x3000;
}
static jfieldID kv_GetStaticFieldID(JNIEnv env, jclass c, const char *n, const char *sig) {
    return kv_GetFieldID(env, c, n, sig);
}

static jstring kv_NewStringUTF(JNIEnv env, const char *utf) {
    (void)env; return kv_make_jstring(utf);
}
static const char *kv_GetStringUTFChars(JNIEnv env, jstring s, jboolean *copy) {
    (void)env; if (copy) *copy = 0; return kv_resolve_jstring(s);
}
static void kv_ReleaseStringUTFChars(JNIEnv env, jstring s, const char *c) {
    (void)env; (void)s; (void)c;
}
static jint kv_GetStringUTFLength(JNIEnv env, jstring s) {
    (void)env; return (jint)strlen(kv_resolve_jstring(s));
}

static jobject kv_NewGlobalRef(JNIEnv env, jobject o) { (void)env; return o; }
static jobject kv_NewLocalRef(JNIEnv env, jobject o) { (void)env; return o; }
static void kv_DeleteGlobalRef(JNIEnv env, jobject o) { (void)env; (void)o; }
static void kv_DeleteLocalRef(JNIEnv env, jobject o) { (void)env; (void)o; }
static jboolean kv_IsSameObject(JNIEnv env, jobject a, jobject b) { (void)env; return a == b; }
static jclass kv_GetObjectClass(JNIEnv env, jobject o) { (void)env; (void)o; return (jclass)(uintptr_t)0x5000; }
static jboolean kv_IsInstanceOf(JNIEnv env, jobject o, jclass c) { (void)env; (void)o; (void)c; return 1; }
static jclass kv_GetSuperclass(JNIEnv env, jclass c) { (void)env; (void)c; return 0; }
/* ---- native method capture ----
 * libunity.so's JNI_OnLoad calls RegisterNatives to register its native
 * methods (initJni, nativeRender, nativeResume, ...).  We capture the
 * {name, sig, fnPtr} triples so the loader can then CALL those methods to
 * drive Unity's player loop - the correct boot path (not a direct
 * il2cpp_init call, which hits uninitialized globals).
 */
struct kv_native_method { const char *name; const char *sig; void *fn; };
static struct kv_native_method kv_natives[512];
static int kv_natives_count = 0;

void *kv_jni_find_native(const char *name) {
    for (int i = 0; i < kv_natives_count; i++)
        if (strcmp(kv_natives[i].name, name) == 0) return kv_natives[i].fn;
    return 0;
}

static jint kv_RegisterNatives(JNIEnv env, jclass c, const void *methods, jint n) {
    (void)env; (void)c;
    const uintptr_t *m = (const uintptr_t *)methods;   /* {name, sig, fnPtr} x n */
    for (int i = 0; i < n && i < 128; i++) {
        const char *nm = (const char *)m[i * 3];
        const char *sg = (const char *)m[i * 3 + 1];
        void *fn = (void *)m[i * 3 + 2];
        if (nm && kv_natives_count < 512) {
            kv_natives[kv_natives_count].name = strdup(nm);
            kv_natives[kv_natives_count].sig = sg;
            kv_natives[kv_natives_count].fn = fn;
            kv_natives_count++;
        }
    }
    return 0;
}
static jint kv_UnregisterNatives(JNIEnv env, jclass c) { (void)env; (void)c; return 0; }
static jint kv_Throw(JNIEnv env, jthrowable t) { (void)env; (void)t; return 0; }
static jint kv_ThrowNew(JNIEnv env, jclass c, const char *m) { (void)env;(void)c;(void)m; return 0; }
static jthrowable kv_ExceptionOccurred(JNIEnv env) { (void)env; return 0; }
static void kv_ExceptionDescribe(JNIEnv env) { (void)env; }
static void kv_ExceptionClear(JNIEnv env) { (void)env; }
static jboolean kv_ExceptionCheck(JNIEnv env) { (void)env; return 0; }
static void kv_FatalError(JNIEnv env, const char *m) { (void)env; (void)m; }
static jint kv_PushLocalFrame(JNIEnv env, jint c) { (void)env; (void)c; return 0; }
static jobject kv_PopLocalFrame(JNIEnv env, jobject r) { (void)env; (void)r; return r; }
static jint kv_EnsureLocalCapacity(JNIEnv env, jint c) { (void)env; (void)c; return 0; }
static jobject kv_AllocObject(JNIEnv env, jclass c) { (void)env;(void)c; return (jobject)(uintptr_t)0x6000; }
static jobject kv_NewObject(JNIEnv env, jclass c, jmethodID m, ...) { (void)env;(void)c;(void)m; return (jobject)(uintptr_t)0x6000; }
static jobject kv_GetObjectArrayElement(JNIEnv env, jobjectArray a, jint i) { (void)env;(void)a;(void)i; return 0; }
static void kv_SetObjectArrayElement(JNIEnv env, jobjectArray a, jint i, jobject o) { (void)env;(void)a;(void)i;(void)o; }
static jint kv_GetArrayLength(JNIEnv env, jarray a) { (void)env;(void)a; return 0; }
static jobjectArray kv_NewObjectArray(JNIEnv env, jint n, jclass c, jobject i) { (void)env;(void)n;(void)c;(void)i; return (jobjectArray)(uintptr_t)0x7000; }
static jint kv_GetJavaVM(JNIEnv env, JavaVM **vm) { (void)env; if (vm) *vm = kv_jni_java_vm(); return 0; }

/* ---- CallObjectMethodV dispatch: return real values for the methods Unity
 *      calls, modeled on terraria-nextos.  Without getAssets returning a valid
 *      AssetManager (and open() returning a stream), Unity's asset load hits
 *      a null deref -> crash at addr 0 right after initJni. ---- */
static jobject kv_fake_obj = (jobject)(uintptr_t)0x6000;
static jobject kv_assetmgr = (jobject)(uintptr_t)0x6100;   /* fake AssetManager */
static jobject kv_asset_stream = (jobject)(uintptr_t)0x6200;/* fake InputStream */

static jobject kv_CallObjectMethodV(JNIEnv env, jobject obj, jmethodID mid, void *ap) {
    (void)env;
    const char *nm = kv_method_name(mid);
    if (!nm) return kv_fake_obj;
    if (strcmp(nm, "getAssets") == 0) return kv_assetmgr;
    if (strcmp(nm, "open") == 0 || strcmp(nm, "openNonAsset") == 0) {
        /* read the asset path; we don't serve content (no GPU) but must not
         * return null or Unity crashes.  Return the fake stream. */
        return kv_asset_stream;
    }
    if (strcmp(nm, "getFilesDir") == 0 || strcmp(nm, "getExternalFilesDir") == 0 ||
        strcmp(nm, "getCacheDir") == 0 || strcmp(nm, "getDataDir") == 0 ||
        strcmp(nm, "getPath") == 0 || strcmp(nm, "getAbsolutePath") == 0 ||
        strcmp(nm, "getCanonicalPath") == 0) {
        return kv_make_jstring(".");
    }
    if (strcmp(nm, "toString") == 0) return kv_make_jstring("");
    if (strcmp(nm, "getName") == 0 || strcmp(nm, "getCanonicalName") == 0 ||
        strcmp(nm, "getTypeName") == 0) return kv_make_jstring("java.lang.Object");
    /* builders / chained setters return the object itself */
    if (strcmp(nm, "addFlags") == 0 || strcmp(nm, "setFlags") == 0 ||
        strcmp(nm, "setData") == 0 || strcmp(nm, "setAction") == 0 ||
        strcmp(nm, "append") == 0 || strcmp(nm, "edit") == 0) return obj;
    /* fluent prefs editor */
    if (strcmp(nm, "putString") == 0 || strcmp(nm, "putInt") == 0 ||
        strcmp(nm, "putBoolean") == 0 || strcmp(nm, "putFloat") == 0 ||
        strcmp(nm, "putLong") == 0 || strcmp(nm, "remove") == 0) return obj;
    /* queryIntentActivities etc -> empty list */
    if (strcmp(nm, "queryIntentActivities") == 0 || strcmp(nm, "iterator") == 0)
        return kv_fake_obj;
    return kv_fake_obj;
}
static jobject kv_CallObjectMethodA(JNIEnv env, jobject obj, jmethodID mid, const jvalue *args) {
    (void)args; return kv_CallObjectMethodV(env, obj, mid, 0);
}
static jobject kv_CallObj(JNIEnv env, jobject o, jmethodID m, ...) {
    return kv_CallObjectMethodV(env, o, m, 0);
}
static jboolean kv_CallBool(JNIEnv env, jobject o, jmethodID m, ...) { (void)env;(void)o;(void)m; return 0; }
static jint kv_CallInt(JNIEnv env, jobject o, jmethodID m, ...) { (void)env;(void)o;(void)m; return 0; }
static jlong kv_CallLong(JNIEnv env, jobject o, jmethodID m, ...) { (void)env;(void)o;(void)m; return 0; }
static void kv_CallVoid(JNIEnv env, jobject o, jmethodID m, ...) { (void)env;(void)o;(void)m; }
static jfloat kv_CallFloat(JNIEnv env, jobject o, jmethodID m, ...) { (void)env;(void)o;(void)m; return 0; }
static jobject kv_CallStaticObj(JNIEnv env, jclass c, jmethodID m, ...) { (void)env;(void)c;(void)m; return 0; }
static jint kv_CallStaticInt(JNIEnv env, jclass c, jmethodID m, ...) { (void)env;(void)c;(void)m; return 0; }
static void kv_CallStaticVoid(JNIEnv env, jclass c, jmethodID m, ...) { (void)env;(void)c;(void)m; }
static jlong kv_CallStaticLong(JNIEnv env, jclass c, jmethodID m, ...) { (void)env;(void)c;(void)m; return 0; }
static jboolean kv_CallStaticBool(JNIEnv env, jclass c, jmethodID m, ...) { (void)env;(void)c;(void)m; return 0; }
static jobject kv_GetObjectField(JNIEnv env, jobject o, jfieldID f) { (void)env;(void)o;(void)f; return 0; }
static jint kv_GetIntField(JNIEnv env, jobject o, jfieldID f) { (void)env;(void)o;(void)f; return 0; }
static void kv_SetIntField(JNIEnv env, jobject o, jfieldID f, jint v) { (void)env;(void)o;(void)f;(void)v; }
static jobject kv_GetStaticObjectField(JNIEnv env, jclass c, jfieldID f) { (void)env;(void)c;(void)f; return 0; }
static jint kv_GetStaticIntField(JNIEnv env, jclass c, jfieldID f) { (void)env;(void)c;(void)f; return 0; }
static jstring kv_NewString(JNIEnv env, const jchar *u, jint len) { (void)env;(void)u;(void)len; return (jstring)(uintptr_t)0x4000; }
static jint kv_GetStringLength(JNIEnv env, jstring s) { (void)env;(void)s; return 0; }

/* ---- JavaVM vtable ---- */
struct JavaVMFunctionTable_ {
    jint (*reserved0)(JavaVM, void*);
    jint (*reserved1)(JavaVM, void*);
    jint (*reserved2)(JavaVM, void*);
    jint (*reserved3)(JavaVM, void*);
    jint (*GetEnv)(JavaVM, void**, jint);          /* 0x20 - engine reads env via this */
    jint (*DestroyJavaVM)(JavaVM);
    jint (*AttachCurrentThread)(JavaVM, void*, void*);
    jint (*DetachCurrentThread)(JavaVM);
    jint (*AttachCurrentThreadAsDaemon)(JavaVM, void*, void*);
};
struct JavaVM_ { const struct JavaVMFunctionTable_ *functions; };

static jint kv_DestroyJavaVM(JavaVM vm) { (void)vm; return 0; }
static jint kv_AttachCurrentThread(JavaVM vm, void *penv, void *args) {
    (void)vm;(void)args; if (penv) *(void**)penv = kv_jni_env(); return 0;
}
static jint kv_DetachCurrentThread(JavaVM vm) { (void)vm; return 0; }
static jint kv_GetEnv(JavaVM vm, void **penv, jint ver) {
    (void)vm;(void)ver; if (penv) *penv = kv_jni_env(); return 0;
}
static jint kv_AttachCurrentThreadAsDaemon(JavaVM vm, void *penv, void *args) {
    return kv_AttachCurrentThread(vm, penv, args);
}

/* ================= the full JNINativeInterface table ================= */
struct JNINativeInterface_ {
    void *reserved0, *reserved1, *reserved2, *reserved3;
    jint (*GetVersion)(JNIEnv);
    jclass (*DefineClass)(JNIEnv, const char*, jobject, const jbyte*, jint);
    jclass (*FindClass)(JNIEnv, const char*);
    jmethodID (*FromReflectedMethod)(JNIEnv, jobject);
    jfieldID (*FromReflectedField)(JNIEnv, jobject);
    jobject (*ToReflectedMethod)(JNIEnv, jclass, jmethodID, jboolean);
    jclass (*GetSuperclass)(JNIEnv, jclass);
    jboolean (*IsAssignableFrom)(JNIEnv, jclass, jclass);
    jobject (*ToReflectedField)(JNIEnv, jclass, jfieldID, jboolean);
    jint (*Throw)(JNIEnv, jthrowable);
    jint (*ThrowNew)(JNIEnv, jclass, const char*);
    jthrowable (*ExceptionOccurred)(JNIEnv);
    void (*ExceptionDescribe)(JNIEnv);
    void (*ExceptionClear)(JNIEnv);
    void (*FatalError)(JNIEnv, const char*);
    jint (*PushLocalFrame)(JNIEnv, jint);
    jobject (*PopLocalFrame)(JNIEnv, jobject);
    jobject (*NewGlobalRef)(JNIEnv, jobject);
    void (*DeleteGlobalRef)(JNIEnv, jobject);
    void (*DeleteLocalRef)(JNIEnv, jobject);
    jboolean (*IsSameObject)(JNIEnv, jobject, jobject);
    jobject (*NewLocalRef)(JNIEnv, jobject);
    jint (*EnsureLocalCapacity)(JNIEnv, jint);
    jobject (*AllocObject)(JNIEnv, jclass);
    jobject (*NewObject)(JNIEnv, jclass, jmethodID, ...);
    jobject (*NewObjectV)(JNIEnv, jclass, jmethodID, void*);
    jobject (*NewObjectA)(JNIEnv, jclass, jmethodID, const jvalue*);
    jclass (*GetObjectClass)(JNIEnv, jobject);
    jboolean (*IsInstanceOf)(JNIEnv, jobject, jclass);
    jmethodID (*GetMethodID)(JNIEnv, jclass, const char*, const char*);
    jobject (*CallObjectMethod)(JNIEnv, jobject, jmethodID, ...);
    jobject (*CallObjectMethodV)(JNIEnv, jobject, jmethodID, void*);
    jobject (*CallObjectMethodA)(JNIEnv, jobject, jmethodID, const jvalue*);
    jboolean (*CallBooleanMethod)(JNIEnv, jobject, jmethodID, ...);
    jboolean (*CallBooleanMethodV)(JNIEnv, jobject, jmethodID, void*);
    jboolean (*CallBooleanMethodA)(JNIEnv, jobject, jmethodID, const jvalue*);
    jbyte (*CallByteMethod)(JNIEnv, jobject, jmethodID, ...);
    jbyte (*CallByteMethodV)(JNIEnv, jobject, jmethodID, void*);
    jbyte (*CallByteMethodA)(JNIEnv, jobject, jmethodID, const jvalue*);
    jchar (*CallCharMethod)(JNIEnv, jobject, jmethodID, ...);
    jchar (*CallCharMethodV)(JNIEnv, jobject, jmethodID, void*);
    jchar (*CallCharMethodA)(JNIEnv, jobject, jmethodID, const jvalue*);
    jshort (*CallShortMethod)(JNIEnv, jobject, jmethodID, ...);
    jshort (*CallShortMethodV)(JNIEnv, jobject, jmethodID, void*);
    jshort (*CallShortMethodA)(JNIEnv, jobject, jmethodID, const jvalue*);
    jint (*CallIntMethod)(JNIEnv, jobject, jmethodID, ...);
    jint (*CallIntMethodV)(JNIEnv, jobject, jmethodID, void*);
    jint (*CallIntMethodA)(JNIEnv, jobject, jmethodID, const jvalue*);
    jlong (*CallLongMethod)(JNIEnv, jobject, jmethodID, ...);
    jlong (*CallLongMethodV)(JNIEnv, jobject, jmethodID, void*);
    jlong (*CallLongMethodA)(JNIEnv, jobject, jmethodID, const jvalue*);
    jfloat (*CallFloatMethod)(JNIEnv, jobject, jmethodID, ...);
    jfloat (*CallFloatMethodV)(JNIEnv, jobject, jmethodID, void*);
    jfloat (*CallFloatMethodA)(JNIEnv, jobject, jmethodID, const jvalue*);
    jdouble (*CallDoubleMethod)(JNIEnv, jobject, jmethodID, ...);
    jdouble (*CallDoubleMethodV)(JNIEnv, jobject, jmethodID, void*);
    jdouble (*CallDoubleMethodA)(JNIEnv, jobject, jmethodID, const jvalue*);
    void (*CallVoidMethod)(JNIEnv, jobject, jmethodID, ...);
    void (*CallVoidMethodV)(JNIEnv, jobject, jmethodID, void*);
    void (*CallVoidMethodA)(JNIEnv, jobject, jmethodID, const jvalue*);
    jobject (*CallNonvirtualObjectMethod)(JNIEnv, jobject, jclass, jmethodID, ...);
    jobject (*CallNonvirtualObjectMethodV)(JNIEnv, jobject, jclass, jmethodID, void*);
    jobject (*CallNonvirtualObjectMethodA)(JNIEnv, jobject, jclass, jmethodID, const jvalue*);
    jboolean (*CallNonvirtualBooleanMethod)(JNIEnv, jobject, jclass, jmethodID, ...);
    jboolean (*CallNonvirtualBooleanMethodV)(JNIEnv, jobject, jclass, jmethodID, void*);
    jboolean (*CallNonvirtualBooleanMethodA)(JNIEnv, jobject, jclass, jmethodID, const jvalue*);
    jbyte (*CallNonvirtualByteMethod)(JNIEnv, jobject, jclass, jmethodID, ...);
    jbyte (*CallNonvirtualByteMethodV)(JNIEnv, jobject, jclass, jmethodID, void*);
    jbyte (*CallNonvirtualByteMethodA)(JNIEnv, jobject, jclass, jmethodID, const jvalue*);
    jchar (*CallNonvirtualCharMethod)(JNIEnv, jobject, jclass, jmethodID, ...);
    jchar (*CallNonvirtualCharMethodV)(JNIEnv, jobject, jclass, jmethodID, void*);
    jchar (*CallNonvirtualCharMethodA)(JNIEnv, jobject, jclass, jmethodID, const jvalue*);
    jshort (*CallNonvirtualShortMethod)(JNIEnv, jobject, jclass, jmethodID, ...);
    jshort (*CallNonvirtualShortMethodV)(JNIEnv, jobject, jclass, jmethodID, void*);
    jshort (*CallNonvirtualShortMethodA)(JNIEnv, jobject, jclass, jmethodID, const jvalue*);
    jint (*CallNonvirtualIntMethod)(JNIEnv, jobject, jclass, jmethodID, ...);
    jint (*CallNonvirtualIntMethodV)(JNIEnv, jobject, jclass, jmethodID, void*);
    jint (*CallNonvirtualIntMethodA)(JNIEnv, jobject, jclass, jmethodID, const jvalue*);
    jlong (*CallNonvirtualLongMethod)(JNIEnv, jobject, jclass, jmethodID, ...);
    jlong (*CallNonvirtualLongMethodV)(JNIEnv, jobject, jclass, jmethodID, void*);
    jlong (*CallNonvirtualLongMethodA)(JNIEnv, jobject, jclass, jmethodID, const jvalue*);
    jfloat (*CallNonvirtualFloatMethod)(JNIEnv, jobject, jclass, jmethodID, ...);
    jfloat (*CallNonvirtualFloatMethodV)(JNIEnv, jobject, jclass, jmethodID, void*);
    jfloat (*CallNonvirtualFloatMethodA)(JNIEnv, jobject, jclass, jmethodID, const jvalue*);
    jdouble (*CallNonvirtualDoubleMethod)(JNIEnv, jobject, jclass, jmethodID, ...);
    jdouble (*CallNonvirtualDoubleMethodV)(JNIEnv, jobject, jclass, jmethodID, void*);
    jdouble (*CallNonvirtualDoubleMethodA)(JNIEnv, jobject, jclass, jmethodID, const jvalue*);
    void (*CallNonvirtualVoidMethod)(JNIEnv, jobject, jclass, jmethodID, ...);
    void (*CallNonvirtualVoidMethodV)(JNIEnv, jobject, jclass, jmethodID, void*);
    void (*CallNonvirtualVoidMethodA)(JNIEnv, jobject, jclass, jmethodID, const jvalue*);
    jfieldID (*GetFieldID)(JNIEnv, jclass, const char*, const char*);
    jobject (*GetObjectField)(JNIEnv, jobject, jfieldID);
    jboolean (*GetBooleanField)(JNIEnv, jobject, jfieldID);
    jbyte (*GetByteField)(JNIEnv, jobject, jfieldID);
    jchar (*GetCharField)(JNIEnv, jobject, jfieldID);
    jshort (*GetShortField)(JNIEnv, jobject, jfieldID);
    jint (*GetIntField)(JNIEnv, jobject, jfieldID);
    jlong (*GetLongField)(JNIEnv, jobject, jfieldID);
    jfloat (*GetFloatField)(JNIEnv, jobject, jfieldID);
    jdouble (*GetDoubleField)(JNIEnv, jobject, jfieldID);
    void (*SetObjectField)(JNIEnv, jobject, jfieldID, jobject);
    void (*SetBooleanField)(JNIEnv, jobject, jfieldID, jboolean);
    void (*SetByteField)(JNIEnv, jobject, jfieldID, jbyte);
    void (*SetCharField)(JNIEnv, jobject, jfieldID, jchar);
    void (*SetShortField)(JNIEnv, jobject, jfieldID, jshort);
    void (*SetIntField)(JNIEnv, jobject, jfieldID, jint);
    void (*SetLongField)(JNIEnv, jobject, jfieldID, jlong);
    void (*SetFloatField)(JNIEnv, jobject, jfieldID, jfloat);
    void (*SetDoubleField)(JNIEnv, jobject, jfieldID, jdouble);
    jmethodID (*GetStaticMethodID)(JNIEnv, jclass, const char*, const char*);
    jobject (*CallStaticObjectMethod)(JNIEnv, jclass, jmethodID, ...);
    jobject (*CallStaticObjectMethodV)(JNIEnv, jclass, jmethodID, void*);
    jobject (*CallStaticObjectMethodA)(JNIEnv, jclass, jmethodID, const jvalue*);
    jboolean (*CallStaticBooleanMethod)(JNIEnv, jclass, jmethodID, ...);
    jboolean (*CallStaticBooleanMethodV)(JNIEnv, jclass, jmethodID, void*);
    jboolean (*CallStaticBooleanMethodA)(JNIEnv, jclass, jmethodID, const jvalue*);
    jbyte (*CallStaticByteMethod)(JNIEnv, jclass, jmethodID, ...);
    jbyte (*CallStaticByteMethodV)(JNIEnv, jclass, jmethodID, void*);
    jbyte (*CallStaticByteMethodA)(JNIEnv, jclass, jmethodID, const jvalue*);
    jchar (*CallStaticCharMethod)(JNIEnv, jclass, jmethodID, ...);
    jchar (*CallStaticCharMethodV)(JNIEnv, jclass, jmethodID, void*);
    jchar (*CallStaticCharMethodA)(JNIEnv, jclass, jmethodID, const jvalue*);
    jshort (*CallStaticShortMethod)(JNIEnv, jclass, jmethodID, ...);
    jshort (*CallStaticShortMethodV)(JNIEnv, jclass, jmethodID, void*);
    jshort (*CallStaticShortMethodA)(JNIEnv, jclass, jmethodID, const jvalue*);
    jint (*CallStaticIntMethod)(JNIEnv, jclass, jmethodID, ...);
    jint (*CallStaticIntMethodV)(JNIEnv, jclass, jmethodID, void*);
    jint (*CallStaticIntMethodA)(JNIEnv, jclass, jmethodID, const jvalue*);
    jlong (*CallStaticLongMethod)(JNIEnv, jclass, jmethodID, ...);
    jlong (*CallStaticLongMethodV)(JNIEnv, jclass, jmethodID, void*);
    jlong (*CallStaticLongMethodA)(JNIEnv, jclass, jmethodID, const jvalue*);
    jfloat (*CallStaticFloatMethod)(JNIEnv, jclass, jmethodID, ...);
    jfloat (*CallStaticFloatMethodV)(JNIEnv, jclass, jmethodID, void*);
    jfloat (*CallStaticFloatMethodA)(JNIEnv, jclass, jmethodID, const jvalue*);
    jdouble (*CallStaticDoubleMethod)(JNIEnv, jclass, jmethodID, ...);
    jdouble (*CallStaticDoubleMethodV)(JNIEnv, jclass, jmethodID, void*);
    jdouble (*CallStaticDoubleMethodA)(JNIEnv, jclass, jmethodID, const jvalue*);
    void (*CallStaticVoidMethod)(JNIEnv, jclass, jmethodID, ...);
    void (*CallStaticVoidMethodV)(JNIEnv, jclass, jmethodID, void*);
    void (*CallStaticVoidMethodA)(JNIEnv, jclass, jmethodID, const jvalue*);
    jfieldID (*GetStaticFieldID)(JNIEnv, jclass, const char*, const char*);
    jobject (*GetStaticObjectField)(JNIEnv, jclass, jfieldID);
    jboolean (*GetStaticBooleanField)(JNIEnv, jclass, jfieldID);
    jbyte (*GetStaticByteField)(JNIEnv, jclass, jfieldID);
    jchar (*GetStaticCharField)(JNIEnv, jclass, jfieldID);
    jshort (*GetStaticShortField)(JNIEnv, jclass, jfieldID);
    jint (*GetStaticIntField)(JNIEnv, jclass, jfieldID);
    jlong (*GetStaticLongField)(JNIEnv, jclass, jfieldID);
    jfloat (*GetStaticFloatField)(JNIEnv, jclass, jfieldID);
    jdouble (*GetStaticDoubleField)(JNIEnv, jclass, jfieldID);
    void (*SetStaticObjectField)(JNIEnv, jclass, jfieldID, jobject);
    void (*SetStaticBooleanField)(JNIEnv, jclass, jfieldID, jboolean);
    void (*SetStaticByteField)(JNIEnv, jclass, jfieldID, jbyte);
    void (*SetStaticCharField)(JNIEnv, jclass, jfieldID, jchar);
    void (*SetStaticShortField)(JNIEnv, jclass, jfieldID, jshort);
    void (*SetStaticIntField)(JNIEnv, jclass, jfieldID, jint);
    void (*SetStaticLongField)(JNIEnv, jclass, jfieldID, jlong);
    void (*SetStaticFloatField)(JNIEnv, jclass, jfieldID, jfloat);
    void (*SetStaticDoubleField)(JNIEnv, jclass, jfieldID, jdouble);
    jstring (*NewString)(JNIEnv, const jchar*, jint);
    jint (*GetStringLength)(JNIEnv, jstring);
    const jchar* (*GetStringChars)(JNIEnv, jstring, jboolean*);
    void (*ReleaseStringChars)(JNIEnv, jstring, const jchar*);
    jstring (*NewStringUTF)(JNIEnv, const char*);
    jint (*GetStringUTFLength)(JNIEnv, jstring);
    const char* (*GetStringUTFChars)(JNIEnv, jstring, jboolean*);
    void (*ReleaseStringUTFChars)(JNIEnv, jstring, const char*);
    jint (*GetArrayLength)(JNIEnv, jarray);
    jobjectArray (*NewObjectArray)(JNIEnv, jint, jclass, jobject);
    jobject (*GetObjectArrayElement)(JNIEnv, jobjectArray, jint);
    void (*SetObjectArrayElement)(JNIEnv, jobjectArray, jint, jobject);
    jbooleanArray (*NewBooleanArray)(JNIEnv, jint);
    jbyteArray (*NewByteArray)(JNIEnv, jint);
    jcharArray (*NewCharArray)(JNIEnv, jint);
    jshortArray (*NewShortArray)(JNIEnv, jint);
    jintArray (*NewIntArray)(JNIEnv, jint);
    jlongArray (*NewLongArray)(JNIEnv, jint);
    jfloatArray (*NewFloatArray)(JNIEnv, jint);
    jdoubleArray (*NewDoubleArray)(JNIEnv, jint);
    jboolean* (*GetBooleanArrayElements)(JNIEnv, jbooleanArray, jboolean*);
    jbyte* (*GetByteArrayElements)(JNIEnv, jbyteArray, jboolean*);
    jchar* (*GetCharArrayElements)(JNIEnv, jcharArray, jboolean*);
    jshort* (*GetShortArrayElements)(JNIEnv, jshortArray, jboolean*);
    jint* (*GetIntArrayElements)(JNIEnv, jintArray, jboolean*);
    jlong* (*GetLongArrayElements)(JNIEnv, jlongArray, jboolean*);
    jfloat* (*GetFloatArrayElements)(JNIEnv, jfloatArray, jboolean*);
    jdouble* (*GetDoubleArrayElements)(JNIEnv, jdoubleArray, jboolean*);
    void (*ReleaseBooleanArrayElements)(JNIEnv, jbooleanArray, jboolean*, jint);
    void (*ReleaseByteArrayElements)(JNIEnv, jbyteArray, jbyte*, jint);
    void (*ReleaseCharArrayElements)(JNIEnv, jcharArray, jchar*, jint);
    void (*ReleaseShortArrayElements)(JNIEnv, jshortArray, jshort*, jint);
    void (*ReleaseIntArrayElements)(JNIEnv, jintArray, jint*, jint);
    void (*ReleaseLongArrayElements)(JNIEnv, jlongArray, jlong*, jint);
    void (*ReleaseFloatArrayElements)(JNIEnv, jfloatArray, jfloat*, jint);
    void (*ReleaseDoubleArrayElements)(JNIEnv, jdoubleArray, jdouble*, jint);
    void (*GetBooleanArrayRegion)(JNIEnv, jbooleanArray, jint, jint, jboolean*);
    void (*GetByteArrayRegion)(JNIEnv, jbyteArray, jint, jint, jbyte*);
    void (*GetCharArrayRegion)(JNIEnv, jcharArray, jint, jint, jchar*);
    void (*GetShortArrayRegion)(JNIEnv, jshortArray, jint, jint, jshort*);
    void (*GetIntArrayRegion)(JNIEnv, jintArray, jint, jint, jint*);
    void (*GetLongArrayRegion)(JNIEnv, jlongArray, jint, jint, jlong*);
    void (*GetFloatArrayRegion)(JNIEnv, jfloatArray, jint, jint, jfloat*);
    void (*GetDoubleArrayRegion)(JNIEnv, jdoubleArray, jint, jint, jdouble*);
    void (*SetBooleanArrayRegion)(JNIEnv, jbooleanArray, jint, jint, const jboolean*);
    void (*SetByteArrayRegion)(JNIEnv, jbyteArray, jint, jint, const jbyte*);
    void (*SetCharArrayRegion)(JNIEnv, jcharArray, jint, jint, const jchar*);
    void (*SetShortArrayRegion)(JNIEnv, jshortArray, jint, jint, const jshort*);
    void (*SetIntArrayRegion)(JNIEnv, jintArray, jint, jint, const jint*);
    void (*SetLongArrayRegion)(JNIEnv, jlongArray, jint, jint, const jlong*);
    void (*SetFloatArrayRegion)(JNIEnv, jfloatArray, jint, jint, const jfloat*);
    void (*SetDoubleArrayRegion)(JNIEnv, jdoubleArray, jint, jint, const jdouble*);
    jint (*RegisterNatives)(JNIEnv, jclass, const void*, jint);
    jint (*UnregisterNatives)(JNIEnv, jclass);
    jint (*MonitorEnter)(JNIEnv, jobject);
    jint (*MonitorExit)(JNIEnv, jobject);
    jint (*GetJavaVM)(JNIEnv, JavaVM**);
    void (*GetStringRegion)(JNIEnv, jstring, jint, jint, jchar*);
    void (*GetStringUTFRegion)(JNIEnv, jstring, jint, jint, char*);
    void* (*GetPrimitiveArrayCritical)(JNIEnv, jarray, jboolean*);
    void (*ReleasePrimitiveArrayCritical)(JNIEnv, jarray, void*, jint);
    const jchar* (*GetStringCritical)(JNIEnv, jstring, jboolean*);
    void (*ReleaseStringCritical)(JNIEnv, jstring, const jchar*);
    jweak (*NewWeakGlobalRef)(JNIEnv, jobject);
    void (*DeleteWeakGlobalRef)(JNIEnv, jweak);
    jboolean (*ExceptionCheck)(JNIEnv);
    jobject (*NewDirectByteBuffer)(JNIEnv, void*, jlong);
    void* (*GetDirectBufferAddress)(JNIEnv, jobject);
    jlong (*GetDirectBufferCapacity)(JNIEnv, jobject);
    jobjectRefType (*GetObjectRefType)(JNIEnv, jobject);
};

/* generic "unsupported" pointer so every slot is non-null */
static void *kv_unsupported(void) { return 0; }

const struct JNINativeInterface_ kv_jni_table = {
    /* 0-3 reserved */
    0,0,0,0,
    kv_GetVersion, kv_unsupported, kv_FindClass, kv_unsupported, kv_unsupported,
    kv_unsupported, kv_GetSuperclass, kv_unsupported, kv_unsupported,
    kv_Throw, kv_ThrowNew, kv_ExceptionOccurred, kv_ExceptionDescribe,
    kv_ExceptionClear, kv_FatalError, kv_PushLocalFrame, kv_PopLocalFrame,
    kv_NewGlobalRef, kv_DeleteGlobalRef, kv_DeleteLocalRef, kv_IsSameObject,
    kv_NewLocalRef, kv_EnsureLocalCapacity, kv_AllocObject, kv_NewObject,
    kv_unsupported, kv_unsupported, kv_GetObjectClass, kv_IsInstanceOf,
    kv_GetMethodID,
    kv_CallObj, kv_CallObjectMethodV, kv_CallObjectMethodA,
    kv_CallBool, kv_unsupported, kv_unsupported,
    kv_unsupported, kv_unsupported, kv_unsupported,
    kv_unsupported, kv_unsupported, kv_unsupported,
    kv_unsupported, kv_unsupported, kv_unsupported,
    kv_CallInt, kv_unsupported, kv_unsupported,
    kv_CallLong, kv_unsupported, kv_unsupported,
    kv_CallFloat, kv_unsupported, kv_unsupported,
    kv_unsupported, kv_unsupported, kv_unsupported,
    kv_CallVoid, kv_unsupported, kv_unsupported,
    kv_unsupported, kv_unsupported, kv_unsupported,
    kv_CallBool, kv_unsupported, kv_unsupported,
    kv_unsupported, kv_unsupported, kv_unsupported,
    kv_unsupported, kv_unsupported, kv_unsupported,
    kv_unsupported, kv_unsupported, kv_unsupported,
    kv_CallInt, kv_unsupported, kv_unsupported,
    kv_CallLong, kv_unsupported, kv_unsupported,
    kv_CallFloat, kv_unsupported, kv_unsupported,
    kv_unsupported, kv_unsupported, kv_unsupported,
    kv_CallVoid, kv_unsupported, kv_unsupported,
    kv_GetFieldID,
    kv_GetObjectField, kv_unsupported, kv_unsupported, kv_unsupported,
    kv_unsupported, kv_GetIntField, kv_unsupported, kv_unsupported,
    kv_unsupported,
    kv_unsupported, kv_unsupported, kv_unsupported, kv_unsupported,
    kv_unsupported, kv_SetIntField, kv_unsupported, kv_unsupported,
    kv_unsupported,
    kv_GetStaticMethodID,
    kv_CallStaticObj, kv_unsupported, kv_unsupported,
    kv_CallStaticBool, kv_unsupported, kv_unsupported,
    kv_unsupported, kv_unsupported, kv_unsupported,
    kv_unsupported, kv_unsupported, kv_unsupported,
    kv_unsupported, kv_unsupported, kv_unsupported,
    kv_CallStaticInt, kv_unsupported, kv_unsupported,
    kv_CallStaticLong, kv_unsupported, kv_unsupported,
    kv_unsupported, kv_unsupported, kv_unsupported,
    kv_unsupported, kv_unsupported, kv_unsupported,
    kv_CallStaticVoid, kv_unsupported, kv_unsupported,
    kv_GetStaticFieldID,
    kv_GetStaticObjectField, kv_unsupported, kv_unsupported, kv_unsupported,
    kv_unsupported, kv_GetStaticIntField, kv_unsupported, kv_unsupported,
    kv_unsupported,
    kv_unsupported, kv_unsupported, kv_unsupported, kv_unsupported,
    kv_unsupported, kv_unsupported, kv_unsupported, kv_unsupported,
    kv_unsupported,
    kv_NewString, kv_GetStringLength, kv_unsupported, kv_unsupported,
    kv_NewStringUTF, kv_GetStringUTFLength, kv_GetStringUTFChars, kv_ReleaseStringUTFChars,
    kv_GetArrayLength, kv_NewObjectArray, kv_GetObjectArrayElement, kv_SetObjectArrayElement,
    kv_unsupported, kv_unsupported, kv_unsupported, kv_unsupported, kv_unsupported,
    kv_unsupported, kv_unsupported, kv_unsupported,
    kv_unsupported, kv_unsupported, kv_unsupported, kv_unsupported, kv_unsupported,
    kv_unsupported, kv_unsupported, kv_unsupported,
    kv_unsupported, kv_unsupported, kv_unsupported, kv_unsupported, kv_unsupported,
    kv_unsupported, kv_unsupported, kv_unsupported,
    kv_unsupported, kv_unsupported, kv_unsupported, kv_unsupported, kv_unsupported,
    kv_unsupported, kv_unsupported, kv_unsupported,
    kv_unsupported, kv_unsupported, kv_unsupported, kv_unsupported, kv_unsupported,
    kv_unsupported, kv_unsupported, kv_unsupported,
    kv_RegisterNatives, kv_UnregisterNatives,
    kv_unsupported, kv_unsupported,
    kv_GetJavaVM,
    kv_unsupported, kv_unsupported,
    kv_unsupported, kv_unsupported, kv_unsupported, kv_unsupported,
    kv_unsupported, kv_unsupported, kv_unsupported,
    kv_ExceptionCheck,
    kv_unsupported, kv_unsupported, kv_unsupported,
    kv_unsupported,
};

static jint kv_vm_reserved(JavaVM vm, void *a) { (void)vm; (void)a; return 0; }
static JNIEnvS kv_env = { &kv_jni_table };
static const struct JavaVMFunctionTable_ kv_vm_table = {
    kv_vm_reserved, kv_vm_reserved, kv_vm_reserved, kv_vm_reserved,
    kv_GetEnv,                    /* 0x20 */
    kv_DestroyJavaVM, kv_AttachCurrentThread, kv_DetachCurrentThread,
    kv_AttachCurrentThreadAsDaemon,
};
static struct JavaVM_ kv_vm = { &kv_vm_table };

/* Public: get the JavaVM and JNIEnv the loader can hand to libunity's
 * JNI_OnLoad.  `*out_env` is the JNIEnv** value callers pass as the first
 * argument to JNI functions. */
void *kv_jni_java_vm(void) { return &kv_vm; }
void *kv_jni_env(void) { return &kv_env; }

/* Convenience: lookup a module's exported function by name (e.g. JNI_OnLoad). */
typedef void (*kv_fn_void)(void);
