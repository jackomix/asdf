/* glibc_shims.c - bionic-only + loader-helper shims for the glibc-linked build.
 *
 * The freestanding loader (freestdlib.c) defines its own libc, but in the
 * glibc build glibc provides malloc/fopen/printf/etc.  The only things glibc
 * does NOT provide that the loader's host_syms.c / jni_shim.c reference are the
 * bionic extras (strlcpy, _chk FORTIFY, __errno, __sF) and the loader's crash/
 * log helpers (kv_install_crash_handler, kv_log_open).  Those live here.
 */
#include <stdarg.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <stdio.h>
#include <fcntl.h>
#include <signal.h>
#include <errno.h>

/* bionic strlcpy: glibc may not expose it; provide it here. */
size_t strlcpy(char *d, const char *s, size_t n) {
    size_t sl = strlen(s);
    if (n) { size_t c = sl < n - 1 ? sl : n - 1; memcpy(d, s, c); d[c] = 0; }
    return sl;
}

/* bionic strlcpy: glibc has strlcpy as a GNU extension in <string.h>, but to be
 * safe provide it unconditionally (the loader's host_syms exports it). */
#if 0
size_t strlcpy(char *d, const char *s, size_t n) {
    size_t sl = strlen(s);
    if (n) { size_t c = sl < n - 1 ? sl : n - 1; memcpy(d, s, c); d[c] = 0; }
    return sl;
}
#endif

/* FORTIFY _chk wrappers (glibc has real ones; these let us override to the
 * non-checked versions so the .so's calls don't abort). */
void *__memcpy_chk(void *d, const void *s, size_t n, size_t dlen) { (void)dlen; return memcpy(d, s, n); }
void *__memmove_chk(void *d, const void *s, size_t n, size_t dlen) { (void)dlen; return memmove(d, s, n); }
void *__memset_chk(void *d, int c, size_t n, size_t dlen) { (void)dlen; return memset(d, c, n); }
size_t __strlen_chk(const char *s, size_t slen) { (void)slen; return strlen(s); }
void __FD_SET_chk(int fd, void *set) { (void)fd; (void)set; }

/* bionic __errno: glibc provides errno, but the .so calls *__errno().  Return
 * a pointer to the real errno. */
int *__errno(void) { extern int errno; return &errno; }

/* __sF: glibc has stdout/stderr; give a minimal table so early stderr writes
 * don't crash.  (glibc code uses its own; this is for the .so's direct refs.) */
static char kv_sF_buf[3];
void *__sF = kv_sF_buf;

/* --- loader crash/log helpers (were in freestdlib.c) --- */
static int kv_logfd = -1;
static void kv_outc(int c) { write(1, &c, 1); if (kv_logfd >= 0) write(kv_logfd, &c, 1); }
static void kv_outstr(const char *s) { size_t n = strlen(s); write(1, s, n); if (kv_logfd >= 0) write(kv_logfd, s, n); }
int kv_log_open(const char *path) {
    if (path) {
        /* O_TRUNC: fresh log every run.  Without it, loader.log kept the
         * previous deployment's content forever (the stale-0.5.0 bug).  We
         * also dup2 it onto stdout+stderr so every printf/crash line lands in
         * the fresh loader.log, not just in the launcher's port_launch.log. */
        kv_logfd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (kv_logfd >= 0) {
            dup2(kv_logfd, 1);   /* stdout -> loader.log */
            dup2(kv_logfd, 2);   /* stderr -> loader.log */
        }
    }
    return 0;
}
static void kv_sighandler(int sig, void *info, void *ucontext) {
    (void)ucontext;
    unsigned char *p = (unsigned char *)info;
    unsigned long addr = 0;
    for (int i = 0; i < 8; i++) addr |= ((unsigned long)p[16 + i]) << (8 * i);
    /* glibc aarch64: regs[i] @ mcontext+8+i*8; pc @ mcontext+8+31*8+8 = 0x1C0
     * (mcontext @ 0xB8).  x30=regs[30] @ 0xB8+8+240=0x168. */
    unsigned char *u = (unsigned char *)ucontext;
    unsigned long pc = 0, x30 = 0, x0 = 0;
    if (u) {
        for (int i = 0; i < 8; i++) pc |= ((unsigned long)u[0x1C0 + i]) << (8 * i);
        for (int i = 0; i < 8; i++) x30 |= ((unsigned long)u[0x168 + i]) << (8 * i);
        for (int i = 0; i < 8; i++) x0  |= ((unsigned long)u[0xC0 + i]) << (8 * i);
    }
    char buf[128]; int n = 0;
    n += sprintf(buf + n, "[loader] CRASH sig=%d addr=0x%lx pc=0x%lx x0=0x%lx x30=0x%lx\n",
                 sig, addr, pc, x0, x30);
    write(2, buf, n);
    _exit(139);
}
void kv_install_crash_handler(void) {
    struct sigaction sa; memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = kv_sighandler; sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigaction(SIGSEGV, &sa, 0); sigaction(SIGBUS, &sa, 0); sigaction(SIGILL, &sa, 0);
}

/* --- bionic-only symbols that glibc lacks but the .so imports.
 * Exported (non-static) so dlsym(RTLD_DEFAULT, ...) finds them. --- */
int __system_property_get(const char *name, char *value) { (void)name; if (value) value[0]=0; return 0; }
void __system_property_find(void) {}
int __system_property_read(void *e, char *n, char *v) { (void)e;(void)n; if (v) v[0]=0; return 0; }
int __android_log_print(int prio, const char *tag, const char *fmt, ...) { (void)prio;(void)tag;(void)fmt; return 0; }
int __android_log_vprint(int prio, const char *tag, const char *fmt, va_list ap) { (void)prio;(void)tag;(void)fmt;(void)ap; return 0; }
int __android_log_write(int prio, const char *tag, const char *msg) { (void)prio;(void)tag;(void)msg; return 0; }
void _ctype_(void) {}
void _ZTH15gDeferredAction(void) {}

/* --- load the real GPU drivers so Unity's dlsym(RTLD_DEFAULT) finds real GL.
 * The R36S/ArkOS has libEGL.so / libGLESv2.so (Mali).  dlopen them RTLD_GLOBAL
 * so Unity resolves eglGetProcAddress/glGetString to the real driver. --- */
#include <dlfcn.h>
void kv_egl_dlopen(void) {
    void *g = dlopen("libGLESv2.so.2", RTLD_NOW | RTLD_GLOBAL);
    if (!g) dlopen("libGLESv2.so", RTLD_NOW | RTLD_GLOBAL);
    void *e = dlopen("libEGL.so.1", RTLD_NOW | RTLD_GLOBAL);
    if (!e) dlopen("libEGL.so", RTLD_NOW | RTLD_GLOBAL);
    void *s = dlopen("libSDL2.so.0", RTLD_NOW | RTLD_GLOBAL);
    if (!s) dlopen("libSDL2-2.0.so.0", RTLD_NOW | RTLD_GLOBAL);
}
