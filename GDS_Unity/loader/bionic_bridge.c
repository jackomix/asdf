/* bionic_bridge.c - bionic->glibc ABI bridge for the GDS glibc loader.
 *
 * libil2cpp.so / libunity.so are Android (bionic) binaries.  Under glibc their
 * calls into libc/pthread/signal hit ABI mismatches that corrupt memory:
 *
 *   - sigset_t is 8 bytes (64-bit mask) in arm64 bionic but 128 bytes in
 *     glibc.  If a bionic caller passes an 8-byte sigset_t to glibc's
 *     sigemptyset/sigaction/pthread_sigmask, glibc writes 128 bytes into it ->
 *     heap/stack corruption ("malloc(): invalid size (unsorted)").
 *   - pthread_mutex_t / pthread_cond_t / pthread_rwlock_t / sem_t have
 *     different sizes/layouts.  We store a pointer to a real glibc object in
 *     the bionic object's first word ("slot" trick) and route all ops through
 *     glibc, so no size-mismatch overflow.
 *
 * Adapted from terraria-nextos (src/pthread_fake.c, src/bionic_shims.c).
 * The loader calls these only to RESOLVE the .so's imports (see resolve()),
 * so the loader's own glibc pthread/signal usage is unaffected.
 */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <pthread.h>
#include <semaphore.h>
#include <sched.h>
#include <time.h>
#include <dlfcn.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <sys/vfs.h>

void *kv_set_job_workers_zero(void *unused);  /* defined in loader_glibc_main.c */
void *kv_il_sym(const char *name);              /* ditto */
int kv_jobworkers_is_done(void);                /* ditto */

/* ---- statfs shim: defeat Unity's "Not enough storage space" dialog ----
 * libunity.so imports statfs and checks the free space on the target volume
 * before deciding whether it can install/extract il2cpp resources.  Under glibc
 * the real statfs can return -1 on an Android path Unity probes, which makes
 * Unity conclude there's not enough storage and pop an AlertDialog that blocks
 * the whole boot (the log's final AlertDialog$Builder/setMessage).  We report
 * abundant free space so the check always passes.  Offsets below are the glibc
 * aarch64 struct statfs layout (kernel-compatible, matches bionic). */
int kv_statfs(const char *path, void *buf) {
    (void)path;
    struct statfs *s = (struct statfs *)buf;
    if (!s) { errno = EINVAL; return -1; }
    memset(s, 0, sizeof(struct statfs));
    s->f_bsize = 4096;
    s->f_blocks = (1ULL << 40) / 4096;   /* 1 TiB total */
    s->f_bfree  = s->f_blocks;           /* all free */
    s->f_bavail = s->f_blocks;
    s->f_frsize = 4096;
    s->f_files = 1000000;
    s->f_ffree = 1000000;
    s->f_namelen = 255;
    s->f_flags = 0;
    return 0;
}

/* ================= sigset_t: bionic(8B) <-> glibc(128B) ================= */
typedef unsigned long kv_bionic_sigset_t;   /* arm64 bionic: 64-bit mask */
_Static_assert(sizeof(kv_bionic_sigset_t) == 8, "arm64 bionic sigset ABI");

static void b_sig_to_host(kv_bionic_sigset_t b, sigset_t *host) {
  sigemptyset(host);
  for (int s = 1; s <= 64; s++) if (b & (1UL << (s - 1))) sigaddset(host, s);
}
static kv_bionic_sigset_t host_to_b_sig(const sigset_t *host) {
  kv_bionic_sigset_t b = 0;
  for (int s = 1; s <= 64; s++) if (sigismember(host, s)) b |= (1UL << (s - 1));
  return b;
}

int kv_sigemptyset(kv_bionic_sigset_t *set) { if (!set) { errno = EINVAL; return -1; } *set = 0; return 0; }
int kv_sigfillset(kv_bionic_sigset_t *set) { if (!set) { errno = EINVAL; return -1; } *set = ~0UL; return 0; }
int kv_sigaddset(kv_bionic_sigset_t *set, int sn) {
  if (!set || sn < 1 || sn > 64) { errno = EINVAL; return -1; } *set |= 1UL << (sn - 1); return 0;
}
int kv_sigdelset(kv_bionic_sigset_t *set, int sn) {
  if (!set || sn < 1 || sn > 64) { errno = EINVAL; return -1; } *set &= ~(1UL << (sn - 1)); return 0;
}
int kv_sigsuspend(const kv_bionic_sigset_t *set) {
  if (!set) { errno = EINVAL; return -1; } sigset_t host; b_sig_to_host(*set, &host); return sigsuspend(&host);
}

/* bionic sigaction: {flags(int), handler(ptr), mask(8B), restorer(ptr)} */
struct kv_bionic_sigaction { int bsa_flags; void *bsa_handler; unsigned long bsa_mask; void *bsa_restorer; };

/* Block Unity from replacing our alt stack (we sized ours for our handler).
 * Pretend-success: return 0, do nothing.  (Real sigaltstack still in effect.) */
int kv_sigaltstack_noop(const void *ss, void *old_ss) {
  (void)ss;
  if (old_ss) memset(old_ss, 0, sizeof(stack_t));   /* report empty as "no alt stack" */
  return 0;
}
/* List of crash/abort signals we MUST keepOurHandler on.  Unity's engine (and
 * the Mali driver, and the IL2CPP GC) install their own handlers on these
 * during boot, OVERWRITING ours - then any SIGSEGV/SIGABRT goes to their
 * default-action path which terminates the process silently (exit 139, no
 * [loader] === CRASH line).  Both reference ports (terraria-nextos
 * bionic_shims.c:117 and horizonchase-nextos) block these installs outright
 * by pretend-success: when Unity calls sigaction(sig, ...) on one of these
 * we return 0 and DON'T call the real sigaction.  Faking "installed" lets
 * Unity proceed, but our on_crash stays as the actual handler.
 *
 * Signal numbers:
 *   4 SIGILL, 5 SIGTRAP, 6 SIGABRT, 7 SIGBUS, 8 SIGFPE, 11 SIGSEGV
 * (aarch64 Linux same as glibc.) */
static int kv_is_crash_sig(int sig) {
  return sig == 4 || sig == 5 || sig == 6 || sig == 7 || sig == 8 || sig == 11
#ifdef SIGSYS
      || sig == SIGSYS
#endif
      ;
}
int kv_sigaction(int sig, const struct kv_bionic_sigaction *act,
                 struct kv_bionic_sigaction *oldact) {
  /* Block Unity from installing its own handler on crash signals.
   * Pretend-success (return 0) so the caller thinks it won. */
  if (kv_is_crash_sig(sig)) {
    if (oldact) {
      /* Report our currently-installed handler as the "old" so callers that
       * save+restore will only ever restore OUR handler, not Unity's. */
      struct sigaction cur; memset(&cur, 0, sizeof cur);
      sigaction(sig, NULL, &cur);
      oldact->bsa_flags = cur.sa_flags;
      oldact->bsa_handler = (cur.sa_flags & SA_SIGINFO) ? (void *)cur.sa_sigaction : (void *)cur.sa_handler;
      oldact->bsa_mask = 0;
      oldact->bsa_restorer = NULL;
    }
    return 0;
  }
  struct sigaction ga, go; struct sigaction *pga = NULL, *pgo = NULL;
  if (act) {
    memset(&ga, 0, sizeof ga);
    ga.sa_flags = act->bsa_flags;
    if (act->bsa_flags & SA_SIGINFO)
      ga.sa_sigaction = (void (*)(int, siginfo_t *, void *))act->bsa_handler;
    else ga.sa_handler = (void (*)(int))act->bsa_handler;
    sigemptyset(&ga.sa_mask);
    for (int s = 1; s <= 64; s++) if (act->bsa_mask & (1UL << (s - 1))) sigaddset(&ga.sa_mask, s);
    pga = &ga;
  }
  if (oldact) { memset(&go, 0, sizeof go); pgo = &go; }
  int r = sigaction(sig, pga, pgo);
  if (oldact) {
    oldact->bsa_flags = go.sa_flags;
    oldact->bsa_handler = (go.sa_flags & SA_SIGINFO) ? (void *)go.sa_sigaction : (void *)go.sa_handler;
    oldact->bsa_mask = host_to_b_sig(&go.sa_mask);
    oldact->bsa_restorer = NULL;
  }
  return r;
}

int kv_pthread_sigmask(int how, const void *set, void *old) {
  sigset_t gset, gold; sigset_t *pset = NULL, *pold = NULL;
  if (set) {
    sigemptyset(&gset);
    unsigned long bm = set ? *(const unsigned long *)set : 0UL;   /* bionic 8B mask */
    for (int s = 1; s <= 64; s++) if (bm & (1UL << (s - 1))) sigaddset(&gset, s);
    pset = &gset;
  }
  if (old) pold = &gold;
  int r = pthread_sigmask(how, pset, pold);
  if (old) {
    unsigned long bm = 0;
    for (int s = 1; s <= 64; s++) if (sigismember(&gold, s)) bm |= (1UL << (s - 1));
    *(unsigned long *)old = bm;   /* return bionic 8B mask */
  }
  return r;
}

/* ================= pthread mutex / cond / rwlock / sem (slot->glibc) ================= */
static pthread_mutex_t g_lazy = PTHREAD_MUTEX_INITIALIZER;

static pthread_mutex_t *mtx_get(void **slot) {
  void *cur = *slot;
  if (cur) return (pthread_mutex_t *)cur;
  pthread_mutex_lock(&g_lazy);
  cur = *slot;
  if (!cur) {
    pthread_mutexattr_t a; pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_t *m = malloc(sizeof(pthread_mutex_t));
    pthread_mutex_init(m, &a); pthread_mutexattr_destroy(&a);
    *slot = cur = m;
  }
  pthread_mutex_unlock(&g_lazy);
  return (pthread_mutex_t *)cur;
}
int kv_pthread_mutex_init(void **slot, const void *attr) { (void)attr; *slot = NULL; mtx_get(slot); return 0; }
int kv_pthread_mutex_destroy(void **slot) { if (*slot) { pthread_mutex_destroy((pthread_mutex_t *)*slot); free(*slot); *slot = NULL; } return 0; }
int kv_pthread_mutex_lock(void **slot) { return pthread_mutex_lock(mtx_get(slot)); }
int kv_pthread_mutex_unlock(void **slot) { if (!*slot) return 0; return pthread_mutex_unlock((pthread_mutex_t *)*slot); }
int kv_pthread_mutex_trylock(void **slot) { return pthread_mutex_trylock(mtx_get(slot)); }

static pthread_cond_t *cond_get(void **slot) {
  void *cur = *slot;
  if (cur) return (pthread_cond_t *)cur;
  pthread_mutex_lock(&g_lazy);
  cur = *slot;
  if (!cur) { pthread_cond_t *c = malloc(sizeof(pthread_cond_t)); pthread_cond_init(c, NULL); *slot = cur = c; }
  pthread_mutex_unlock(&g_lazy);
  return (pthread_cond_t *)cur;
}
int kv_pthread_cond_init(void **slot, const void *attr) { (void)attr; *slot = NULL; cond_get(slot); return 0; }
int kv_pthread_cond_destroy(void **slot) { if (*slot) { pthread_cond_destroy((pthread_cond_t *)*slot); free(*slot); *slot = NULL; } return 0; }
int kv_pthread_cond_signal(void **slot) { return pthread_cond_signal(cond_get(slot)); }
int kv_pthread_cond_broadcast(void **slot) { return pthread_cond_broadcast(cond_get(slot)); }
/* POLLING (breaks the job-system deadlock): on this loader the workers + main
 * thread all futex-wait on cond/sem that may never be signaled (the Android
 * Java Activity / looper that drives them doesn't exist).  Instead of blocking
 * forever, wait a short slice and return as a spurious wakeup, so the caller
 * re-checks its predicate in its while() loop and can make progress.  This is
 * Terraria's CUP_CONDPOLL approach.  The main thread (tid == getpid) polls
 * shorter so it stays responsive; workers poll longer to avoid burning CPU. */
#define KV_COND_POLL_MAIN_NS 2000000L    /* 2 ms for the main thread */
#define KV_COND_POLL_WORK_NS 5000000L    /* 5 ms for worker threads */
int kv_is_main_thread(void) {
    return (int)syscall(SYS_gettid) == (int)getpid();
}
static int kv_cond_poll_wait(void **cslot, void **mslot) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    long ns = kv_is_main_thread() ? KV_COND_POLL_MAIN_NS : KV_COND_POLL_WORK_NS;
    ts.tv_nsec += ns;
    if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
    int r = pthread_cond_timedwait(cond_get(cslot), mtx_get(mslot), &ts);
    return (r == ETIMEDOUT) ? 0 : r;   /* timeout -> spurious wakeup */
}
int kv_pthread_cond_wait(void **cslot, void **mslot) {
    /* If called on the MAIN thread: return 0 immediately (spurious wake).
     * ALSO: opportunistically invoke set_JobWorkerCount(0) on the FIRST main
     * call where Unity's il2cpp domain is already set up.  By the time Unity
     * main reaches cond_wait inside nativeRender, Unity's own il2cpp_init has
     * at least created the root domain (dom_get() != NULL).  Calling
     * il2cpp_runtime_invoke(set_JobWorkerCount, 0) here from main works
     * because main thread has full il2cpp thread context.
     *
     * terra-nextos does the equivalent from its eglSwapBuffers hook.
     * We do it inline here because eglSwapBuffers never returns for us. */
    cond_get(cslot); mtx_get(mslot);
    if (kv_is_main_thread()) {
        static int jobfix_tried = 0;
        if (!jobfix_tried) {
            void *(*dom_get)(void) = (void *(*)(void))kv_il_sym("il2cpp_domain_get");
            if (dom_get && dom_get()) {
                jobfix_tried = 1;
                printf("[jobfix] main cond_wait + dom_get OK - running set_JobWorkerCount once\n");
                fflush(stdout);
                kv_set_job_workers_zero(0);
            }
            /* if dom_get returns NULL: leave jobfix_tried=0, retry on next cond_wait;
             * Unity continues its own init past us because we return 0 immediately. */
        }
        return 0;
    }
    return kv_cond_poll_wait(cslot, mslot);
}
int kv_pthread_cond_timedwait(void **cslot, void **mslot, const struct timespec *ts) {
    /* Like cond_wait: if main thread, return 0 immediately (spurious wake).
     * Otherwise honor caller deadline but cap to KV_COND_POLL_WORK_NS so
     * idle workers don't burn CPU. */
    cond_get(cslot); mtx_get(mslot);
    if (kv_is_main_thread()) return 0;
    struct timespec now; clock_gettime(CLOCK_REALTIME, &now);
    long ns = KV_COND_POLL_WORK_NS;
    struct timespec cap = *ts;
    long capns = (cap.tv_sec - now.tv_sec) * 1000000000L + (cap.tv_nsec - now.tv_nsec);
    if (capns < 0 || capns > ns) capns = ns;
    struct timespec use = now;
    use.tv_nsec += capns;
    if (use.tv_nsec >= 1000000000L) { use.tv_sec++; use.tv_nsec -= 1000000000L; }
    return pthread_cond_timedwait(cond_get(cslot), mtx_get(mslot), &use);
}

static sem_t *sem_get(void **slot) {
  void *cur = *slot;
  if (cur) return (sem_t *)cur;
  pthread_mutex_lock(&g_lazy);
  cur = *slot;
  if (!cur) { sem_t *s = malloc(sizeof(sem_t)); sem_init(s, 0, 0); *slot = cur = s; }
  pthread_mutex_unlock(&g_lazy);
  return (sem_t *)cur;
}
int kv_sem_init(void **slot, int pshared, unsigned value) {
  (void)pshared; sem_t *s = malloc(sizeof(sem_t)); sem_init(s, 0, value); *slot = s; return 0;
}
int kv_sem_destroy(void **slot) { if (*slot) { sem_destroy((sem_t *)*slot); free(*slot); *slot = NULL; } return 0; }
int kv_sem_post(void **slot) { return sem_post(sem_get(slot)); }
/* Polling sem_wait: like cond_wait, cap the wait so a sem that is never posted
 * (Android job/looper absent) wakes the caller to re-check.  */
int kv_sem_wait(void **slot) {
    static int once; if (!once && (once = 1))
        printf("[kv_sem_wait] routed! tid=%ld slot=%p *slot=%p\n", (long)syscall(178), (void*)slot, slot ? (void*)*slot : 0);
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    long ns = kv_is_main_thread() ? KV_COND_POLL_MAIN_NS : KV_COND_POLL_WORK_NS;
    ts.tv_nsec += ns;
    if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
    int r = sem_timedwait(sem_get(slot), &ts);
    return (r == ETIMEDOUT) ? EAGAIN : r;   /* timeout -> treat as would-block */
}
int kv_sem_trywait(void **slot) { return sem_trywait(sem_get(slot)); }
int kv_sem_getvalue(void **slot, int *v) { return sem_getvalue(sem_get(slot), v); }
int kv_sem_timedwait(void **slot, const struct timespec *ts) { return sem_timedwait(sem_get(slot), ts); }

static pthread_rwlock_t *rwl_get(void **slot) {
  void *cur = *slot;
  if (cur) return (pthread_rwlock_t *)cur;
  pthread_mutex_lock(&g_lazy);
  cur = *slot;
  if (!cur) { pthread_rwlock_t *r = malloc(sizeof(pthread_rwlock_t)); pthread_rwlock_init(r, NULL); *slot = cur = r; }
  pthread_mutex_unlock(&g_lazy);
  return (pthread_rwlock_t *)cur;
}
int kv_pthread_rwlock_init(void **slot, const void *a) { (void)a; *slot = NULL; rwl_get(slot); return 0; }
int kv_pthread_rwlock_destroy(void **slot) { if (*slot) { pthread_rwlock_destroy((pthread_rwlock_t *)*slot); free(*slot); *slot = NULL; } return 0; }
int kv_pthread_rwlock_rdlock(void **slot) { return pthread_rwlock_rdlock(rwl_get(slot)); }
int kv_pthread_rwlock_wrlock(void **slot) { return pthread_rwlock_wrlock(rwl_get(slot)); }
int kv_pthread_rwlock_unlock(void **slot) { if (!*slot) return 0; return pthread_rwlock_unlock((pthread_rwlock_t *)*slot); }

/* ================= pthread thread/key/once/attr (compat on arm64) ================= */
int kv_pthread_detach(pthread_t t) {
  if ((uintptr_t)t == 0xdeadbeef) return 0;
  return pthread_detach(t);
}
int kv_pthread_join(pthread_t t, void **r) {
  if ((uintptr_t)t == 0xdeadbeef) { if (r) *r = 0; return 0; }
  return pthread_join(t, r);
}
pthread_t kv_pthread_self(void) { return pthread_self(); }

/* 0.39.6: Wrap Unity worker start_routine so the new thread attaches to
 * il2cpp domain BEFORE running the original start. Without this, the worker
 * start_routine calls il2cpp class lookups with no thread context -> NULL
 * MonoClass* -> crash in mono_class_get_checked. terra-nextos achieves the
 * same result via il2cpp_thread_attach(domain) at top of every worker bootstrap. */
struct kv_worker_ctx { void *(*orig_start)(void *); void *orig_arg; };
static void *kv_worker_wrapper(void *ctxv) {
  struct kv_worker_ctx *ctx = (struct kv_worker_ctx *)ctxv;
  printf("[kv_worker] wrapper entered tid=%ld orig_start=%p arg=%p\n",
         (long)syscall(178), (void*)ctx->orig_start, (void*)ctx->orig_arg);
  fflush(stdout);
  void *(*att)(void *) = (void *(*)(void *))kv_il_sym("il2cpp_thread_attach");
  void *(*dom)(void)   = (void *(*)(void))kv_il_sym("il2cpp_domain_get");
  (void)att; (void)dom;
  /* 0.39.10: ABORTED il2cpp_thread_attach / mono_thread_attach attempt —
   * both trip Unity's internal "Threads explicit registering is not
   * previously enabled" assertion because bionic's pthread_create hook
   * (which on Android+Unity registers a thread before its start_routine runs)
   * is not reproduced under glibc. Enabling explicit registration requires
   * running through Unity's own internal il2cpp_init phase that occurs
   * AFTER the worker has finished its own thread setup — a chicken-and-egg
   * problem. Next session: inspect Unity's exact pthread_atfork or
   * init_array ordering in libil2cpp's .init to find the implicit
   * "register-main-thread" call site, then replicate it before spawn. */
  /* 0.39.9: il2cpp_thread_attach aborts with "Threads explicit registering is
   * not previously enabled" because il2cpp guards attach behind an internal
   * explicit-registration flag. mono_thread_attach (lower-level Mono API)
   * bypasses that gate. Use mono_thread_attach exclusively. */
  void *(*mono_att)(void *) = (void *(*)(void *))kv_il_sym("mono_thread_attach");
  (void)mono_att;  /* not used after abort discovered; left for future */
  printf("[kv_worker] dispatching orig_start (no attach attempted - aborts Unity)\n"); fflush(stdout);
  printf("[kv_worker] dispatching orig_start\n"); fflush(stdout);
  void *(*s)(void *) = ctx->orig_start; void *a = ctx->orig_arg;
  free(ctx);
  return s(a);
}
int kv_pthread_create(pthread_t *t, const void *attr, void *(*start)(void *), void *arg) {
  static int n; long tid = syscall(178);
  if (++n <= 300) printf("[kv_pthread_create] n=%d tid=%ld start=%p arg=%p\n", n, tid, (void*)start, (void*)arg);
  (void)attr;
  /* 0.39.6: Unity's JobWorker start_routine lives inside libunity (worker
   * start = libunity+0x508174 under 0.39 log).  It crashes immediately in
   * mono_class_get_checked because the new thread is NOT auto-attached to
   * il2cpp (Unity expects pthread_create to register the thread; under our
   * loader it doesn't) AND because the worker's role is running il2cpp_init
   * to set up the domain — Unity main is blocked on cond_wait waiting for
   * this worker.  But the worker crashes before completing init.
   *
   * 0.39.6 first attempt: detoured the spawn (returned fake thread_t).
   * Result: Unity main, with spurious cond_wait return, proceeded PAST its
   * wait point while il2cpp_init never ran → dom_get NULL → same crash
   * (mono_class_get_checked from lr=libunity+0x5c3550).
   *
   * New plan: Let worker spawn, but it WILL crash because the worker's
   * start_routine body does il2cpp_init / class lookup with no thread attach.
   * So we must DETOUR the worker's START routine — wrap it so that BEFORE
   * the user-supplied start runs, we attach the thread to il2cpp (so Unity
   * can register it and class lookups work).  terra-nextos do this via
   * il2cpp_thread_attach(domain) at top of worker bootstrap.
   *
   * Implement by replacing `start` with our wrapper that does
   *   il2cpp_thread_attach(il2cpp_domain_get());
   * then calls original `start(arg)`.  But domain might be NULL at this
   * point — Unity main never ran il2cpp_init (the WHOLE POINT of this
   * worker is init).  So thread_attach with NULL domain would fail too.
   *
   * Real worker code path is unknown yet.  Fall back: spawn NORMALLY and
   * let it crash, capture log, then we know start_routine body. */
  void *att = kv_il_sym("il2cpp_thread_attach");           /* libil2cpp */
  void *unity_send = kv_il_sym("UnitySendMessage");         /* libunity */
  uintptr_t lo_il = (uintptr_t)att - 0xce47ec;               /* libil2cpp base */
  uintptr_t hi_il = lo_il + 0x221f000;
  uintptr_t lo_un = (uintptr_t)unity_send - 0x642b18;          /* libunity base */
  uintptr_t hi_un = lo_un + 0x103a000;
  uintptr_t s = (uintptr_t)start;
  int il2 = (att && s >= lo_il && s < hi_il);
  int un  = (unity_send && s >= lo_un && s < hi_un);
  if (il2 || un) {
    printf("[kv_pthread_create] Unity-internal worker spawn (start=%p mod=%s offset=0x%lx) — wrapping with thread_attach\n",
           (void*)s, il2?"libil2cpp":"libunity",
           (unsigned long)(s - (il2?lo_il:lo_un)));
    fflush(stdout);
    struct kv_worker_ctx *ctx = (struct kv_worker_ctx *)malloc(sizeof(*ctx));
    if (!ctx) return ENOMEM;
    ctx->orig_start = start; ctx->orig_arg = arg;
    return pthread_create(t, NULL, kv_worker_wrapper, ctx);
  }
  return pthread_create(t, NULL, start, arg);
}
int kv_pthread_attr_init(void *a) { (void)a; return 0; }
int kv_pthread_attr_destroy(void *a) { (void)a; return 0; }
int kv_pthread_attr_setdetachstate(void *a, int s) { (void)a; (void)s; return 0; }
int kv_pthread_attr_setstacksize(void *a, size_t s) { (void)a; (void)s; return 0; }
int kv_pthread_attr_setschedparam(void *a, const void *p) { (void)a; (void)p; return 0; }
int kv_pthread_setschedparam(pthread_t t, int p, const void *s) { (void)t; (void)p; (void)s; return 0; }
int kv_pthread_setname_np(pthread_t t, const char *n) { (void)t; (void)n; return 0; }
int kv_pthread_getattr_np(pthread_t t, void *attr) { (void)t; (void)attr; return 0; }
int kv_pthread_attr_getstack(void *attr, void **addr, size_t *size) {
  (void)attr;
  unsigned long sp; __asm__ volatile("mov %0, sp" : "=r"(sp));
  size_t sz = 8 * 1024 * 1024; unsigned long base = (sp & ~0xfffUL) - sz;
  if (addr) *addr = (void *)base; if (size) *size = sz; return 0;
}
int kv_pthread_key_create(unsigned *k, void (*d)(void *)) { return pthread_key_create((pthread_key_t *)k, d); }
int kv_pthread_key_delete(unsigned k) { return pthread_key_delete((pthread_key_t)k); }
void *kv_pthread_getspecific(unsigned k) { return pthread_getspecific((pthread_key_t)k); }
int kv_pthread_setspecific(unsigned k, const void *v) { return pthread_setspecific((pthread_key_t)k, v); }
int kv_pthread_once(int *o, void (*f)(void)) { return pthread_once((pthread_once_t *)o, f); }
int kv_pthread_equal(pthread_t a, pthread_t b) { return pthread_equal(a, b); }
int kv_pthread_kill(pthread_t t, int sig) {
  if ((uintptr_t)t == 0xdeadbeef) return 0;
  return pthread_kill(t, sig);
}
int kv_pthread_atfork(void (*pre)(void), void (*parent)(void), void (*child)(void)) {
  (void)pre; (void)parent; (void)child; return 0;   /* no fork() in this game */
}
int kv_pthread_condattr_init(void *a) { (void)a; return 0; }
int kv_pthread_condattr_destroy(void *a) { (void)a; return 0; }
int kv_pthread_condattr_setclock(void *a, int c) { (void)a; (void)c; return 0; }
int kv_pthread_mutexattr_init(void *a) { (void)a; return 0; }
int kv_pthread_mutexattr_destroy(void *a) { (void)a; return 0; }
int kv_pthread_mutexattr_settype(void *a, int t) { (void)a; (void)t; return 0; }

/* ---- syscall shim: raw futex + sched_getaffinity (horizonchase/terraria) ----
 * Unity's JOB SYSTEM calls raw `syscall(SYS_futex, FUTEX_WAIT, ...)` DIRECTLY,
 * bypassing pthread_cond/sem.  That is why the worker threads in every thread
 * dump futex-wait with timeout=NULL (0x0) - my pthread_cond/sem polling shims
 * never reach them.  Horizonchase's TER_FUTEXPOLL solves this by intercepting
 * syscall() and injecting a short timeout into any FUTEX_WAIT without one, so
 * the waiter wakes periodically, re-checks its predicate, and can make progress.
 * Also intercept SYS_sched_getaffinity to force 1 CPU at the syscall level. */
#ifndef SYS_futex
#define SYS_futex 98
#endif
#ifndef SYS_sched_getaffinity
#define SYS_sched_getaffinity 123
#endif
static long kv_futexpoll_ms = 2;   /* 2 ms poll slice for raw futex waits */
extern long syscall(long n, ...);
static long kv_syscall(long n, long a1, long a2, long a3, long a4, long a5, long a6) {
    static int once; if (!once && (once = 1))
        printf("[kv_syscall] routed! n=%ld tid=%ld\n", n, (long)syscall(178));
    static int clone_n; if (n == 220 /*SYS_clone*/) { if (++clone_n <= 30) printf("[kv_syscall] SYS_clone #%d flags=%lx parent=%ld\n", clone_n, (unsigned long)a1, (long)syscall(178)); }
    if (n == 221 /*SYS_clone3*/ && (++once, 1)) {
        static int cl3; if (++cl3 <= 30) printf("[kv_syscall] SYS_clone3 args=%lx pidtid=%p parent=%ld\n", (unsigned long)a1, (void*)a2, (long)syscall(178));
    }
    if (n == 56 /*SYS_openat*/) printf("[kv_syscall] SYS_openat dirfd=%ld path=%s tid=%ld\n", a1, (char*)a2, (long)syscall(178));
    /* force 1 CPU at the syscall level (job workers = num_cpus - 1) */
    if (n == SYS_sched_getaffinity && a3) {
        long r = syscall(n, a1, a2, a3, a4, a5, a6);
        if (r > 0) { memset((void *)a3, 0, (size_t)a2); *(unsigned long *)a3 = 1UL; }
        return r > 0 ? r : (memset((void *)a3, 0, 8), *(unsigned long *)a3 = 1UL, 8);
    }
    if (n == SYS_futex) {
        int op = (int)a2 & 0x7f;
        if (op == 0 /*FUTEX_WAIT*/ || op == 9 /*FUTEX_WAIT_BITSET*/) {
            long tid = syscall(178 /*SYS_gettid*/);
            /* CRITICAL: the MAIN thread deadlocks on a raw futex (Unity's job
             * system), never reaching kv_pthread_cond_wait where the jobfix
             * lives.  So set_JobWorkerCount(0) never fires, Unity keeps
             * dispatching jobs to workers that never run them, and main futex-
             * waits forever.  FIRE the jobfix from the main-thread futex wait,
             * exactly as terraria fires its ter_jobworkers0 from eglSwapBuffers
             * (both are re-entrant into Unity's render path).  When
             * JobWorkerCount=0, Unity runs jobs INLINE -> the job completes ->
             * main's futex predicate becomes true -> main advances. */
            if (tid == (long)getpid() && !kv_jobworkers_is_done()) {
                static int jf_tried = 0;
                if (!jf_tried) {
                    void *(*dom_get)(void) = (void *(*)(void))kv_il_sym("il2cpp_domain_get");
                    if (dom_get && dom_get()) {
                        jf_tried = 1;
                        printf("[jobfix] main futex-wait + dom_get OK - set_JobWorkerCount(0)\n");
                        fflush(stdout);
                        kv_set_job_workers_zero(0);
                    }
                }
            }
            printf("[kv_syscall] SYS_futex tid=%ld op=%d a4=%p\n", tid, op, (void*)a4);
            /* FUTEX_WAIT: (uaddr, op, val, timeout). FUTEX_WAIT_BITSET: (uaddr,
             * op, val, timeout, bitset).  a4 = timeout (0 = infinite). */
            long t4 = a4;
            struct timespec ts;
            if (a4 == 0) {   /* infinite wait -> inject poll timeout */
                int clk = ((int)a2 & 256 /*FUTEX_CLOCK_REALTIME*/) ? CLOCK_REALTIME : CLOCK_MONOTONIC;
                if (op == 0) { ts.tv_sec = 0; ts.tv_nsec = kv_futexpoll_ms * 1000000L; }
                else {
                    clock_gettime(clk, &ts);
                    ts.tv_sec += kv_futexpoll_ms / 1000;
                    ts.tv_nsec += (kv_futexpoll_ms % 1000) * 1000000L;
                    if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
                }
                t4 = (long)&ts;
            }
            return syscall(n, a1, a2, a3, t4, a5, a6);
        }
    }
    return syscall(n, a1, a2, a3, a4, a5, a6);
}

/* ---- job-system inline fix: report a single CPU ----
 * Unity sizes its job-worker pool as (num_cpus - 1).  On Android the workers are
 * driven by a Java/looper that doesn't exist under our loader, so jobs dispatched
 * to workers never run -> the main thread + workers all futex-wait forever (the
 * deadlock in every thread dump).  If sched_getaffinity reports ONLY CPU 0, Unity
 * creates 0 workers and runs jobs INLINE on the main thread -> no deadlock.  This
 * is Terraria's TER_JOBWORKERS0 / my_sched_getaffinity fix. */
int kv_sched_getaffinity(int pid, size_t setsize, void *mask) {
    (void)pid;
    if (mask && setsize >= sizeof(unsigned long)) {
        memset(mask, 0, setsize);
        *(unsigned long *)mask = 1UL;   /* only CPU 0 */
        return 0;
    }
    return -1;
}
int kv_sched_setaffinity(int pid, size_t setsize, const void *mask) {
    (void)pid; (void)setsize; (void)mask; return 0;   /* no-op: keep 1 CPU */
}
/* sysconf: report 1 CPU for the processor-count queries Unity uses to size
 * workers (_SC_NPROCESSORS_ONLN=0x62, _SC_NPROCESSORS_CONF=0x61). */
long kv_sysconf(int name) {
    static int once; if (!once && (once = 1))
        printf("[kv_sysconf] routed! name=%d tid=%ld\n", name, (long)syscall(178));
    if (name == 0x62 || name == 0x61) return 1;   /* 1 CPU -> 0 job workers */
    if (name == 0x27) return 4096;                 /* _SC_PAGESIZE */
    return sysconf(name);
}

 /* bionic-only symbol glibc lacks.  Called when bionic aborts; log it. */
void android_set_abort_message(const char *m) {
  fprintf(stderr, "[abort] %s\n", m ? m : "");
}

/* Forward decls for engine-abort/exit overrides - implemented in glibc_shims.c.
 * Routing the engine's abort/raise/tgkill/exit GOT entries through these
 * functions converts a silent exit-139 into a [loader] === ENGINE ... === log
 * line + caller address (otherwise no handler fires). */
void kv_engine_abort(void);
int kv_engine_raise(int sig);
int kv_engine_tgkill(int tgid, int tid, int sig);
void kv_engine_exit(int code);
void kv_stack_chk_fail(void);

/* ---- route table: .so imports that must bind to this bridge ---- */
void *kv_bionic_route(const char *name) {
  static const struct { const char *n; void *f; } m[] = {
    {"sigemptyset", kv_sigemptyset}, {"sigfillset", kv_sigfillset},
    {"sigaddset", kv_sigaddset}, {"sigdelset", kv_sigdelset},
    {"sigsuspend", kv_sigsuspend}, {"sigaction", kv_sigaction},
    {"pthread_sigmask", kv_pthread_sigmask},
    {"pthread_mutex_init", kv_pthread_mutex_init}, {"pthread_mutex_destroy", kv_pthread_mutex_destroy},
    {"pthread_mutex_lock", kv_pthread_mutex_lock}, {"pthread_mutex_unlock", kv_pthread_mutex_unlock},
    {"pthread_mutex_trylock", kv_pthread_mutex_trylock},
    {"pthread_cond_init", kv_pthread_cond_init}, {"pthread_cond_destroy", kv_pthread_cond_destroy},
    {"pthread_cond_signal", kv_pthread_cond_signal}, {"pthread_cond_broadcast", kv_pthread_cond_broadcast},
    {"pthread_cond_wait", kv_pthread_cond_wait}, {"pthread_cond_timedwait", kv_pthread_cond_timedwait},
    {"sem_init", kv_sem_init}, {"sem_destroy", kv_sem_destroy},
    {"sem_post", kv_sem_post}, {"sem_wait", kv_sem_wait}, {"sem_trywait", kv_sem_trywait},
    {"sem_getvalue", kv_sem_getvalue}, {"sem_timedwait", kv_sem_timedwait},
    {"pthread_rwlock_init", kv_pthread_rwlock_init}, {"pthread_rwlock_destroy", kv_pthread_rwlock_destroy},
    {"pthread_rwlock_rdlock", kv_pthread_rwlock_rdlock}, {"pthread_rwlock_wrlock", kv_pthread_rwlock_wrlock},
    {"pthread_rwlock_unlock", kv_pthread_rwlock_unlock},
    {"pthread_detach", kv_pthread_detach}, {"pthread_join", kv_pthread_join},
    {"pthread_self", kv_pthread_self}, {"pthread_create", kv_pthread_create},
    {"pthread_attr_init", kv_pthread_attr_init}, {"pthread_attr_destroy", kv_pthread_attr_destroy},
    {"pthread_attr_setdetachstate", kv_pthread_attr_setdetachstate},
    {"pthread_attr_setstacksize", kv_pthread_attr_setstacksize},
    {"pthread_attr_setschedparam", kv_pthread_attr_setschedparam},
    {"pthread_setschedparam", kv_pthread_setschedparam},
    {"pthread_setname_np", kv_pthread_setname_np},
    {"pthread_getattr_np", kv_pthread_getattr_np}, {"pthread_attr_getstack", kv_pthread_attr_getstack},
    {"pthread_key_create", kv_pthread_key_create}, {"pthread_key_delete", kv_pthread_key_delete},
    {"pthread_getspecific", kv_pthread_getspecific}, {"pthread_setspecific", kv_pthread_setspecific},
    {"pthread_once", kv_pthread_once}, {"pthread_equal", kv_pthread_equal},
    {"pthread_kill", kv_pthread_kill}, {"pthread_atfork", kv_pthread_atfork},
    {"pthread_condattr_init", kv_pthread_condattr_init}, {"pthread_condattr_destroy", kv_pthread_condattr_destroy},
    {"pthread_condattr_setclock", kv_pthread_condattr_setclock},
    {"pthread_mutexattr_init", kv_pthread_mutexattr_init}, {"pthread_mutexattr_destroy", kv_pthread_mutexattr_destroy},
    {"pthread_mutexattr_settype", kv_pthread_mutexattr_settype},
    {"sched_getaffinity", kv_sched_getaffinity}, {"sched_setaffinity", kv_sched_setaffinity},
    {"sysconf", kv_sysconf},
    {"syscall", kv_syscall},
    {"statfs", kv_statfs}, {"statfs64", kv_statfs},
    {"android_set_abort_message", android_set_abort_message},
    /* sigaltstack override - Unity calls real sigaltstack during its init to
     * install ITS OWN alt stack, replacing ours (which is sized for our
     * crash handler).  If their (typically smaller) alt stack is then used
     * when our handler fires, the handler push-overflow-re-faults and the
     * kernel runs default SIGSEGV -> silent exit 139.  We no-op Unity's
     * sigaltstack (pretend success) so OUR 256KB alt stack stays installed. */
    {"sigaltstack", kv_sigaltstack_noop},
    /* abort/raise/tgkill/exit/setjmp overrrides - terraria-nextos & horizonchase
     * BOTH route these to logging wrappers.  Without the overrides, Unity's
     * engine calls libc abort()/raise(SIGABRT)/tgkill(SIGABRT)/exit() when it
     * detects an internal error, which terminates the process through a path
     * our crash handler doesn't catch (abort() raises SIGABRT but glibc may
     * block handler installation via the legacy signal() path BEFORE our
     * sigaction; OR Unity calls _exit() directly, bypassing signals entirely).
     * Result: silent exit 139 with no [loader] CRASH line.  By routing these
     * GOT entries to our wrappers we (a) log the caller, (b) optionally
     * proceed instead of dying. */
    {"abort",  (void *)kv_engine_abort},
    {"raise",  (void *)kv_engine_raise},
    {"tgkill", (void *)kv_engine_tgkill},
    {"exit",   (void *)kv_engine_exit},
    {"_exit",  (void *)kv_engine_exit},
    {"__stack_chk_fail", (void *)kv_stack_chk_fail},
    {0, 0}
  };
  for (int i = 0; m[i].n; i++) if (strcmp(m[i].n, name) == 0) return m[i].f;
  return NULL;
}
