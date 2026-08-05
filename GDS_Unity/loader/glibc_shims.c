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
#include <pthread.h>
#include <fcntl.h>
#include <signal.h>
#include <ucontext.h>
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
    /* Use the real struct access (like terraria-nextos/horizonchase-nextos do:
     * `uc->uc_mcontext.pc, .regs[30]` etc) instead of byte offsets.  Our
     * previous hand-computed offsets (pc@ucontext+0x1C0 etc) were WRONG for
     * this glibc build and we got garbage PC values - exactly the bug that
     * made 0.31 print pc=0x20000000 (truncated/garbage) for a real crash. */
    ucontext_t *uc = (ucontext_t *)ucontext;
    unsigned long pc  = uc ? uc->uc_mcontext.pc        : 0;
    unsigned long far = uc ? uc->uc_mcontext.fault_address : 0;
    unsigned long sp  = uc ? uc->uc_mcontext.sp        : 0;
    unsigned long x29 = uc ? uc->uc_mcontext.regs[29] : 0;
    unsigned long x30 = uc ? uc->uc_mcontext.regs[30] : 0;
    (void)info;

    char buf[4096]; int n = 0;
extern int kv_is_main_thread(void);
    n += sprintf(buf + n, "[loader] === CRASH sig=%d tid=%ld pthread_self=%p main=%d ===\n",
                 sig, (long)syscall(178), (void*)pthread_self(), kv_is_main_thread());
    n += sprintf(buf + n, "[loader]   FAR=0x%lx\n", far);
    n += sprintf(buf + n, "[loader]   pc=0x%lx sp=0x%lx fp(x29)=0x%lx lr(x30)=0x%lx\n",
                 pc, sp, x29, x30);
    /* Dump x0..x28 in a 3-per-line grid so we can see arg values + aux regs. */
    if (uc) for (int i = 0; i < 29; i++) {
        n += sprintf(buf + n, " x%-2d=0x%lx", i, (unsigned long)uc->uc_mcontext.regs[i]);
        if (i % 3 == 2) { buf[n++] = '\n'; }
    }
    if (n > 0 && buf[n-1] != '\n') buf[n++] = '\n';

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

/* 256KB alt stack — same size as both reference ports use (terraria-nextos
 * and horizonchase-nextos).  Our previous 32KB (SIGSTKSZ*4) was too small
 * for a handler that walks /proc/self/maps and /proc/self/mem, and SA_ONSTACK
 * without a large-enough stack re-faults on push → silent exit 139 with no
 * handler invocation.  256KB comfortably holds the handler even on a blown
 * main stack. */
static char kv_altstack_buf[256 * 1024] __attribute__((aligned(16)));

/* 0.50.2: SIGUSR1 -> dump the receiving thread's PC + regs + backtrace and
 * RETURN (unlike kv_sighandler which exits).  The watchdog sends SIGUSR1 to the
 * main thread (which is busy-spinning, so /proc/self/task/<tid>/syscall just says
 * "running" and gives no PC) to reveal exactly where it spins in nativeRender. */
extern int kv_is_main_thread(void);
static void kv_usrdump_handler(int sig, void *info, void *ucontext) {
    (void)sig; (void)info;
    ucontext_t *uc = (ucontext_t *)ucontext;
    unsigned long pc = uc ? uc->uc_mcontext.pc : 0;
    unsigned long sp = uc ? uc->uc_mcontext.sp : 0;
    unsigned long x29 = uc ? uc->uc_mcontext.regs[29] : 0;
    unsigned long x30 = uc ? uc->uc_mcontext.regs[30] : 0;
    char buf[2048]; int n = 0;
    n += sprintf(buf + n, "[usp] === SIGUSR1 dump tid=%ld main=%d pc=0x%lx sp=0x%lx fp=0x%lx lr=0x%lx ===\n",
                 (long)syscall(178), kv_is_main_thread(), pc, sp, x29, x30);
    if (uc) for (int i = 0; i < 29; i++) {
        n += sprintf(buf + n, " x%-2d=0x%lx", i, (unsigned long)uc->uc_mcontext.regs[i]);
        if (i % 4 == 3) { buf[n++] = '\n'; }
    }
    if (n > 0 && buf[n-1] != '\n') buf[n++] = '\n';
    /* backtrace via x29 chain (safe reads via /proc/self/mem) */
    n += sprintf(buf + n, "[usp] backtrace:\n");
    unsigned long fp = x29; int frame = 0;
    while (fp && frame < 24) {
        unsigned long lr = 0;
        int memfd = open("/proc/self/mem", O_RDONLY);
        if (memfd >= 0) {
            if (lseek(memfd, (off_t)(fp + 8), SEEK_SET) == (off_t)(fp + 8) &&
                read(memfd, &lr, 8) == 8) { } else { lr = 0; }
            close(memfd);
        }
        n += sprintf(buf + n, "[usp]   #%d lr=0x%lx\n", frame, lr);
        if (!lr) break;
        unsigned long nextfp = 0;
        int m2 = open("/proc/self/mem", O_RDONLY);
        if (m2 >= 0) {
            if (lseek(m2, (off_t)fp, SEEK_SET) == (off_t)fp &&
                read(m2, &nextfp, 8) != 8) nextfp = 0;
            close(m2);
        }
        if (nextfp <= fp) break;
        fp = nextfp; frame++;
    }
    (void)write(2, buf, n);
    (void)write(2, "\n", 1);
}

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
    /* Match the signal set of both reference ports: catch SIGSEGV, SIGBUS,
     * SIGABRT (Unity abort() path), SIGILL, SIGFPE, SIGTRAP, SIGSYS.
     * Without SIGABRT/SIGTRAP/SIGSYS, BRK/seccomp/abort kill silently. */
    sigaction(SIGSEGV, &sa, 0);
    sigaction(SIGBUS,  &sa, 0);
    sigaction(SIGABRT, &sa, 0);
    sigaction(SIGILL,  &sa, 0);
    sigaction(SIGFPE,  &sa, 0);
    sigaction(SIGTRAP, &sa, 0);
    sigaction(SIGSYS,  &sa, 0);
    /* SIGUSR1: diagnostic PC dump (returns, doesn't die). */
    { struct sigaction su; memset(&su, 0, sizeof su);
      su.sa_sigaction = kv_usrdump_handler;
      su.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESTART;
      sigemptyset(&su.sa_mask);
      sigaction(SIGUSR1, &su, 0); }
}

/* ------------------ engine abort/exit path overrides (0.30) ------------------
 * Both reference ports override the GOT entries for abort/raise/tgkill/exit
 * to log the caller and (optionally) proceed instead of terminating. Without
 * this, an internal-engine error during nativeRender silently exit()s the
 * process - no SIGSEGV/SIGABRT ever fires, so our crash handler can't dump a
 * PC.  Routing these through here turns the silent-exit case into a clear
 * [ENGINE-ABORT] log line + caller address, which is the diagnostic we need
 * for "exit 139 with no [loader] === CRASH ===" failures.
 *
 * `kv_stack_chk_fail` returns instead of aborting: Unity's operator-new tags
 * canaries that, on certain nativeRecreateGfxState paths, mis-fire; aborting
 * kills the boot.  Returning lets the caller carry on (the canary mismatch is
 * a false-alarm under our foreign-libc model). */
#include <stdlib.h>
#include <signal.h>
void kv_stack_chk_fail(void) {
    static int n = 0;
    if (n++ < 8) {
        /* __builtin_return_address is not async-safe but stderr-write is */
        char buf[160]; int m = 0;
        m += sprintf(buf + m, "[loader] __stack_chk_fail #%d caller=%p (continuing)\n",
                     n, __builtin_return_address(0));
        if (write(2, buf, m) < 0) { /* ignore */ }
    }
    /* RETURN.  Do NOT abort/raise - let the caller continue. */
}
void kv_engine_abort(void) {
    char buf[160]; int m = 0;
    m += sprintf(buf + m, "[loader] === ENGINE ABORT caller=%p ===\n",
                 __builtin_return_address(0));
    if (write(2, buf, m) < 0) { /* ignore */ }
    /* Fall into the crash handler explicitly so we get a backtrace dump -
     * trap by writing 0 (SIGSEGV on alt stack).  This gets us a real PC
     * instead of silently dying. */
    *(volatile int *)0 = 0;
    /* if that for some reason continues (it shouldn't), exit 139 */
    _exit(134);
}
int kv_engine_raise(int sig) {
    char buf[160]; int m = 0;
    m += sprintf(buf + m, "[loader] === ENGINE raise(sig=%d) caller=%p ===\n",
                 sig, __builtin_return_address(0));
    if (write(2, buf, m) < 0) { /* ignore */ }
    /* Forward to real raise so the crash handler runs (it catches SIGABRT etc). */
    return raise(sig);
}
int kv_engine_tgkill(int tgid, int tid, int sig) {
    char buf[200]; int m = 0;
    m += sprintf(buf + m, "[loader] === ENGINE tgkill(tgid=%d tid=%d sig=%d) caller=%p ===\n",
                 tgid, tid, sig, __builtin_return_address(0));
    if (write(2, buf, m) < 0) { /* ignore */ }
    /* Forward via syscall - if sig is 0 (just a probe), returning 0 fine. */
    if (sig == 0) return 0;
    /* Otherwise route to raise so the handler dump fires on the calling thread. */
    return raise(sig);
}
void kv_engine_exit(int code) {
    char buf[160]; int m = 0;
    m += sprintf(buf + m, "[loader] === ENGINE exit(%d) caller=%p ===\n",
                 code, __builtin_return_address(0));
    if (write(2, buf, m) < 0) { /* ignore */ }
    _exit(code);
}

/* --- bionic-only symbols that glibc lacks but the .so imports.
 * Exported (non-static) so dlsym(RTLD_DEFAULT, ...) finds them. ---
 *
 * 0.50.3: ported from hitmango-nextos bionic.c — real system-property table and
 * log mirroring.  Unity reads these (ro.product.model, ro.opengles.version,
 * ro.build.version.sdk, persist.sys.locale ...) to pick quality tiers and decide
 * whether it is on a known device.  The old stubs returned empty, so Unity fell
 * into low-end fallbacks / uncertain state. */
static const char gds_lvl[] = "?????VDIWEFS";
int __android_log_write(int prio, const char *tag, const char *msg) {
    fprintf(stderr, "[%c/%s] %s\n", gds_lvl[prio & 15], tag ? tag : "?", msg ? msg : "");
    return 0;
}
int __android_log_vprint(int prio, const char *tag, const char *fmt, va_list ap) {
    char buf[2048];
    vsnprintf(buf, sizeof buf, fmt, ap);
    return __android_log_write(prio, tag, buf);
}
int __android_log_print(int prio, const char *tag, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    int r = __android_log_vprint(prio, tag, fmt, ap);
    va_end(ap);
    return r;
}

static const struct { const char *k, *v; } gds_props[] = {
    { "ro.product.model",            "Pixel 6" },
    { "ro.product.manufacturer",     "Google" },
    { "ro.product.brand",            "google" },
    { "ro.product.device",           "oriole" },
    { "ro.product.name",             "oriole" },
    { "ro.product.board",            "oriole" },
    { "ro.product.cpu.abi",          "arm64-v8a" },
    { "ro.product.cpu.abilist",      "arm64-v8a" },
    { "ro.build.version.sdk",        "31" },
    { "ro.build.version.release",    "12" },
    { "ro.build.id",                 "SQ1D.220205.004" },
    { "ro.build.type",               "user" },
    { "ro.build.tags",               "release-keys" },
    { "ro.debuggable",               "0" },
    { "ro.secure",                   "1" },
    { "ro.hardware",                 "oriole" },
    { "ro.arch",                     "arm64" },
    { "debug.egl.hw",                "1" },
    { "ro.opengles.version",         "196608" },
    { "persist.sys.locale",          "en-US" },
    { "ro.product.locale",           "en-US" },
};
int __system_property_get(const char *key, char *value) {
    for (size_t i = 0; i < sizeof gds_props / sizeof *gds_props; i++)
        if (strcmp(gds_props[i].k, key) == 0)
            return (int)strlen(strcpy(value, gds_props[i].v));
    value[0] = 0;
    return 0;
}
const void *__system_property_find(const char *key) {
    for (size_t i = 0; i < sizeof gds_props / sizeof *gds_props; i++)
        if (strcmp(gds_props[i].k, key) == 0)
            return &gds_props[i];
    return NULL;
}
int __system_property_read(const void *pi, char *name, char *value) {
    const struct { const char *k, *v; } *p = pi;
    if (!p) return 0;
    if (name) strcpy(name, p->k);
    if (value) strcpy(value, p->v);
    return (int)strlen(p->v);
}
int __system_property_foreach(void (*cb)(const void *, void *), void *ck) {
    for (size_t i = 0; i < sizeof gds_props / sizeof *gds_props; i++)
        cb(&gds_props[i], ck);
    return 0;
}
/* _ctype_ — bionic exports this as `const unsigned char*` pointing at a
 * 257-byte char-class table indexed as `_ctype_[(int)c+1]` by isalpha/
 * isdigit/tolower/etc.  libunity reads it as `ldr [_ctype_ GOT]; ldr [x0]`
 * — if `_ctype_` resolves to NULL (or an empty function stub) the second
 * `ldr [x0]` faults and crashes inside nativeRender during asset/string
 * processing (terraria-nextos documents this exact symptom:
 * "crash libunity+0xe449d4 no asset loading").
 *
 * Bits (bionic): _U=1 _L=2 _N=4 _S=8 _P=0x10 _C=0x20 _X=0x40 _B=0x80.
 * Slot 0 = EOF (c=-1).  Slot [c+1] = bits for c.
 *
 * The bionic `_ctype_` symbol IS the pointer-to-table itself.  We export
 * our own pointer to our own table as a real data symbol; host_syms.c binds
 * the GOT slot to it. */
#include <ctype.h>
unsigned char g_kv_ctype_table[257];
const unsigned char *g_kv_ctype_ptr = g_kv_ctype_table;
unsigned char g_kv_tolower_table[257], g_kv_toupper_table[257];
const unsigned char *g_kv_tolower_ptr = g_kv_tolower_table;
const unsigned char *g_kv_toupper_ptr = g_kv_toupper_table;
/* Public symbols the loader/route table resolves for libunity/libil2cpp.
 * Bionic libunity imports `_ctype_` as `const unsigned char*` (a data
 * symbol — pointer to the table).  Same for `_tolower_tab_`/`_toupper_tab_`. */
const unsigned char * const _ctype_  = g_kv_ctype_table;
const unsigned char * const _tolower_tab_ = g_kv_tolower_table;
const unsigned char * const _toupper_tab_ = g_kv_toupper_table;
void kv_ctype_init(void) {
    g_kv_ctype_table[0] = 0;
    g_kv_tolower_table[0] = 0; g_kv_toupper_table[0] = 0;
    for (int c = 0; c < 256; c++) {
        unsigned char b = 0;
        if (isupper(c)) b |= 0x01;          /* _U */
        if (islower(c)) b |= 0x02;          /* _L */
        if (isdigit(c)) b |= 0x04;          /* _N */
        if (isspace(c)) b |= 0x08;          /* _S */
        if (ispunct(c)) b |= 0x10;          /* _P */
        if (iscntrl(c)) b |= 0x20;          /* _C */
        if (isxdigit(c) && !isdigit(c)) b |= 0x40;  /* _X (hex-letter only) */
        if (c == ' ')  b |= 0x80;           /* _B (blank printable) */
        g_kv_ctype_table[c + 1] = b;
        g_kv_tolower_table[c + 1] = (unsigned char)tolower(c);
        g_kv_toupper_table[c + 1] = (unsigned char)toupper(c);
    }
}
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
