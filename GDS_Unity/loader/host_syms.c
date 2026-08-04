/* host_syms.c - symbols the loaded .so resolves against.
 *
 * libil2cpp.so is built for bionic and imports a large slice of libc/libm/
 * pthread/locale from the system.  The native loader resolves them here.  On
 * the device these come from glibc; under the Unicorn bench we provide
 * freestanding versions so the .so's init_array can run headless.
 *
 * The implementations live in freestdlib.c (allocator, mem/str, printf, time,
 * pthread stubs) and in this file (math/locale/etc. stubs).  host_dlsym()
 * maps the imported name to the function pointer by an explicit table.
 */
#include <stddef.h>
#include <stdint.h>
#include "kv_elf.h"
#include "kv_libc.h"
#ifdef KV_USE_GLIBC
#include <dlfcn.h>
#endif

#ifndef KV_USE_GLIBC

/* ---- math stubs (return values good enough to keep init_array alive) ---- */
static double kv_nan(void) { union { double d; uint64_t u; } x; x.u = 0x7ff8000000000000ULL; return x.d; }
double cos(double x) { (void)x; return 1.0; }
float  cosf(float x) { (void)x; return 1.0f; }
double sin(double x) { (void)x; return 0.0; }
float  sinf(float x) { (void)x; return 0.0f; }
double tan(double x) { (void)x; return 0.0; }
float  tanf(float x) { (void)x; return 0.0f; }
double acos(double x) { (void)x; return 0.0; }
float  acosf(float x) { (void)x; return 0.0f; }
double asin(double x) { (void)x; return 0.0; }
double atan(double x) { (void)x; return 0.0; }
float  atanf(float x) { (void)x; return 0.0f; }
double atan2(double y, double x) { (void)y;(void)x; return 0.0; }
float  atan2f(float y, float x) { (void)y;(void)x; return 0.0f; }
double log(double x) { (void)x; return 0.0; }
double log10(double x) { (void)x; return 0.0; }
float  log10f(float x) { (void)x; return 0.0f; }
double log2(double x) { (void)x; return 0.0; }
double logb(double x) { (void)x; return 0.0; }
double exp2f(float x) { (void)x; return 1.0f; }
double pow(double a, double b) { (void)a;(void)b; return 1.0; }
float  powf(float a, float b) { (void)a;(void)b; return 1.0f; }
double sqrt(double x) { (void)x; return 1.0; }
double fmod(double a, double b) { (void)a;(void)b; return 0.0; }
float  fmodf(float a, float b) { (void)a;(void)b; return 0.0f; }
double hypot(double a, double b) { (void)a;(void)b; return 0.0; }
double modf(double x, double *i) { (void)x; if (i) *i = 0; return 0.0; }
double scalbn(double x, int n) { (void)x;(void)n; return 0.0; }
double difftime(double a, double b) { return a - b; }
void sincosf(float x, float *s, float *c) { (void)x; if (s) *s = 0; if (c) *c = 1; }
void *android_set_abort_message(void *m) { (void)m; return 0; }

/* ---- locale / wchar stubs ---- */
void *newlocale(int m, const char *l, void *b) { (void)m;(void)l;(void)b; return 0; }
void *freelocale(void *l) { (void)l; return 0; }
void *uselocale(void *l) { (void)l; return 0; }
void *localeconv(void) { return 0; }
void *setlocale(int c, const char *l) { (void)c;(void)l; return 0; }
int isdigit_l(int c, void *l) { (void)l; return c >= '0' && c <= '9'; }
int isxdigit_l(int c, void *l) { (void)l; return (c>='0'&&c<='9')||(c>='a'&&c<='f')||(c>='A'&&c<='F'); }
int islower_l(int c, void *l) { (void)l; return c >= 'a' && c <= 'z'; }
int isupper_l(int c, void *l) { (void)l; return c >= 'A' && c <= 'Z'; }
int iswalpha_l(int c, void *l) { (void)l; (void)c; return 0; }
int iswblank_l(int c, void *l) { (void)l;(void)c; return 0; }
int iswcntrl_l(int c, void *l) { (void)l;(void)c; return 0; }
int iswdigit_l(int c, void *l) { (void)l;(void)c; return 0; }
int iswlower_l(int c, void *l) { (void)l;(void)c; return 0; }
int iswprint_l(int c, void *l) { (void)l;(void)c; return 0; }
int iswpunct_l(int c, void *l) { (void)l;(void)c; return 0; }
int iswspace_l(int c, void *l) { (void)l;(void)c; return 0; }
int iswupper_l(int c, void *l) { (void)l;(void)c; return 0; }
int iswxdigit_l(int c, void *l) { (void)l;(void)c; return 0; }
int towlower(int c) { (void)c; return c; }
int towupper(int c) { (void)c; return c; }
int towlower_l(int c, void *l) { (void)l; return c; }
int towupper_l(int c, void *l) { (void)l; return c; }
int tolower_l(int c, void *l) { (void)l; return c >= 'A' && c <= 'Z' ? c + 32 : c; }
int toupper_l(int c, void *l) { (void)l; return c >= 'a' && c <= 'z' ? c - 32 : c; }
int btowc(int c) { return c; }
int wctob(int c) { return c; }
unsigned long wcslen(const void *s) { (void)s; return 0; }
void *wmemchr(const void *s, int c, unsigned long n) { (void)s;(void)c;(void)n; return 0; }
int wmemcmp(const void *a, const void *b, unsigned long n) { (void)a;(void)b;(void)n; return 0; }
void *wmemcpy(void *d, const void *s, unsigned long n) { (void)s;(void)n; return d; }
void *wmemmove(void *d, const void *s, unsigned long n) { (void)s;(void)n; return d; }
void *wmemset(void *d, int c, unsigned long n) { (void)c;(void)n; return d; }
unsigned long mbrlen(const char *s, unsigned long n, void *st) { (void)s;(void)n;(void)st; return 1; }
unsigned long mbrtowc(void *wc, const char *s, unsigned long n, void *st) { (void)wc;(void)s;(void)n;(void)st; return 1; }
unsigned long mbtowc(void *wc, const char *s, unsigned long n) { (void)wc;(void)s;(void)n; return 1; }
unsigned long wcrtomb(char *s, int wc, void *st) { (void)s;(void)wc;(void)st; return 1; }
unsigned long mbsrtowcs(void *d, const void *ss, unsigned long n, void *st) { (void)d;(void)ss;(void)n;(void)st; return 1; }
unsigned long mbsnrtowcs(void *d, const void *ss, unsigned long n, void *st) { (void)d;(void)ss;(void)n;(void)st; return 1; }
unsigned long wcsnrtombs(char *d, const void *ss, unsigned long n, void *st) { (void)d;(void)ss;(void)n;(void)st; return 1; }
int __ctype_get_mb_cur_max(void) { return 1; }

/* ---- misc libc ---- */
int __system_property_get(const char *name, char *value) {
    (void)name; if (value) value[0] = 0; return 0;
}
int div(int a, int b) { return a / b; }
void *bsearch(const void *k, const void *b, unsigned long n, unsigned long sz,
              int (*cmp)(const void *, const void *)) {
    (void)k;(void)b;(void)n;(void)sz;(void)cmp; return 0;
}
void qsort(void *b, unsigned long n, unsigned long sz, int (*cmp)(const void *, const void *)) {
    (void)b;(void)n;(void)sz;(void)cmp;
}
long lrand48(void) { return 0; }
void srand48(long x) { (void)x; }
void *gmtime(const void *t) { (void)t; return 0; }
void *localtime(const void *t) { (void)t; return 0; }
unsigned long mktime(void *t) { (void)t; return 0; }
unsigned long strftime(char *s, unsigned long n, const char *f, const void *tm) {
    (void)s;(void)n;(void)f;(void)tm; return 0;
}
void *strftime_l(void *s, unsigned long n, void *f, void *tm, void *l) { (void)s;(void)n;(void)f;(void)tm;(void)l; return 0; }
int strcoll_l(const char *a, const char *b, void *l) { (void)l; return strcmp(a, b); }
unsigned long strxfrm_l(char *d, const char *s, unsigned long n, void *l) { (void)d;(void)s;(void)n;(void)l; return 0; }
int wcscoll_l(const void *a, const void *b, void *l) { (void)a;(void)b;(void)l; return 0; }
unsigned long wcsxfrm_l(void *d, const void *s, unsigned long n, void *l) { (void)d;(void)s;(void)n;(void)l; return 0; }
/* real fopen/fclose/fread/fwrite/fputc/fseek/fseeko/ftello live in freestdlib.c */
int access(const char *p, int m) { (void)p;(void)m; return -1; }
int unlink(const char *p) { (void)p; return -1; }
int rename(const char *a, const char *b) { (void)a;(void)b; return -1; }
int chmod(const char *p, int m) { (void)p;(void)m; return 0; }
int fchmod(int fd, int m) { (void)fd;(void)m; return 0; }
int mkdir(const char *p, int m) { (void)p;(void)m; return 0; }
int rmdir(const char *p) { (void)p; return 0; }
int link(const char *a, const char *b) { (void)a;(void)b; return -1; }
int symlink(const char *a, const char *b) { (void)a;(void)b; return -1; }
int readlink(const char *p, char *b, unsigned long n) { (void)p;(void)b;(void)n; return -1; }
/* lseek is provided by freestdlib.c (needs to actually seek for read_all) */
int dup(int fd) { (void)fd; return -1; }
int ftruncate(int fd, long l) { (void)fd;(void)l; return 0; }
int futimens(int fd, void *t) { (void)fd;(void)t; return 0; }
int utimes(const char *p, void *t) { (void)p;(void)t; return 0; }
int ioctl(int fd, unsigned long r, ...) { (void)fd;(void)r; return 0; }
int pipe(int *p) { (void)p; return -1; }
int select(int n, void *r, void *w, void *e, void *t) { (void)n;(void)r;(void)w;(void)e;(void)t; return 0; }
long sendfile(int o, int i, long *off, unsigned long n) { (void)o;(void)i;(void)off;(void)n; return 0; }
void *opendir(const char *p) { (void)p; return 0; }
void *readdir(void *d) { (void)d; return 0; }
int closedir(void *d) { (void)d; return 0; }
int stat(const char *p, void *st) { (void)p; if (st) memset(st, 0, 144); return -1; }
int lstat(const char *p, void *st) { (void)p; if (st) memset(st, 0, 144); return -1; }
/* POSIX signal-set fns return int 0 on success.  bionic sigset_t is 8 bytes;
 * glibc's is 128.  The .so passes a bionic-sized sigset; we don't actually use
 * it for boot, so just set bit 0 and return 0 (success).  Returning the pointer
 * (non-zero) made the runtime think sigfillset "failed". */
int sigaction(int s, const void *a, void *o) { (void)s;(void)a;(void)o; return 0; }
int signal(int s, void *h) { (void)s;(void)h; return 0; }
int sigaltstack(const void *ss, void *os) { (void)ss; if (os) memset(os, 0, 24); return 0; }
int sigemptyset(void *s) { (void)s; return 0; }
int sigfillset(void *s) { (void)s; return 0; }
int sigaddset(void *s, int n) { (void)s;(void)n; return 0; }
int sigdelset(void *s, int n) { (void)s;(void)n; return 0; }
int sigsuspend(void *m) { (void)m; return -1; }
int madvise(void *a, unsigned long l, int adv) { (void)a;(void)l;(void)adv; return 0; }
int dladdr(const void *a, void *i) { (void)a;(void)i; return 0; }
int dlclose(void *h) { (void)h; return 0; }
char *dlerror(void) { return 0; }
int dl_iterate_phdr(void *c, void *d) { (void)c;(void)d; return 0; }
long syscall(long n, ...) { (void)n; return -1; }
int setjmp(void *e) { (void)e; return 0; }
void longjmp(void *e, int v) { (void)e;(void)v; for (;;) {} }
int strerror_r(int e, char *b, unsigned long n) { (void)e;(void)n; if (b) b[0]=0; return 0; }
int snprintf(char *s, unsigned long n, const char *f, ...) { (void)s;(void)n;(void)f; return 0; }
int sprintf(char *s, const char *f, ...) { (void)s;(void)f; return 0; }
int vsnprintf(char *s, unsigned long n, const char *f, void *a) { (void)s;(void)n;(void)f;(void)a; return 0; }
int swprintf(void *s, unsigned long n, const void *f, ...) { (void)s;(void)n;(void)f; return 0; }
int vfprintf(void *f, const char *fmt, void *a) { (void)f;(void)fmt;(void)a; return 0; }
int vasprintf(char **o, const char *f, void *a) { (void)o;(void)f;(void)a; return -1; }
int sscanf(const char *s, const char *f, ...) { (void)s;(void)f; return 0; }
int vsscanf(const char *s, const char *f, void *a) { (void)s;(void)f;(void)a; return 0; }
int openlog(const char *i, int o, int f) { (void)i;(void)o;(void)f; return 0; }
int syslog(int p, const char *f, ...) { (void)p;(void)f; return 0; }
int closelog(void) { return 0; }
int __android_log_print(int prio, const char *tag, const char *fmt, ...) {
    (void)prio;(void)tag;(void)fmt; return 0;
}
int __android_log_vprint(int prio, const char *tag, const char *fmt, void *ap) {
    (void)prio;(void)tag;(void)fmt;(void)ap; return 0;
}
int __android_log_write(int prio, const char *tag, const char *msg) {
    (void)prio;(void)tag;(void)msg; return 0;
}
int uname(void *u) {
    /* sys_utsname: 6 fixed char fields + version; zero it so .so boot doesn't
     * read garbage. */
    memset(u, 0, 390);
    return 0;
}
double strtod(const char *s, char **e) { (void)e; return 0.0; }
float strtof(const char *s, char **e) { (void)e; return 0.0f; }
long double strtold(const char *s, char **e) { (void)e; return 0.0; }
long double strtold_l(const char *s, char **e, void *l) { (void)e;(void)l; return 0.0; }
long long strtoll_l(const char *s, char **e, int b, void *l) { (void)l; return strtoll(s, e, b); }
unsigned long long strtoull_l(const char *s, char **e, int b, void *l) { (void)l; return strtoull(s, e, b); }
double wcstod(const void *s, void **e) { (void)s;(void)e; return 0.0; }
float wcstof(const void *s, void **e) { (void)s;(void)e; return 0.0f; }
long double wcstold(const void *s, void **e) { (void)s;(void)e; return 0.0; }
long wcstol(const void *s, void **e, int b) { (void)s;(void)e;(void)b; return 0; }
long long wcstoll(const void *s, void **e, int b) { (void)s;(void)e;(void)b; return 0; }
unsigned long wcstoul(const void *s, void **e, int b) { (void)s;(void)e;(void)b; return 0; }
unsigned long long wcstoull(const void *s, void **e, int b) { (void)s;(void)e;(void)b; return 0; }

/* ---- libunity.so: NDK native (ALooper / ANativeWindow / ASensor) ---- */
void *ALooper_acquire(void) { return (void *)1; }
void *ALooper_forThread(void) { return 0; }
int ALooper_pollOnce(int t, int *o, void *e, void *d) { (void)t;(void)o;(void)e;(void)d; return 0; }
void *ALooper_prepare(int o) { (void)o; return (void *)1; }
void ALooper_release(void) {}
void ALooper_wake(void) {}
void ANativeWindow_acquire(void *w) { (void)w; }
/* Unity's nativeRecreateGfxState needs a REAL ANativeWindow (not (void*)1).
 * fbdev: it's a struct {u16 w, u16 h}.  Return a valid one. */
static struct { unsigned short w, h; } kv_fbdev_win = { 640, 480 };
void *ANativeWindow_fromSurface(void *s) { (void)s; return &kv_fbdev_win; }
int ANativeWindow_getHeight(void *w) { (void)w; return 480; }
int ANativeWindow_getWidth(void *w) { (void)w; return 640; }
void ANativeWindow_release(void *w) { (void)w; }
int ANativeWindow_setBuffersGeometry(void *w, int x, int y, int f) { (void)w;(void)x;(void)y;(void)f; return 0; }
void *ASensorEventQueue_disableSensor(void *q, void *s) { (void)q;(void)s; return q; }
void *ASensorEventQueue_enableSensor(void *q, void *s, int t) { (void)q;(void)s;(void)t; return q; }
int ASensorEventQueue_getEvents(void *q, void *e, int n) { (void)q;(void)e;(void)n; return 0; }
int ASensorEventQueue_hasEvents(void *q) { (void)q; return 0; }
void *ASensorEventQueue_setEventRate(void *q, void *s, long t) { (void)q;(void)s;(void)t; return q; }
void *ASensorManager_createEventQueue(void *m, void *l, int p, void *u) { (void)m;(void)l;(void)p;(void)u; return (void *)1; }
void ASensorManager_destroyEventQueue(void *m, void *q) { (void)m;(void)q; }
void *ASensorManager_getDefaultSensor(void *m, int t) { (void)m;(void)t; return (void *)1; }
void *ASensorManager_getInstance(void) { return (void *)1; }
void *ASensorManager_getSensorList(void *m, void **l) { (void)m; if (l) *l = 0; return 0; }
int ASensor_getMinDelay(void *s) { (void)s; return 1000; }
char *ASensor_getName(void *s) { (void)s; return "sensor"; }
float ASensor_getResolution(void *s) { (void)s; return 1.0f; }
int ASensor_getType(void *s) { (void)s; return 0; }
char *ASensor_getVendor(void *s) { (void)s; return "vendor"; }

/* ---- libunity.so: zlib (inflate) ---- */
typedef unsigned long kv_z_uLong;
typedef unsigned char kv_z_Bytef;
typedef unsigned kv_z_uInt;
typedef struct { kv_z_Bytef *next_in; kv_z_uInt avail_in; unsigned long total_in;
                 kv_z_Bytef *next_out; kv_z_uInt avail_out; unsigned long total_out;
                 void *state; } kv_z_stream;
int inflateInit2_(kv_z_stream *s, int w, const char *v, int n) { (void)s;(void)w;(void)v;(void)n; return 0; }
int inflate(kv_z_stream *s, int f) { (void)s;(void)f; return 1; } /* Z_STREAM_END */
int inflateEnd(kv_z_stream *s) { (void)s; return 0; }

/* ---- libunity.so: EGL ---- */
/* Return valid, distinct handles so Unity's nativeRecreateGfxState doesn't
 * crash on null.  (Real GL would come from libEGL via dlopen on device; these
 * are placeholders that let the boot proceed.) */
void *eglGetDisplay(void *d) { (void)d; return (void *)0xE1000; }
int eglInitialize(void *d, int *maj, int *min) { (void)d; if (maj) *maj=1; if (min) *min=4; return 1; }
int eglTerminate(void *d) { (void)d; return 1; }
int eglChooseConfig(void *d, const int *a, void *c, int n, int *num) { (void)d;(void)a;(void)c;(void)n; if (num) *num=1; return 1; }
int eglGetConfigAttrib(void *d, void *c, int a, int *v) { (void)d;(void)c;(void)a; if (v) *v=0; return 1; }
void *eglCreateContext(void *d, void *c, void *s, const int *a) { (void)d;(void)c;(void)s;(void)a; return (void *)0xE2000; }
void *eglCreateWindowSurface(void *d, void *c, void *w, const int *a) { (void)d;(void)c;(void)w;(void)a; return (void *)0xE3000; }
void *eglCreatePbufferSurface(void *d, void *c, const int *a) { (void)d;(void)c;(void)a; return (void *)0xE3001; }
int eglDestroyContext(void *d, void *c) { (void)d;(void)c; return 1; }
int eglDestroySurface(void *d, void *s) { (void)d;(void)s; return 1; }
int eglMakeCurrent(void *d, void *dr, void *rd, void *c) { (void)d;(void)dr;(void)rd;(void)c; return 1; }
void *eglGetCurrentContext(void) { return (void *)0xE2000; }
void *eglGetCurrentSurface(int r) { (void)r; return (void *)0xE3000; }
int eglSwapBuffers(void *d, void *s) { (void)d;(void)s; return 1; }
int eglSwapInterval(void *d, int i) { (void)d;(void)i; return 1; }
int eglGetError(void) { return 0x3000; } /* EGL_SUCCESS */
int eglSurfaceAttrib(void *d, void *s, int a, int v) { (void)d;(void)s;(void)a;(void)v; return 1; }
char *eglQueryString(void *d, int n) { (void)d;(void)n; return "1.4"; }
int eglQuerySurface(void *d, void *s, int a, int *v) { (void)d;(void)s;(void)a; if (v) *v=0; return 1; }
/* eglGetProcAddress must return a non-null function pointer so Unity can call
 * the resolved GL function (glGetString etc.).  We return a generic stub. */
static void kv_gl_stub(void) {}
static const char *kv_glGetString(unsigned n) {
    switch (n) { case 0x1F02: return "OpenGL ES 2.0"; /* GL_VERSION */
                 case 0x1F01: return "Mali-G31";      /* GL_RENDERER */
                 case 0x1F03: return "GLES";          /* GL_VENDOR */
                 default: return ""; }
}
void *eglGetProcAddress(const char *n) {
    if (n && strcmp(n, "glGetString") == 0) return (void *)kv_glGetString;
    return (void *)kv_gl_stub;   /* non-null: Unity can call it safely */
}

/* ---- libunity.so: POSIX sockets / net ---- */
int socket(int d, int t, int p) { (void)d;(void)t;(void)p; return -1; }
int bind(int fd, const void *a, unsigned n) { (void)fd;(void)a;(void)n; return -1; }
int listen(int fd, int b) { (void)fd;(void)b; return -1; }
int accept(int fd, void *a, void *n) { (void)fd;(void)a;(void)n; return -1; }
int connect(int fd, const void *a, unsigned n) { (void)fd;(void)a;(void)n; return -1; }
long send(int fd, const void *b, unsigned long n, int f) { (void)fd;(void)b;(void)n;(void)f; return -1; }
long recv(int fd, void *b, unsigned long n, int f) { (void)fd;(void)b;(void)n;(void)f; return -1; }
int getsockname(int fd, void *a, void *n) { (void)fd;(void)a;(void)n; return -1; }
int getpeername(int fd, void *a, void *n) { (void)fd;(void)a;(void)n; return -1; }
int getsockopt(int fd, int l, int o, void *v, void *n) { (void)fd;(void)l;(void)o;(void)v;(void)n; return -1; }
int setsockopt(int fd, int l, int o, const void *v, unsigned n) { (void)fd;(void)l;(void)o;(void)v;(void)n; return -1; }
int shutdown(int fd, int h) { (void)fd;(void)h; return -1; }
int getaddrinfo(const char *n, const char *s, const void *h, void **r) { (void)n;(void)s;(void)h; if (r) *r=0; return -1; }
void freeaddrinfo(void *r) { (void)r; }
void *gethostbyname(const char *n) { (void)n; return 0; }
void *gethostbyaddr(const void *a, unsigned l, int t) { (void)a;(void)l;(void)t; return 0; }
unsigned long inet_addr(const char *s) { (void)s; return 0; }
const char *inet_ntop(int f, const void *s, char *b, unsigned n) { (void)f;(void)s;(void)n; if (b) b[0]=0; return b; }
int inet_pton(int f, const char *s, void *d) { (void)f;(void)s;(void)d; return -1; }
int poll(void *f, unsigned long n, int t) { (void)f;(void)n;(void)t; return 0; }
int fcntl(int fd, int c, ...) { (void)fd;(void)c; return -1; }
int flock(int fd, int o) { (void)fd;(void)o; return 0; }
int if_nametoindex(const char *n) { (void)n; return 0; }
int getpriority(int w, int n) { (void)w;(void)n; return 0; }
int setpriority(int w, int n, int p) { (void)w;(void)n;(void)p; return 0; }
int sched_getaffinity(int p, unsigned n, void *m) { (void)p;(void)n; if (m) memset(m, 0, 8); return 0; }
int sched_setaffinity(int p, unsigned n, const void *m) { (void)p;(void)n;(void)m; return 0; }
long prctl(int o, ...) { (void)o; return 0; }
long ptrace(int o, ...) { (void)o; return 0; }
int raise(int s) { (void)s; return 0; }
unsigned long getauxval(unsigned long t) { (void)t; return 0; }
int gettid(void) { return getpid(); }
void *getpwuid(unsigned u) { (void)u; return 0; }
int getpwuid_r(unsigned u, void *p, char *b, unsigned n, void **r) { (void)u;(void)p;(void)b;(void)n; if (r) *r=0; return 0; }
void *gmtime_r(const void *t, void *o) { (void)t;(void)o; return o; }
void *localtime_r(const void *t, void *o) { (void)t;(void)o; return o; }
long truncate(const char *p, long l) { (void)p;(void)l; return -1; }
int utime(const char *p, const void *t) { (void)p;(void)t; return -1; }
int statfs(const char *p, void *s) { (void)p; if (s) memset(s, 0, 120); return -1; }
int fnmatch(const char *p, const char *n, int f) { (void)p;(void)n;(void)f; return 1; }
void *memrchr(const void *s, int c, unsigned long n) {
    const unsigned char *p = s;
    for (unsigned long i = n; i > 0; i--) if (p[i-1]==(unsigned char)c) return (void *)(p+i-1);
    return 0;
}
char *basename(char *p) { return p; }
char *realpath(const char *p, char *r) { (void)p; if (r) r[0]='/'; return r; }
int remove(const char *p) { (void)p; return -1; }

/* ---- more libc stubs ---- */
int _ctype_probe;  /* placeholder */
/* `_ctype_` is now defined as a real data symbol in glibc_shims.c (the
 * bionic char-class table pointer — see comments there).  Do NOT redefine
 * it here as a function stub: that produces a wrong GOT binding and made
 * libunity crash inside nativeRender during string/asset processing. */
int puts(const char *s) { if (s) printf("%s\n", s); return 0; }
float logf(float x) { (void)x; return 0.0f; }
float ldexpf(float a, int e) { (void)a;(void)e; return 0.0f; }
double ldexp(double a, int e) { (void)a;(void)e; return 0.0; }
double exp(double x) { (void)x; return 1.0; }
float expf(float x) { (void)x; return 1.0f; }
float asinf(float x) { (void)x; return 0.0f; }
float modff(float x, float *i) { (void)x; if (i) *i=0; return 0.0f; }
float sqrtf(float x) { (void)x; return 1.0f; }
int pthread_exit(void *r) { (void)r; for (;;) {} return 0; }
int pthread_cond_init(void *c, void *a) { (void)c;(void)a; return 0; }
int pthread_condattr_init(void *a) { (void)a; return 0; }
int pthread_condattr_destroy(void *a) { (void)a; return 0; }
int pthread_condattr_setclock(void *a, int c) { (void)a;(void)c; return 0; }
int pthread_rwlock_init(void *l, void *a) { (void)l;(void)a; return 0; }
int pthread_attr_setdetachstate(void *a, int d) { (void)a;(void)d; return 0; }
int pthread_attr_setstacksize(void *a, unsigned long s) { (void)a;(void)s; return 0; }
char *strerror(int e) { (void)e; return "error"; }
int strcasecmp(const char *a, const char *b) {
    unsigned char x,y; for (;;a++,b++){ x=*a; y=*b; if (x>='A'&&x<='Z')x+=32; if(y>='A'&&y<='Z')y+=32;
        if (x!=y) return (int)x-(int)y; if (x==0) return 0; }
}
char *strcat(char *d, const char *s) { char *r=d; while(*d)d++; while((*d++=*s++)); return r; }
unsigned long strcspn(const char *s, const char *rej) {
    unsigned long n=0; for (const char*p=s;*p;p++){ const char*r=rej; while(*r) if(*r++==*p) return n; n++; } return n;
}
unsigned long strspn(const char *s, const char *acc) {
    unsigned long n=0; for (const char*p=s;*p;p++){ int f=0; const char*r=acc; while(*r) if(*r++==*p){f=1;break;} if(!f)break; n++; } return n;
}
char *strtok_r(char *s, const char *delim, char **save) {
    if (!s) s = *save; if (!s) return 0;
    s += strspn(s, delim); if (!*s) { *save=0; return 0; }
    char *start = s; s += strcspn(s, delim);
    if (*s) { *s = 0; *save = s+1; } else *save = 0;
    return start;
}
unsigned long strnlen(const char *s, unsigned long n) {
    unsigned long i=0; while (i<n && s[i]) i++; return i;
}
/* real clearerr/feof/ferror/fileno/fdopen/fgets/fputs/fscanf/ftell/setbuf/setvbuf
 * live in freestdlib.c */
int vprintf(const char *fmt, void *a) { (void)fmt;(void)a; return 0; }
long lseek64(int fd, long o, int w) { (void)fd;(void)o;(void)w; return 0; }
int lldiv(long a, long b) { return (int)(a/b); }
void __system_property_find(void) {}
int __system_property_read(void *e, char *n, char *v) { (void)e;(void)n; if (v) v[0]=0; return 0; }
void __FD_ISSET_chk(int fd, void *set) { (void)fd;(void)set; }
void _ZTH15gDeferredAction(void) {}

#endif /* KV_USE_GLIBC */

void *host_dlsym(const char *name) {
#ifdef KV_USE_GLIBC
    /* glibc build: the freestanding stub table is compiled out.  Delegate
     * everything to the REAL system: glibc provides libc, and the GPU drivers
     * (loaded by kv_egl_dlopen) provide egl/gl/NDK.  bionic-only symbols that
     * glibc lacks are provided in glibc_shims.c. */
    if (name) return dlsym(RTLD_DEFAULT, name);
    return 0;
#else
    /* exact-name table; keep in sync with what libil2cpp.so imports.  Each
     * pointer is the real freestanding implementation (freestdlib.c or this
     * file). */
    static const struct { const char *n; void *p; } tab[] = {
        {"memcpy", (void *)memcpy}, {"memset", (void *)memset},
        {"memcmp", (void *)memcmp}, {"memmove", (void *)memmove},
        {"memchr", (void *)memchr}, {"strlen", (void *)strlen},
        {"strcmp", (void *)strcmp}, {"strncmp", (void *)strncmp},
        {"strcpy", (void *)strcpy}, {"strncpy", (void *)strncpy},
        {"strchr", (void *)strchr}, {"strrchr", (void *)strrchr},
        {"strstr", (void *)strstr}, {"strdup", (void *)strdup},
        {"strlcpy", (void *)strlcpy},
        {"__memcpy_chk", (void *)__memcpy_chk},
        {"__memmove_chk", (void *)__memmove_chk},
        {"__memset_chk", (void *)__memset_chk},
        {"__strlen_chk", (void *)__strlen_chk},
        {"__vsnprintf_chk", (void *)__vsnprintf_chk},
        {"__FD_SET_chk", (void *)__FD_SET_chk},
        {"__system_property_get", (void *)__system_property_get},
        {"__errno", (void *)__errno},
        {"environ", (void *)&environ},
        {"__sF", (void *)&__sF},
        {"__cxa_atexit", (void *)__cxa_atexit},
        {"__cxa_finalize", (void *)__cxa_finalize},
        {"__stack_chk_fail", (void *)__stack_chk_fail},
        {"abort", (void *)abort},
        {"exit", (void *)exit}, {"_exit", (void *)_exit}, {"_Exit", (void *)_Exit},
        {"malloc", (void *)malloc}, {"free", (void *)free},
        {"calloc", (void *)calloc}, {"realloc", (void *)realloc},
        {"memalign", (void *)memalign}, {"posix_memalign", (void *)posix_memalign},
        {"atoi", (void *)atoi}, {"atol", (void *)atol},
        {"strtol", (void *)strtol}, {"strtoul", (void *)strtoul},
        {"strtoll", (void *)strtoll}, {"strtoull", (void *)strtoull},
        {"fprintf", (void *)fprintf}, {"perror", (void *)perror},
        {"printf", (void *)printf},
        {"time", (void *)time}, {"gettimeofday", (void *)gettimeofday},
        {"clock", (void *)clock}, {"clock_gettime", (void *)clock_gettime},
        {"clock_getres", (void *)clock_getres}, {"nanosleep", (void *)nanosleep},
        {"usleep", (void *)usleep}, {"getpid", (void *)getpid},
        {"getuid", (void *)getuid}, {"geteuid", (void *)geteuid},
        {"getegid", (void *)getegid}, {"sched_yield", (void *)sched_yield},
        {"getpagesize", (void *)getpagesize}, {"sysconf", (void *)sysconf},
        {"isatty", (void *)isatty}, {"getenv", (void *)getenv},
        {"setenv", (void *)setenv}, {"unsetenv", (void *)unsetenv},
        {"gethostname", (void *)gethostname}, {"getcwd", (void *)getcwd},
        {"pthread_mutex_init", (void *)pthread_mutex_init},
        {"pthread_mutex_destroy", (void *)pthread_mutex_destroy},
        {"pthread_mutex_lock", (void *)pthread_mutex_lock},
        {"pthread_mutex_unlock", (void *)pthread_mutex_unlock},
        {"pthread_mutex_trylock", (void *)pthread_mutex_trylock},
        {"pthread_mutexattr_init", (void *)pthread_mutexattr_init},
        {"pthread_mutexattr_destroy", (void *)pthread_mutexattr_destroy},
        {"pthread_mutexattr_settype", (void *)pthread_mutexattr_settype},
        {"pthread_cond_destroy", (void *)pthread_cond_destroy},
        {"pthread_cond_broadcast", (void *)pthread_cond_broadcast},
        {"pthread_cond_signal", (void *)pthread_cond_signal},
        {"pthread_cond_wait", (void *)pthread_cond_wait},
        {"pthread_cond_timedwait", (void *)pthread_cond_timedwait},
        {"pthread_create", (void *)pthread_create},
        {"pthread_join", (void *)pthread_join},
        {"pthread_detach", (void *)pthread_detach},
        {"pthread_self", (void *)pthread_self},
        {"pthread_equal", (void *)pthread_equal},
        {"pthread_once", (void *)pthread_once},
        {"pthread_key_create", (void *)pthread_key_create},
        {"pthread_key_delete", (void *)pthread_key_delete},
        {"pthread_getspecific", (void *)pthread_getspecific},
        {"pthread_setspecific", (void *)pthread_setspecific},
        {"pthread_sigmask", (void *)pthread_sigmask},
        {"pthread_kill", (void *)pthread_kill},
        {"pthread_atfork", (void *)pthread_atfork},
        {"pthread_attr_init", (void *)pthread_attr_init},
        {"pthread_attr_destroy", (void *)pthread_attr_destroy},
        {"pthread_attr_getstack", (void *)pthread_attr_getstack},
        {"pthread_getattr_np", (void *)pthread_getattr_np},
        {"pthread_setname_np", (void *)pthread_setname_np},
        {"pthread_rwlock_rdlock", (void *)pthread_rwlock_rdlock},
        {"pthread_rwlock_wrlock", (void *)pthread_rwlock_wrlock},
        {"pthread_rwlock_unlock", (void *)pthread_rwlock_unlock},
        {"sem_init", (void *)sem_init}, {"sem_post", (void *)sem_post},
        {"sem_wait", (void *)sem_wait}, {"sem_timedwait", (void *)sem_timedwait},
        {"sem_getvalue", (void *)sem_getvalue}, {"sem_destroy", (void *)sem_destroy},
        {"cos", (void *)cos}, {"cosf", (void *)cosf},
        {"sin", (void *)sin}, {"sinf", (void *)sinf},
        {"tan", (void *)tan}, {"tanf", (void *)tanf},
        {"acos", (void *)acos}, {"acosf", (void *)acosf},
        {"asin", (void *)asin}, {"atan", (void *)atan},
        {"atanf", (void *)atanf}, {"atan2", (void *)atan2},
        {"atan2f", (void *)atan2f}, {"log", (void *)log},
        {"log10", (void *)log10}, {"log10f", (void *)log10f},
        {"log2", (void *)log2}, {"logb", (void *)logb},
        {"exp2f", (void *)exp2f}, {"pow", (void *)pow},
        {"powf", (void *)powf}, {"sqrt", (void *)sqrt},
        {"fmod", (void *)fmod}, {"fmodf", (void *)fmodf},
        {"hypot", (void *)hypot}, {"modf", (void *)modf},
        {"scalbn", (void *)scalbn}, {"difftime", (void *)difftime},
        {"sincosf", (void *)sincosf},
        {"android_set_abort_message", (void *)android_set_abort_message},
        {"newlocale", (void *)newlocale}, {"freelocale", (void *)freelocale},
        {"uselocale", (void *)uselocale}, {"localeconv", (void *)localeconv},
        {"setlocale", (void *)setlocale},
        {"isdigit_l", (void *)isdigit_l}, {"isxdigit_l", (void *)isxdigit_l},
        {"islower_l", (void *)islower_l}, {"isupper_l", (void *)isupper_l},
        {"iswalpha_l", (void *)iswalpha_l}, {"iswblank_l", (void *)iswblank_l},
        {"iswcntrl_l", (void *)iswcntrl_l}, {"iswdigit_l", (void *)iswdigit_l},
        {"iswlower_l", (void *)iswlower_l}, {"iswprint_l", (void *)iswprint_l},
        {"iswpunct_l", (void *)iswpunct_l}, {"iswspace_l", (void *)iswspace_l},
        {"iswupper_l", (void *)iswupper_l}, {"iswxdigit_l", (void *)iswxdigit_l},
        {"towlower", (void *)towlower}, {"towupper", (void *)towupper},
        {"towlower_l", (void *)towlower_l}, {"towupper_l", (void *)towupper_l},
        {"tolower_l", (void *)tolower_l}, {"toupper_l", (void *)toupper_l},
        {"btowc", (void *)btowc}, {"wctob", (void *)wctob},
        {"wcslen", (void *)wcslen}, {"wmemchr", (void *)wmemchr},
        {"wmemcmp", (void *)wmemcmp}, {"wmemcpy", (void *)wmemcpy},
        {"wmemmove", (void *)wmemmove}, {"wmemset", (void *)wmemset},
        {"mbrlen", (void *)mbrlen}, {"mbrtowc", (void *)mbrtowc},
        {"mbtowc", (void *)mbtowc}, {"wcrtomb", (void *)wcrtomb},
        {"mbsrtowcs", (void *)mbsrtowcs}, {"mbsnrtowcs", (void *)mbsnrtowcs},
        {"wcsnrtombs", (void *)wcsnrtombs},
        {"__ctype_get_mb_cur_max", (void *)__ctype_get_mb_cur_max},
        {"div", (void *)div}, {"bsearch", (void *)bsearch},
        {"qsort", (void *)qsort}, {"lrand48", (void *)lrand48},
        {"srand48", (void *)srand48}, {"gmtime", (void *)gmtime},
        {"localtime", (void *)localtime}, {"mktime", (void *)mktime},
        {"strftime", (void *)strftime}, {"strftime_l", (void *)strftime_l},
        {"strcoll_l", (void *)strcoll_l}, {"strxfrm_l", (void *)strxfrm_l},
        {"wcscoll_l", (void *)wcscoll_l}, {"wcsxfrm_l", (void *)wcsxfrm_l},
        {"fopen", (void *)fopen}, {"fclose", (void *)fclose},
        {"fflush", (void *)fflush}, {"fread", (void *)fread},
        {"fwrite", (void *)fwrite}, {"fputc", (void *)fputc},
        {"fseek", (void *)fseek}, {"fseeko", (void *)fseeko},
        {"ftello", (void *)ftello}, {"access", (void *)access},
        {"unlink", (void *)unlink}, {"rename", (void *)rename},
        {"chmod", (void *)chmod}, {"fchmod", (void *)fchmod},
        {"mkdir", (void *)mkdir}, {"rmdir", (void *)rmdir},
        {"link", (void *)link}, {"symlink", (void *)symlink},
        {"readlink", (void *)readlink}, {"lseek", (void *)lseek},
        {"dup", (void *)dup}, {"ftruncate", (void *)ftruncate},
        {"futimens", (void *)futimens}, {"utimes", (void *)utimes},
        {"ioctl", (void *)ioctl}, {"pipe", (void *)pipe},
        {"select", (void *)select}, {"sendfile", (void *)sendfile},
        {"opendir", (void *)opendir}, {"readdir", (void *)readdir},
        {"closedir", (void *)closedir}, {"stat", (void *)stat},
        {"lstat", (void *)lstat},
        {"sigaction", (void *)sigaction}, {"signal", (void *)signal},
        {"sigaltstack", (void *)sigaltstack},
        {"sigemptyset", (void *)sigemptyset}, {"sigfillset", (void *)sigfillset},
        {"sigaddset", (void *)sigaddset}, {"sigdelset", (void *)sigdelset},
        {"sigsuspend", (void *)sigsuspend}, {"madvise", (void *)madvise},
        {"munmap", (void *)munmap}, {"dladdr", (void *)dladdr},
        {"dlclose", (void *)dlclose}, {"dlerror", (void *)dlerror},
        {"dl_iterate_phdr", (void *)dl_iterate_phdr},
        {"syscall", (void *)syscall}, {"setjmp", (void *)setjmp},
        {"longjmp", (void *)longjmp}, {"strerror_r", (void *)strerror_r},
        {"snprintf", (void *)snprintf}, {"sprintf", (void *)sprintf},
        {"vsnprintf", (void *)vsnprintf}, {"swprintf", (void *)swprintf},
        {"vfprintf", (void *)vfprintf}, {"vasprintf", (void *)vasprintf},
        {"sscanf", (void *)sscanf}, {"vsscanf", (void *)vsscanf},
        {"openlog", (void *)openlog}, {"syslog", (void *)syslog},
        {"closelog", (void *)closelog},
        {"__android_log_print", (void *)__android_log_print},
        {"uname", (void *)uname},
        {"strtod", (void *)strtod}, {"strtof", (void *)strtof},
        {"strtold", (void *)strtold}, {"strtold_l", (void *)strtold_l},
        {"strtoll_l", (void *)strtoll_l}, {"strtoull_l", (void *)strtoull_l},
        {"wcstod", (void *)wcstod}, {"wcstof", (void *)wcstof},
        {"wcstold", (void *)wcstold}, {"wcstol", (void *)wcstol},
        {"wcstoll", (void *)wcstoll}, {"wcstoul", (void *)wcstoul},
        {"wcstoull", (void *)wcstoull},
        {"dlsym", (void *)dlsym}, {"dlopen", (void *)dlopen},
        {"read", (void *)read}, {"write", (void *)write},
        {"close", (void *)close}, {"open", (void *)open},
        {"mmap", (void *)mmap}, {"munmap", (void *)munmap},
        {"fstat", (void *)fstat},
        /* libunity.so: NDK native */
        {"ALooper_acquire", (void *)ALooper_acquire},
        {"ALooper_forThread", (void *)ALooper_forThread},
        {"ALooper_pollOnce", (void *)ALooper_pollOnce},
        {"ALooper_prepare", (void *)ALooper_prepare},
        {"ALooper_release", (void *)ALooper_release},
        {"ALooper_wake", (void *)ALooper_wake},
        {"ANativeWindow_acquire", (void *)ANativeWindow_acquire},
        {"ANativeWindow_fromSurface", (void *)ANativeWindow_fromSurface},
        {"ANativeWindow_getHeight", (void *)ANativeWindow_getHeight},
        {"ANativeWindow_getWidth", (void *)ANativeWindow_getWidth},
        {"ANativeWindow_release", (void *)ANativeWindow_release},
        {"ANativeWindow_setBuffersGeometry", (void *)ANativeWindow_setBuffersGeometry},
        {"ASensorEventQueue_disableSensor", (void *)ASensorEventQueue_disableSensor},
        {"ASensorEventQueue_enableSensor", (void *)ASensorEventQueue_enableSensor},
        {"ASensorEventQueue_getEvents", (void *)ASensorEventQueue_getEvents},
        {"ASensorEventQueue_hasEvents", (void *)ASensorEventQueue_hasEvents},
        {"ASensorEventQueue_setEventRate", (void *)ASensorEventQueue_setEventRate},
        {"ASensorManager_createEventQueue", (void *)ASensorManager_createEventQueue},
        {"ASensorManager_destroyEventQueue", (void *)ASensorManager_destroyEventQueue},
        {"ASensorManager_getDefaultSensor", (void *)ASensorManager_getDefaultSensor},
        {"ASensorManager_getInstance", (void *)ASensorManager_getInstance},
        {"ASensorManager_getSensorList", (void *)ASensorManager_getSensorList},
        {"ASensor_getMinDelay", (void *)ASensor_getMinDelay},
        {"ASensor_getName", (void *)ASensor_getName},
        {"ASensor_getResolution", (void *)ASensor_getResolution},
        {"ASensor_getType", (void *)ASensor_getType},
        {"ASensor_getVendor", (void *)ASensor_getVendor},
        /* libunity.so: zlib */
        {"inflateInit2_", (void *)inflateInit2_},
        {"inflate", (void *)inflate},
        {"inflateEnd", (void *)inflateEnd},
        /* libunity.so: EGL */
        {"eglGetDisplay", (void *)eglGetDisplay},
        {"eglInitialize", (void *)eglInitialize},
        {"eglTerminate", (void *)eglTerminate},
        {"eglChooseConfig", (void *)eglChooseConfig},
        {"eglGetConfigAttrib", (void *)eglGetConfigAttrib},
        {"eglCreateContext", (void *)eglCreateContext},
        {"eglCreateWindowSurface", (void *)eglCreateWindowSurface},
        {"eglCreatePbufferSurface", (void *)eglCreatePbufferSurface},
        {"eglDestroyContext", (void *)eglDestroyContext},
        {"eglDestroySurface", (void *)eglDestroySurface},
        {"eglMakeCurrent", (void *)eglMakeCurrent},
        {"eglGetCurrentContext", (void *)eglGetCurrentContext},
        {"eglGetCurrentSurface", (void *)eglGetCurrentSurface},
        {"eglSwapBuffers", (void *)eglSwapBuffers},
        {"eglSwapInterval", (void *)eglSwapInterval},
        {"eglGetError", (void *)eglGetError},
        {"eglSurfaceAttrib", (void *)eglSurfaceAttrib},
        {"eglQueryString", (void *)eglQueryString},
        {"eglQuerySurface", (void *)eglQuerySurface},
        {"eglGetProcAddress", (void *)eglGetProcAddress},
        /* libunity.so: POSIX sockets / net */
        {"socket", (void *)socket}, {"bind", (void *)bind},
        {"listen", (void *)listen}, {"accept", (void *)accept},
        {"connect", (void *)connect}, {"send", (void *)send},
        {"recv", (void *)recv}, {"getsockname", (void *)getsockname},
        {"getpeername", (void *)getpeername}, {"getsockopt", (void *)getsockopt},
        {"setsockopt", (void *)setsockopt}, {"shutdown", (void *)shutdown},
        {"getaddrinfo", (void *)getaddrinfo}, {"freeaddrinfo", (void *)freeaddrinfo},
        {"gethostbyname", (void *)gethostbyname}, {"gethostbyaddr", (void *)gethostbyaddr},
        {"inet_addr", (void *)inet_addr}, {"inet_ntop", (void *)inet_ntop},
        {"inet_pton", (void *)inet_pton}, {"poll", (void *)poll},
        {"fcntl", (void *)fcntl}, {"flock", (void *)flock},
        {"if_nametoindex", (void *)if_nametoindex},
        {"getpriority", (void *)getpriority}, {"setpriority", (void *)setpriority},
        {"sched_getaffinity", (void *)sched_getaffinity},
        {"sched_setaffinity", (void *)sched_setaffinity},
        {"prctl", (void *)prctl}, {"ptrace", (void *)ptrace},
        {"raise", (void *)raise}, {"getauxval", (void *)getauxval},
        {"gettid", (void *)gettid}, {"getpwuid", (void *)getpwuid},
        {"getpwuid_r", (void *)getpwuid_r}, {"gmtime_r", (void *)gmtime_r},
        {"localtime_r", (void *)localtime_r}, {"truncate", (void *)truncate},
        {"utime", (void *)utime}, {"statfs", (void *)statfs},
        {"fnmatch", (void *)fnmatch}, {"memrchr", (void *)memrchr},
        {"basename", (void *)basename}, {"realpath", (void *)realpath},
        {"remove", (void *)remove},
        /* libunity.so: more libc */
        {"puts", (void *)puts}, {"logf", (void *)logf},
        {"ldexpf", (void *)ldexpf}, {"ldexp", (void *)ldexp},
        {"exp", (void *)exp}, {"expf", (void *)expf},
        {"asinf", (void *)asinf}, {"modff", (void *)modff},
        {"sqrtf", (void *)sqrtf}, {"pthread_exit", (void *)pthread_exit},
        {"pthread_cond_init", (void *)pthread_cond_init},
        {"pthread_condattr_init", (void *)pthread_condattr_init},
        {"pthread_condattr_destroy", (void *)pthread_condattr_destroy},
        {"pthread_condattr_setclock", (void *)pthread_condattr_setclock},
        {"pthread_rwlock_init", (void *)pthread_rwlock_init},
        {"pthread_attr_setdetachstate", (void *)pthread_attr_setdetachstate},
        {"pthread_attr_setstacksize", (void *)pthread_attr_setstacksize},
        {"strerror", (void *)strerror}, {"strcasecmp", (void *)strcasecmp},
        {"strcat", (void *)strcat}, {"strcspn", (void *)strcspn},
        {"strspn", (void *)strspn}, {"strtok_r", (void *)strtok_r},
        {"strnlen", (void *)strnlen}, {"clearerr", (void *)clearerr},
        {"feof", (void *)feof}, {"ferror", (void *)ferror},
        {"fileno", (void *)fileno}, {"fdopen", (void *)fdopen},
        {"fgets", (void *)fgets}, {"fputs", (void *)fputs},
        {"fscanf", (void *)fscanf}, {"ftell", (void *)ftell},
        {"setbuf", (void *)setbuf}, {"setvbuf", (void *)setvbuf},
        {"vprintf", (void *)vprintf}, {"lseek64", (void *)lseek64},
        {"lldiv", (void *)lldiv},
        {"__system_property_find", (void *)__system_property_find},
        {"__system_property_read", (void *)__system_property_read},
        {"__FD_ISSET_chk", (void *)__FD_ISSET_chk},
        {"_ZTH15gDeferredAction", (void *)_ZTH15gDeferredAction},
        {"_ctype_", (void *)_ctype_},
        {"mprotect", (void *)mprotect},
        {"__android_log_vprint", (void *)__android_log_vprint},
        {"__android_log_write", (void *)__android_log_write},
        {0, 0},
    };
    for (int i = 0; tab[i].n; i++)
        if (!strcmp(tab[i].n, name)) return tab[i].p;
    return 0;
#endif /* KV_USE_GLIBC */
}