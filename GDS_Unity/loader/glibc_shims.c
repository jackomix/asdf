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
int __FD_ISSET_chk(int fd, void *set) { (void)fd; (void)set; return 0; }
void __FD_CLR_chk(int fd, void *set) { (void)fd; (void)set; }

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
    /* Must be async-signal-safe: no printf/malloc.  Use write+sprintf only.
     *
     * NOTE on aarch64 glibc ucontext layout (verified empirically):
     *   uc_mcontext @ offsetof(ucontext_t, uc_mcontext) = 0xB8 on aarch64 glibc.
     *   mcontext_t = { unsigned long fault_address; unsigned long regs[31];
     *                  unsigned long sp; unsigned long pc; unsigned long pstate; }
     * So:
     *   fault_address @ mcontext + 0    = ucontext + 0xB8 + 0    = 0xB8
     *   regs[i]       @ mcontext + 8 + i*8  = ucontext + 0xC0 + i*8
     *   sp            @ mcontext + 8 + 31*8 = ucontext + 0x1B0
     *   pc            @ mcontext + 8 + 32*8 = ucontext + 0x1C0
     *   x29 (fp)      = regs[29] = ucontext + 0xC0 + 29*8 = 0x1A8
     *   x30 (lr)      = regs[30] = ucontext + 0xC0 + 30*8 = 0x1B8
     *
     * Older code read x30 at 0x168 (wrong: that's regs[22]) and pc at 0x1C0
     * (still correct: pc is @ ucontext + 0x1C0).  Fixed below. */
    unsigned char *u = (unsigned char *)ucontext;
    unsigned long far = 0, pc = 0, sp = 0, x29 = 0, x30 = 0;
    if (u) {
        for (int i = 0; i < 8; i++) far |= ((unsigned long)u[0xB8 + i]) << (8 * i);
        for (int i = 0; i < 8; i++) pc  |= ((unsigned long)u[0x1C0 + i]) << (8 * i);
        for (int i = 0; i < 8; i++) sp  |= ((unsigned long)u[0x1B0 + i]) << (8 * i);
        for (int i = 0; i < 8; i++) x29 |= ((unsigned long)u[0x1A8 + i]) << (8 * i);
        for (int i = 0; i < 8; i++) x30 |= ((unsigned long)u[0x1B8 + i]) << (8 * i);
    }
    /* si_addr in siginfo (overlaps the FAR for SIGSEGV) - useful cross-check.
     * NOTE: si_addr is a glibc macro - shadow it with our own name. */
    unsigned char *p = (unsigned char *)info;
    unsigned long k_si_addr = 0;
    if (p) for (int i = 0; i < 8; i++) k_si_addr |= ((unsigned long)p[16 + i]) << (8 * i);

    char buf[640]; int n = 0;
    n += sprintf(buf + n, "[loader] === CRASH sig=%d ===\n", sig);
    n += sprintf(buf + n, "[loader]   si_addr=0x%lx FAR=0x%lx\n", k_si_addr, far);
    n += sprintf(buf + n, "[loader]   pc=0x%lx sp=0x%lx fp(x29)=0x%lx lr(x30)=0x%lx\n",
                 pc, sp, x29, x30);

    /* Find the owner module for `pc` so we can compute fn-relative offsets,
     * which is what we actually need to map the crash to a function.  Walk our
     * own loaded modules by scanning /proc/self/maps - cheap and good enough. */
    int mapsfd = open("/proc/self/maps", O_RDONLY);
    if (mapsfd >= 0) {
        char mbuf[8192]; ssize_t mn = 0, r;
        while ((r = read(mapsfd, mbuf + mn, sizeof mbuf - 1 - mn)) > 0) {
            mn += r; if (mn >= (ssize_t)sizeof mbuf - 1) break;
        }
        close(mapsfd);
        mbuf[mn] = 0;
        unsigned long pchi = pc >> 16, pclohi = (pc >> 12);
        char *line = mbuf;
        while (line && *line) {
            char *eol = line; while (*eol && *eol != '\n') eol++;
            char saved = *eol; *eol = 0;
            /* format: start-end perms offset dev inode pathname */
            unsigned long st = 0, en = 0;
            const char *q = line;
            while (*q >= '0' && *q <= 'f') { st = st * 16 + (*q <= '9' ? *q - '0' : (*q & 7) + 9); q++; }
            if (*q == '-') { q++; while (*q >= '0' && *q <= 'f') { en = en * 16 + (*q <= '9' ? *q - '0' : (*q & 7) + 9); q++; } }
            if (pc >= st && pc < en) {
                const char *path = q;
                /* skip to path field */
                int spc = 0; while (*path && spc < 5) { if (*path == ' ') { while (*path == ' ') path++; spc++; } else path++; }
                n += sprintf(buf + n, "[loader]   pc in [%#lx..%#lx) %s  (pc-offset=0x%lx)\n",
                             st, en, path, pc - st);
                break;
            }
            *eol = saved;
            line = (*eol == '\n') ? eol + 1 : 0;
        }
    }

    /* Walk the frame records (x29 chain) - up to 32 frames.
     * Each frame record is { fp, lr } at x29; lr is the return address. */
    n += sprintf(buf + n, "[loader] backtrace (x29 chain):\n");
    unsigned long fp = x29;
    int frame = 0;
    while (fp && frame < 32) {
        unsigned long lr = 0;
        /* lr is at fp+8.  Read carefully - might fault if the chain is bogus. */
        int ok = 1;
        /* Tiny safe read: use a probe via /proc/self/mem so a bad fp doesn't
         * crash the crash handler. */
        int memfd = open("/proc/self/mem", O_RDONLY);
        if (memfd >= 0) {
            if (lseek(memfd, (off_t)(fp + 8), SEEK_SET) == (off_t)(fp + 8) &&
                read(memfd, &lr, 8) == 8) {
                /* ok */
            } else { ok = 0; }
            close(memfd);
        } else { ok = 0; }
        if (!ok) break;
        n += sprintf(buf + n, "[loader]   #%d lr=0x%lx (fp=0x%lx)\n", frame, lr, fp);
        if (lr == 0) break;
        /* next fp = *fp */
        unsigned long nextfp = 0;
        int memfd2 = open("/proc/self/mem", O_RDONLY);
        if (memfd2 >= 0) {
            if (lseek(memfd2, (off_t)fp, SEEK_SET) == (off_t)fp &&
                read(memfd2, &nextfp, 8) != 8) { nextfp = 0; }
            close(memfd2);
        }
        if (nextfp <= fp) break;   /* sanity: chain must grow up */
        fp = nextfp;
        frame++;
    }

    (void)write(2, buf, n);
    _exit(139);
}

static char kv_altstack_buf[SIGSTKSZ * 4] __attribute__((aligned(16)));
void kv_install_crash_handler(void) {
    /* Without an alternate signal stack, a segfault that fires while the
     * stack pointer is near the stack guard / on a tiny stack cannot deliver
     * the signal: the kernel tries to push the frame, faults again, and
     * kills with default SIGSEGV (exit 139) WITHOUT calling the handler.
     * SA_ONSTACK without an alt stack set has the same effect.  So we MUST
     * call sigaltstack() first.  This is almost certainly why 0.28 exited
     * 139 with no crash line in the log. */
    stack_t ss;
    ss.ss_sp = kv_altstack_buf;
    ss.ss_size = sizeof kv_altstack_buf;
    ss.ss_flags = 0;
    if (sigaltstack(&ss, NULL) != 0) {
        /* Fall back: skip SA_ONSTACK so the handler runs on the current stack. */
    }
    struct sigaction sa; memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = kv_sighandler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESETHAND;
    /* Don't block other signals during the handler - we want to die. */
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, 0);
    sigaction(SIGBUS, &sa, 0);
    sigaction(SIGILL, &sa, 0);
    sigaction(SIGFPE, &sa, 0);
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
    /* On ArkOS the Mali G31 driver is at libMali.so (gbm variant) with
     * libEGL.so / libGLESv2.so / libgbm.so -> libMali.so.  The .so.1 names can
     * point at a standalone non-Mali EGL, so load the Mali-named libs
     * RTLD_GLOBAL first; dlopen-ing libMali puts egl and gl entry points in
     * the global namespace for both our shim and SDL's eglGetProcAddress.
     * libz too: libunity.so needs inflate/inflateInit2_/inflateEnd. */
    void *m = dlopen("libmali.so", RTLD_NOW | RTLD_GLOBAL);
    if (!m) m = dlopen("libEGL.so", RTLD_NOW | RTLD_GLOBAL);
    if (!m) m = dlopen("libEGL.so.1", RTLD_NOW | RTLD_GLOBAL);
    void *g = dlopen("libGLESv2.so", RTLD_NOW | RTLD_GLOBAL);
    if (!g) g = dlopen("libGLESv2.so.2", RTLD_NOW | RTLD_GLOBAL);
    void *z = dlopen("libz.so.1", RTLD_NOW | RTLD_GLOBAL);
    if (!z) z = dlopen("libz.so", RTLD_NOW | RTLD_GLOBAL);
    void *s = dlopen("libSDL2.so.0", RTLD_NOW | RTLD_GLOBAL);
    if (!s) s = dlopen("libSDL2-2.0.so.0", RTLD_NOW | RTLD_GLOBAL);
    if (!m) printf("[egl] WARNING: could not dlopen libmali.so/libEGL.so\n");
}
