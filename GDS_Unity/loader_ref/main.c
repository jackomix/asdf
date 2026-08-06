/*
 * main.c -- native Game Dev Story bootstrap for NextOS.
 *
 * There is no Android application or emulator in this path.  We load the
 * original arm64 Unity objects, run their real init arrays/JNI_OnLoad, then
 * drive Unity's native surface and render lifecycle directly.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include "musl_compat.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <sys/stat.h>
#include <link.h>
#include <signal.h>
#include <sys/syscall.h>
#include <pthread.h>
#include <ucontext.h>
#include <sys/mman.h>

#include "nx_elf.h"
#include "gds.h"

char gds_gamedir[1024];
char gds_datadir[1024];
char gds_apk[1024];
char gds_home[1024];
long gds_max_frames = 0;
int gds_trace_gl = 0;
int gds_capture_mode = 0;

/* Android arm64 code reads the stack guard directly from TPIDR_EL0+0x28.
 * Under glibc that address can belong to another module's mutable TLS and a
 * perfectly valid Unity frame then calls __stack_chk_fail.  Keep this as the
 * first initialized TLS object in link order: glibc places the executable's
 * first TLS block immediately after its 16-byte TCB, so this stable pad covers
 * the complete Bionic guard slot on every thread.  This is the same audited
 * layout used by the proven Horizon Chase multi-firmware runtime. */
__attribute__((aligned(16), used))
_Thread_local char g_bionic_guard_pad[256] = { 1 };

/* Game Dev Story 1.18.1 is a normal, unprotected Unity IL2CPP build.  Keep the
 * exact NativeLoader order and do not introduce a synthetic bootstrap. */
static const struct {
    const char *file, *soname;
    int required, capture_only;
} LIBS[] = {
    { "libmain.so",       "libmain.so",       1, 0 },
    { "libunity.so",      "libunity.so",      1, 0 },
    { "libil2cpp.so",     "libil2cpp.so",     1, 0 },
};

extern const nx_import *gds_pthread_table(size_t *n);
extern const nx_import *gds_android_table(size_t *n);
extern const nx_import *gds_egl_table(size_t *n);

/* One combined, sorted import table: bionic + pthread bridge + libandroid +
 * EGL.  nx_resolve_import binary-searches it. */
static nx_import *all;
static size_t all_n;

static int imp_cmp(const void *a, const void *b)
{
    return strcmp(((const nx_import *)a)->name, ((const nx_import *)b)->name);
}

static void build_imports(void)
{
    size_t np, na, ne;
    const nx_import *p = gds_pthread_table(&np);
    const nx_import *an = gds_android_table(&na);
    const nx_import *eg = gds_egl_table(&ne);

    size_t bn;
    extern nx_import *gds_bionic_entries(size_t *n);
    nx_import *be = gds_bionic_entries(&bn);
    all = calloc(bn + np + na + ne + 8, sizeof *all);
    all_n = 0;
    for (size_t i = 0; i < bn; i++)
        all[all_n++] = be[i];
    for (size_t i = 0; i < np; i++)
        all[all_n++] = p[i];
    for (size_t i = 0; i < na; i++)
        all[all_n++] = an[i];
    for (size_t i = 0; i < ne; i++)
        all[all_n++] = eg[i];
    qsort(all, all_n, sizeof *all, imp_cmp);
    nx_set_imports(all, all_n);
    nx_log("import table: %zu entries (bionic %zu, pthread %zu, android %zu, egl %zu)",
           all_n, bn, np, na, ne);
}

/* Report the modules mapped by the native loader. */
/* ------------------------------------------------------------- frame watchdog */

static volatile unsigned long watchdog_frame;
static pid_t watchdog_tid;
static int watchdog_seconds;

void gds_watchdog_frame(void) { watchdog_frame++; }

static void *watchdog_thread(void *arg)
{
    (void)arg;
    unsigned long last = watchdog_frame;
    for (;;) {
        struct timespec t = { watchdog_seconds, 0 };
        nanosleep(&t, NULL);
        if (watchdog_frame != last) {
            last = watchdog_frame;
            continue;
        }
        fprintf(stderr,
                "[gds] watchdog: frame %lu has not returned in %ds; faulting "
                "the render thread so its stack is reported\n",
                last, watchdog_seconds);
        /* Deliver to the render thread specifically, not to the process: any
         * other thread would report a stack we already know is idle. */
        syscall(SYS_tgkill, getpid(), watchdog_tid, SIGSEGV);
        return NULL;
    }
}

void gds_arm_frame_watchdog(void)
{
    const char *v = getenv("GDS_WATCHDOG");
    /* 0.61: default ON (30s).  A hung first frame used to leave a black screen
     * forever; the watchdog faults the render thread so the log shows where it
     * is stuck.  GDS_WATCHDOG=0 disables, or names another timeout. */
    watchdog_seconds = v && *v ? atoi(v) : 30;
    if (watchdog_seconds <= 0)
        return;
    watchdog_tid = (pid_t)syscall(SYS_gettid);
    pthread_t th;
    if (pthread_create(&th, NULL, watchdog_thread, NULL) != 0) {
        nx_log("watchdog: cannot start thread");
        return;
    }
    pthread_detach(th);
    nx_log("watchdog armed: %ds without a frame faults tid %d",
           watchdog_seconds, (int)watchdog_tid);
}

int gds_iterate_mods(int (*cb)(void *, size_t, void *), void *data)
{
    for (size_t i = 0; i < sizeof LIBS / sizeof *LIBS; i++) {
        nx_mod *m = nx_find_mod(LIBS[i].soname);
        if (!m)
            continue;
        struct dl_phdr_info info;
        memset(&info, 0, sizeof info);
        info.dlpi_addr = (ElfW(Addr))m->base;
        info.dlpi_name = m->name;
        info.dlpi_phdr = (const ElfW(Phdr) *)m->phdr;
        info.dlpi_phnum = (ElfW(Half))m->phnum;
        int r = cb(&info, sizeof info, data);
        if (r)
            return r;
    }
    return 0;
}

/* Which mapped module contains an address, for dladdr. */
const char *gds_mod_at(const void *addr, void **base_out)
{
    const uint8_t *p = addr;
    for (size_t i = 0; i < sizeof LIBS / sizeof *LIBS; i++) {
        nx_mod *m = nx_find_mod(LIBS[i].soname);
        if (!m)
            continue;
        if (p >= m->base && p < m->base + m->span) {
            if (base_out)
                *base_out = m->base;
            return m->name;
        }
    }
    return NULL;
}

static void read_env(void)
{
    const char *v;
    nx_verbose   = (v = getenv("GDS_VERBOSE")) && *v != '0';
    /* 0.61: game logcat defaults ON (GDS_LOGCAT=0 turns it off).  A trap with
     * logcat muted hid Unity's own fatal message. */
    gds_log_level = (v = getenv("GDS_LOGCAT")) ? (*v != '0') : 1;
    gds_trace_jni = (v = getenv("GDS_JNILOG")) && *v != '0';
    gds_trace_gl  = (v = getenv("GDS_GLLOG")) && *v != '0';
    if ((v = getenv("GDS_FRAMES")))
        gds_max_frames = strtol(v, NULL, 10);
}

static void copy_path(char *out, size_t capacity, const char *value,
                      const char *description)
{
    size_t length = strlen(value);
    if (length >= capacity)
        nx_die("%s path is too long", description);
    memcpy(out, value, length + 1);
}

static void join_path(char *out, size_t capacity, const char *base,
                      const char *first, const char *second)
{
    int written;
    if (second)
        written = snprintf(out, capacity, "%s/%s/%s", base, first, second);
    else
        written = snprintf(out, capacity, "%s/%s", base, first);
    if (written < 0 || (size_t)written >= capacity)
        nx_die("game path is too long");
}

static void setup_paths(const char *arg)
{
    if (arg && *arg)
        copy_path(gds_gamedir, sizeof gds_gamedir, arg, "game directory");
    else if (!getcwd(gds_gamedir, sizeof gds_gamedir))
        copy_path(gds_gamedir, sizeof gds_gamedir, ".", "game directory");
    join_path(gds_datadir, sizeof gds_datadir, gds_gamedir, "data", NULL);
    join_path(gds_apk, sizeof gds_apk, gds_gamedir, "data", NULL);
    join_path(gds_home, sizeof gds_home, gds_gamedir, "home", NULL);
    mkdir(gds_home, 0755);
}

int gds_load_modules(void)
{
    char path[1200];
    for (size_t i = 0; i < sizeof LIBS / sizeof *LIBS; i++) {
        if (LIBS[i].capture_only && !gds_capture_mode)
            continue;
        snprintf(path, sizeof path, "%s/%s", gds_gamedir, LIBS[i].file);
        nx_mod *m = nx_load(path, LIBS[i].soname);
        if (!m) {
            if (LIBS[i].required)
                nx_die("cannot load %s (expected at %s)", LIBS[i].file, path);
            nx_log("optional %s missing", LIBS[i].file);
        }
    }
    /* Relocate in the same order; by the time libunity is relocated the other
     * modules can satisfy its cross-module imports. */
    int missing = 0;
    for (size_t i = 0; i < sizeof LIBS / sizeof *LIBS; i++) {
        if (LIBS[i].capture_only && !gds_capture_mode)
            continue;
        nx_mod *m = nx_find_mod(LIBS[i].soname);
        if (m)
            missing += nx_relocate(m);
    }
    return missing;
}

/* A fault inside a module we mapped ourselves has no symbols and no link map,
 * so the only way to place it is to print the PC against the module bases.
 * Always on: it costs nothing until something goes wrong. */
/* ---------------- one-shot brk probes (managed exception catcher) ---------
 * GDS_TRAP_AT=1 arms brk#0 patches over IApplication::Error and every
 * Kairosoft *Exception ctor in libil2cpp.so.  On SIGTRAP the handler dumps
 * the exception's klass name / message / KairoException.displayMessage_
 * (obj+0x90 per metadata) / inner-exception chain and the caller (x30),
 * restores the original instruction and resumes.  Device-safe: off unless
 * the env var is set. */
struct probe { uintptr_t addr; uint32_t orig; int hit; int cap; int kind; const char *tag; };
#define MAX_PROBES 192
static struct probe g_probes[MAX_PROBES];
static int g_nprobes;
static int g_in_dump;
static uintptr_t g_il2b;

static int trap_ptr_ok(uintptr_t p)
{
    return p > 0x10000 && p < 0x800000000000UL;
}

/* Mapping check for probe-time derefs.  Uses raw mincore(2): no fd needed,
 * so it cannot flake under fd pressure inside the signal handler, and it
 * always reflects the current address space (the IL2CPP GC heap grows
 * while the game boots). */
static int trap_mapped(uintptr_t a)
{
    unsigned char vec[1];
    if (!trap_ptr_ok(a))
        return 0;
    return syscall(232 /*SYS_mincore*/, (void *)(a & ~0xfffUL), 1, vec) == 0;
}

static const char *trap_cstr(uintptr_t va, char *buf, size_t cap)
{
    size_t n = 0;
    const char *s = (const char *)va;
    if (!trap_ptr_ok(va))
        return "?";
    if (!trap_mapped(va))
        return "<unmapped>";
    while (n + 1 < cap && n < 96 && s[n] >= 0x20 && (unsigned char)s[n] < 0x7f) {
        buf[n] = s[n];
        n++;
    }
    buf[n] = 0;
    return buf;
}

static void trap_il2str(uintptr_t str_va, char *out, size_t cap)
{
    /* Il2CppString: +0x10 int32 length, +0x14 utf16 chars */
    size_t n = 0;
    if (!trap_ptr_ok(str_va)) { snprintf(out, cap, "(null)"); return; }
    if (!trap_mapped(str_va)) { snprintf(out, cap, "<unmapped>"); return; }
    int len = *(volatile int *)(str_va + 0x10);
    if (len < 0 || len > 512) { snprintf(out, cap, "(badlen %d)", len); return; }
    const uint16_t *c = (const uint16_t *)(str_va + 0x14);
    for (int i = 0; i < len && n + 2 < cap; i++) {
        uint16_t ch = c[i];
        out[n++] = (ch >= 0x20 && ch < 0x7f) ? (char)ch : '.';
    }
    out[n] = 0;
}

/* fully-guarded one-line object brief: value, klass, ns.name, +0x10 string */
static void trap_brief(uintptr_t obj, const char *what)
{
    char b1[96], b2[96], s[600];
    fprintf(stderr, "[trap]   %s=%#lx", what, (unsigned long)obj);
    if (!trap_ptr_ok(obj) || !trap_mapped(obj)) { fprintf(stderr, " (bad)\n"); return; }
    uintptr_t k = *(uintptr_t *)obj;
    if (!trap_ptr_ok(k) || !trap_mapped(k)) {
        fprintf(stderr, " klass=(bad %#lx)\n", (unsigned long)k);
        return;
    }
    fprintf(stderr, " %s.%s", trap_cstr(*(uintptr_t *)(k + 0x18), b2, sizeof b2),
            trap_cstr(*(uintptr_t *)(k + 0x10), b1, sizeof b1));
    trap_il2str(*(uintptr_t *)(obj + 0x10), s, sizeof s);
    fprintf(stderr, " msg='%s'\n", s);
}

static void trap_dump_exc(uintptr_t obj, uintptr_t x30, const char *what)
{    char nm[128], ns[128], msg[600];
    uintptr_t klass = trap_ptr_ok(obj) ? *(uintptr_t *)obj : 0;
    if (!trap_ptr_ok(klass))
        return;
    fprintf(stderr, "[trap] %s=%#lx class=%s.%s caller=il2cpp+%#lx\n",
            what, (unsigned long)obj,
            trap_cstr(*(uintptr_t *)(klass + 0x18), ns, sizeof ns),
            trap_cstr(*(uintptr_t *)(klass + 0x10), nm, sizeof nm),
            (unsigned long)(x30 - g_il2b));
    trap_il2str(*(uintptr_t *)(obj + 0x10), msg, sizeof msg);
    fprintf(stderr, "[trap]   message        : '%s'\n", msg);
    trap_il2str(*(uintptr_t *)(obj + 0x90), msg, sizeof msg);
    fprintf(stderr, "[trap]   displayMessage_: '%s'\n", msg);
    uintptr_t inner = *(uintptr_t *)(obj + 0x20);
    for (int d = 0; d < 4 && trap_ptr_ok(inner); d++) {
        uintptr_t ik = *(uintptr_t *)inner;
        if (!trap_ptr_ok(ik))
            break;
        trap_il2str(*(uintptr_t *)(inner + 0x10), msg, sizeof msg);
        fprintf(stderr, "[trap]   inner[%d] %s.%s: '%s'\n", d,
                trap_cstr(*(uintptr_t *)(ik + 0x18), ns, sizeof ns),
                trap_cstr(*(uintptr_t *)(ik + 0x10), nm, sizeof nm), msg);
        inner = *(uintptr_t *)(inner + 0x20);
    }
}

/* ask the il2cpp runtime itself which images resolve form.SpForm */
static void trap_spform_lookup(void)
{
    nx_mod *il2cpp = nx_find_mod("libil2cpp.so");
    if (!il2cpp)
        return;
    void *(*domain_get)(void) = nx_lookup_in(il2cpp, "il2cpp_domain_get");
    void **(*get_assemblies)(void *, size_t *) =
        nx_lookup_in(il2cpp, "il2cpp_domain_get_assemblies");
    void *(*assembly_get_image)(void *) =
        nx_lookup_in(il2cpp, "il2cpp_assembly_get_image");
    void *(*class_from_name)(void *, const char *, const char *) =
        nx_lookup_in(il2cpp, "il2cpp_class_from_name");
    const char *(*image_get_name)(void *) =
        nx_lookup_in(il2cpp, "il2cpp_image_get_name");
    if (!image_get_name)
        image_get_name = nx_lookup_in(il2cpp, "il2cpp_image_get_filename");
    if (!domain_get || !get_assemblies || !assembly_get_image ||
        !class_from_name) {
        fprintf(stderr, "[trap]   spform lookup: exports missing\n");
        return;
    }
    size_t na = 0;
    void **asms = get_assemblies(domain_get(), &na);
    int found = 0;
    for (size_t i = 0; i < na && asms; i++) {
        void *img = assembly_get_image(asms[i]);
        void *cls = img ? class_from_name(img, "form", "SpForm") : 0;
        if (cls) {
            found = 1;
            fprintf(stderr, "[trap]   SpForm FOUND asm[%zu] img=%p name=%s cls=%p\n",
                    i, img, image_get_name ? image_get_name(img) : "?",
                    (void *)cls);
        }
    }
    if (!found)
        fprintf(stderr, "[trap]   SpForm NOT FOUND in any of %zu assemblies\n", na);
}

static void arm_traps(uintptr_t il2b)
{
    static const struct { uint32_t va; const char *tag; int kind; } T[] = {
        {0x1757230, "IApplication::Error",        1},
        {0x17d06f8, "KairoException.ctorA",       2},
        {0x17d0764, "KairoException.ctorB",       2},
        {0x17d4108, "NotPermitException.ctorA",   2},
        {0x17d413c, "NotPermitException.ctorB",   2},
        {0x17ce940, "SignInException.ctorA",      2},
        {0x17ce948, "SignInException.ctorB",      2},
        {0x17ce950, "IllegalFileException.ctorA", 2},
        {0x17ce958, "IllegalFileException.ctorB", 2},
        {0x17cee78, "JarFormatException.ctor",    2},
        {0x179aff8, "UIException.ctor",           2},
        {0x1866584, "ConnectionException.ctor",   2},
        {0x1856bec, "IllegalPurchaseException.ctor", 2},
        {0x17bd558, "java.io.EOFException.ctor",  2},
        /* boot-gate / dialog-source candidates (kind 3: tag + caller) */
        {0x17fb140, "KairoPlugin.ShowDialog",     3},
        {0x17549a0, "IApplication.BootError",     3},
        {0x175be48, "IApplication.OnNotPermit",   3},
        {0x175bfbc, "IApplication.ShowPermissionError", 3},
        {0x175bc04, "IApplication.OnPermit",      3},
        {0x175c0cc, "IApplication.RequestPermission", 3},
        {0x17565b8, "IApplication.TerminateCheck", 5},
        {0x1759dc4, "IApplication.GooglePlayLicenseCheck", 3},
        {0x175c734, "IApplication.DoProcOnSrv",   3},
        {0x175a968, "IApplication.Terminate",     3},
        {0x1842a00, "KairoBase.OnStart",          3},
        {0x183fa4c, "KairoBase.Start",            3},
        /* termination-dialog + form-flow resolution */
        {0x17b6120, "FormManagerBase.DoTerminationDialog", 2},
        {0x17b7918, "FormManagerBase.ShowTerminationDialog", 2},
        {0x17b7930, "FormManagerBase.CheckShowTerminationDialog", 3},
        {0x174b6b0, "ui.Dialog.ShowSelection",    2},
        {0x17b34c4, "FormManagerBase.Push(A)",    4},
        {0x17b34e4, "FormManagerBase.Push(B)",    4},
        {0x17b1224, "FormManagerBase.Push(C)",    4},
        {0x17569bc, "GetEntryAssembly.result",    8},
        {0x17b18a0, "FormManagerBase.GetFormsNum", 3},
        /* boot-chain: does the app ever boot? */
        {0x1754ae4, "IApplication.Start",         3},
        {0x1719d38, "ApplicationManager.Boot(A)", 2},
        {0x1719da0, "ApplicationManager.Boot(B)", 2},
        {0x171a2dc, "ApplicationManager.BeginContainerPlugin", 2},
        {0x171a65c, "ApplicationManager.GetRootPlugin", 4},
        {0x171a04c, "ApplicationManager.ReadSystemRecord", 3},
        {0x1719ed4, "ApplicationManager.GetGamePlayData", 3},
        {0x1755a0c, "Update.Push.site",           4},
        {0x175543c, "IApplication.Update",        3},
        {0x1755960, "Update.postGetType",         3},
        {0x1756210, "Update.assembly-null-exit",  3},
        {0x17560fc, "Update.entrycheck-skip",     3},
        /* mscorlib: confirm GetType args + catch the thrown exception type */
        {0x1914cb8, "Assembly.GetType",         2},
        {0x1918094, "RuntimeAssembly.GetType",    2},
        {0x19e5048, "TypeLoadException.ctor1",    2},
        {0x19e9044, "TypeLoadException.ctor2",    2},
        {0x19ea798, "TypeLoadException.ctor3",    2},
        {0x19ea7e0, "TypeLoadException.ctor4",    2},
        {0x19ea838, "TypeLoadException.ctor5",    2},
        {0x192170c, "FileNotFoundException.ctor1",2},
        {0x1918984, "FileNotFoundException.ctor2",2},
        {0x1921768, "FileNotFoundException.ctor3",2},
        {0x1921a20, "FileNotFoundException.ctor4",2},
        /* service boot chain: is the game's KairoService ever created/started? */
        {0xdb1594, "KairoService..ctor",        3},
        {0xdb19f8, "KairoService.GetInstance",  3},
        {0xdb1ae0, "KairoService.OnInit",       3},
        {0xdb1b74, "KairoService.OnStart",      3},
        {0xe86b38, "form.FormManager..ctor",    3},
        {0xe73e38, "form.FormManager.GetInstance", 3},
        /* main game class lifecycle (virtual-dispatch only) */
        {0xe806d0, "main.Main..ctor",           3},
        {0xe80808, "main.Main.OnCreate",        3},
        {0xe80e40, "main.Main.OnUpdate",        3},
        {0xe81708, "main.Main.OnDraw",          3},
        /* IApplication::Start's virtual dispatch site (blr x9, vtable+0x198) */
        {0x1754eb4, "Start.blrsite",            9},
        /* main.Main::OnUpdate catch: what is actually thrown? */
        {0xe81130, "Main.catch.entry",         10},
        {0xe81260, "Main.catch.pathB-NRE",     11},
        /* OnUpdate try-body stage markers: last one before the catch = source */
        {0xe80ed8, "Main.stage.AppDataGetInstance", 3},
        {0xe80ef8, "Main.armThrowLate",       16},
        {0xe80f38, "Main.stage.FMExecute",     3},
        {0xe80f5c, "Main.stage.jingleBlr",     3},
        {0xe80fac, "Main.stage.SetJingle",     3},
        {0xe8100c, "Main.stage.CheckKeyState", 3},
        {0xe8108c, "Main.stage.setVsync",      3},
        {0xe81098, "Main.stage.GetTargetFps",  3},
        {0xe810c0, "Main.stage.setTargetFps",  3},
        /* native throw entry: fp-walk gives the null-check raise site */
        {0x1894cdc, "Substring.entry",         15},
        {0x17b4160, "ex.qA",                   3},
        {0x18574a0, "NM.ctor.PlayerPrefs",     3},
        {0x18574e0, "NM.ctor.Split",           3},
        {0x1857568, "NM.ctor.ListAdd",         3},
        {0x1857598, "NM.ctor.GetFilter",       3},
        {0x17fdf94, "GetFilter.runner-null-check", 17},
        /* KairoPlugin::Init chain: does Awake call it, does its store run? */
        {0x1752c98, "Awake.callInit",          3},
        {0x17f68d4, "KairoPlugin.Init.entry",  3},
        {0x17f6a34, "KairoPlugin.Init.storeStatics0", 18},
        {0xcb0de4, "raiseNRE.stub",            19},
        {0x186a6c0, "GetNumRecords.files",     20},
        {0xe76ab8, "AppData.Init.entry",       3},
        {0x186a868, "RecordStore.Setup.entry", 22},
        {0x186d61c, "Storage.Open.entry",      24},
        {0x186a8d0, "Setup.postOpen",          3},
        {0x186a8e4, "RecordStore.Setup.store", 21},
        {0xcb0ddc, "raiseAORE.stub",          19},
        /* Storage::GetFolder folder in {1,4}, Android branch: the path is
         * cut out of the base path via LastIndexOf(strA)+5..LastIndexOf(strB)
         * then Substring(start,len) -- AORE if either index is -1. */
        {0x186d07c, "GetFolder.basepath",      25},
        {0x186d0c0, "GetFolder.idx1",          26},
        {0x186d0dc, "GetFolder.idx2",          27},
        {0x186d0ec, "GetFolder.substring",     28},
        {0x186d214, "GetFolder.result",        29},
        {0xd952b0, "cxa_throw",               13},
    };
    if (!getenv("GDS_TRAP_AT"))
        return;
    g_il2b = il2b;
    for (size_t i = 0; i < sizeof T / sizeof *T && g_nprobes < MAX_PROBES; i++) {
        if (T[i].kind == 13)
            continue;   /* late-armed by the kind-16 marker (skip AORE storm) */
        uintptr_t a = il2b + T[i].va;
        uintptr_t pg = a & ~0xfffUL;
        if (mprotect((void *)pg, 0x1000, PROT_READ | PROT_WRITE | PROT_EXEC)) {
            fprintf(stderr, "[trap] arm %s: mprotect fail\n", T[i].tag);
            continue;
        }
        g_probes[g_nprobes].addr = a;
        g_probes[g_nprobes].orig = *(uint32_t *)a;
        g_probes[g_nprobes].hit = 0;
        g_probes[g_nprobes].cap = T[i].kind == 13 ? 40 : (T[i].kind == 24 ? 12 : ((T[i].kind >= 25 && T[i].kind <= 29) ? 6 : ((T[i].kind == 14 || T[i].kind == 15) ? 6 : 1)));
        g_probes[g_nprobes].kind = T[i].kind;
        g_probes[g_nprobes].tag = T[i].tag;
        *(uint32_t *)a = 0xd4200000; /* brk #0 */
        __builtin___clear_cache((char *)a, (char *)a + 4);
        mprotect((void *)pg, 0x1000, PROT_READ | PROT_EXEC);
        g_nprobes++;
        if (0) (void)0;
        { fprintf(stderr, "[trap] armed %s @ il2cpp+%#x\n", T[i].tag, T[i].va); }
    }
}


static void disarm_throw(void)
{
    for (int i = 0; i < g_nprobes; i++) {
        struct probe *p = &g_probes[i];
        if (p->kind != 13)
            continue;
        uintptr_t pg = p->addr & ~0xfffUL;
        if (mprotect((void *)pg, 0x1000, PROT_READ | PROT_WRITE | PROT_EXEC) == 0) {
            *(uint32_t *)p->addr = p->orig;
            __builtin___clear_cache((char *)p->addr, (char *)p->addr + 4);
            mprotect((void *)pg, 0x1000, PROT_READ | PROT_EXEC);
        }
        p->hit = 1;
        p->cap = 1;   /* dead */
        fprintf(stderr, "[trap] window closed: cxa_throw disarmed\n");
    }
}

static void arm_late_throw(uintptr_t va)
{
    if (g_nprobes >= MAX_PROBES) {
        fprintf(stderr, "[trap] !! arm_late_throw: MAX_PROBES full\n");
        return;
    }
    uintptr_t a = g_il2b + va;
    for (int i = 0; i < g_nprobes; i++)
        if (g_probes[i].addr == a) {
            if (g_probes[i].hit >= g_probes[i].cap && g_probes[i].cap) {
                /* previously disarmed: re-arm */
                uintptr_t pg = a & ~0xfffUL;
                if (mprotect((void *)pg, 0x1000, PROT_READ | PROT_WRITE | PROT_EXEC) == 0) {
                    g_probes[i].hit = 0;
                    g_probes[i].cap = 200;
                    *(uint32_t *)a = 0xd4200000;
                    __builtin___clear_cache((char *)a, (char *)a + 4);
                    mprotect((void *)pg, 0x1000, PROT_READ | PROT_EXEC);
                    fprintf(stderr, "[trap] cxa_throw re-armed\n");
                }
            }
            return;   /* already armed */
        }
    uintptr_t pg = a & ~0xfffUL;
    if (mprotect((void *)pg, 0x1000, PROT_READ | PROT_WRITE | PROT_EXEC))
        return;
    g_probes[g_nprobes].addr = a;
    g_probes[g_nprobes].orig = *(uint32_t *)a;
    g_probes[g_nprobes].hit = 0;
    g_probes[g_nprobes].cap = 200;
    g_probes[g_nprobes].kind = 13;
    g_probes[g_nprobes].tag = "cxa_throw(late)";
    *(uint32_t *)a = 0xd4200000;
    __builtin___clear_cache((char *)a, (char *)a + 4);
    mprotect((void *)pg, 0x1000, PROT_READ | PROT_EXEC);
    g_nprobes++;
    fprintf(stderr, "[trap] late-armed cxa_throw\n");
}

static void on_fault(int sig, siginfo_t *si, void *uc)
{
    ucontext_t *u = uc;
    unsigned long pc = (unsigned long)u->uc_mcontext.pc;
    if (g_in_dump) {
        write(2, "[trap] nested fault in dump\n", 28);
        _exit(3);
    }
    /* one-shot brk probes: identify, dump, restore, resume */
    if (sig == SIGTRAP && g_nprobes) {
        for (int i = 0; i < g_nprobes; i++) {
            struct probe *p = &g_probes[i];
            if ((p->cap ? (p->hit >= p->cap) : p->hit) || (uintptr_t)pc != p->addr)
                continue;
            p->hit++;
            if (p->kind == 98) {
                /* transient step-over: re-arm the parent probe at addr-4,
                 * then take the generic one-shot path for ourselves below */
                uintptr_t t = p->addr - 4;
                uintptr_t tpg = t & ~0xfffUL;
                if (mprotect((void *)tpg, 0x1000, PROT_READ | PROT_WRITE | PROT_EXEC) == 0) {
                    *(uint32_t *)t = 0xd4200000;
                    __builtin___clear_cache((char *)t, (char *)t + 4);
                    mprotect((void *)tpg, 0x1000, PROT_READ | PROT_EXEC);
                }
            }
            g_in_dump = 1;
            uintptr_t x30 = (uintptr_t)u->uc_mcontext.regs[30];
            if (p->kind != 13)
                fprintf(stderr, "\n[trap] hit %s @ il2cpp+%#lx caller=il2cpp+%#lx\n",
                        p->tag, (unsigned long)(p->addr - g_il2b),
                        (unsigned long)(x30 >= g_il2b ? x30 - g_il2b : x30));
            if (p->addr == g_il2b + 0x1914cb8) {
                uintptr_t k2 = trap_ptr_ok((uintptr_t)u->uc_mcontext.regs[0])
                    ? *(uintptr_t *)u->uc_mcontext.regs[0] : 0;
                if (trap_ptr_ok(k2) && trap_mapped(k2))
                    fprintf(stderr, "[trap]   impl vtable+0x2b8=%#lx (il2cpp+%#lx) +0x2c0=%#lx\n",
                            *(volatile uintptr_t *)(k2 + 0x2b8),
                            *(volatile uintptr_t *)(k2 + 0x2b8) - g_il2b,
                            *(volatile uintptr_t *)(k2 + 0x2c0));
            }
            if (p->kind == 1) {
                /* Error(this, ex): x1 is the exception about to be shown */
                trap_dump_exc((uintptr_t)u->uc_mcontext.regs[1], x30, "ex");
            } else if (p->kind == 2) {
                /* ctor / static entry: x0 = object, x1..x3 = string args */
                trap_dump_exc((uintptr_t)u->uc_mcontext.regs[0], x30, "obj");
                for (int r = 1; r <= 3; r++) {
                    char b[600];
                    trap_il2str((uintptr_t)u->uc_mcontext.regs[r], b, sizeof b);
                    fprintf(stderr, "[trap]   x%d='%s'\n", r, b);
                }
            } else if (p->kind == 5) {
                /* TerminateCheck decision state */
                uintptr_t th = (uintptr_t)u->uc_mcontext.regs[0];
                char nm2[128], ns2[128];
                fprintf(stderr, "[trap]   flag=x1=%ld this=%#lx\n",
                        (long)u->uc_mcontext.regs[1], (unsigned long)th);
                if (trap_ptr_ok(th)) {
                    fprintf(stderr, "[trap]   app.bootStageSnapshot(0x58)=%d countdown(0x140)=%d ts(0xb0)=%ld\n",
                            *(volatile int *)(th + 0x58),
                            *(volatile int *)(th + 0x140),
                            *(volatile long *)(th + 0xb0));
                }
                /* entry-class name GetType(name) is called with */
                {
                    char es[600];
                    es[0] = 0;
                    uintptr_t sp2 = *(uintptr_t *)(g_il2b + 0x1ecd798);
                    trap_il2str(trap_ptr_ok(sp2) ? *(uintptr_t *)sp2 : 0,
                                es, sizeof es);
                    fprintf(stderr, "[trap]   entry-name slot[0x1ecd798]='%s'\n", es);
                }
                unsigned soffs[3] = {0x1ebf338, 0x1ebf2c8, 0x1ebf2d8};
                for (int s = 0; s < 3; s++) {
                    uintptr_t slot = g_il2b + soffs[s];
                    if (!trap_ptr_ok(slot)) { continue; }
                    uintptr_t cls = *(uintptr_t *)slot;
                    fprintf(stderr, "[trap]   static[+%x] classptr=%#lx", soffs[s],
                            (unsigned long)cls);
                    if (!trap_ptr_ok(cls) || (cls & 7)) { fprintf(stderr, " (bad)\n"); continue; }
                    uintptr_t st = *(volatile uintptr_t *)(cls + 0xb8);
                    fprintf(stderr, " statics=%#lx", (unsigned long)st);
                    if (!trap_ptr_ok(st) || (st & 7) || st < 0x10000) { fprintf(stderr, " (bad)\n"); continue; }
                    fprintf(stderr, " f0x30=%d f0x50=%d f0x60=%d\n",
                            *(volatile unsigned char *)(st + 0x30),
                            *(volatile int *)(st + 0x50),
                            *(volatile unsigned char *)(st + 0x60));
                }
            } else if (p->kind == 8) {
                /* GetEntryAssembly result: which slot, which assembly image */
                uintptr_t res = (uintptr_t)u->uc_mcontext.regs[0];
                fprintf(stderr, "[trap]   chosen x8=%ld result=%#lx\n",
                        (long)u->uc_mcontext.regs[8], (unsigned long)res);
                uintptr_t asmp = trap_ptr_ok(res) ? *(uintptr_t *)(res + 0x10) : 0;
                fprintf(stderr, "[trap]   native asm=%#lx\n", (unsigned long)asmp);
                uintptr_t kla = trap_ptr_ok(res) ? *(uintptr_t *)res : 0;
                if (trap_ptr_ok(kla) && trap_mapped(kla)) {
                    fprintf(stderr, "[trap]   klass vtable+0x258=%#lx (il2cpp+%#lx) +0x260=%#lx\n",
                            *(volatile uintptr_t *)(kla + 0x258),
                            *(volatile uintptr_t *)(kla + 0x258) - g_il2b,
                            *(volatile uintptr_t *)(kla + 0x260));
                }
                trap_spform_lookup();
            } else if (p->kind == 9) {
                /* virtual dispatch site: dump target reg + this (x19) vtable */
                for (int r = 0; r <= 9; r++) {
                    if (r > 1 && r < 8) continue;   /* x0,x1,x8,x9 only */
                    uintptr_t v = (uintptr_t)u->uc_mcontext.regs[r];
                    fprintf(stderr, "[trap]   x%d=%#lx%s%#lx%s\n", r,
                            (unsigned long)v,
                            (v >= g_il2b && v < g_il2b + 0x2400000) ? " (il2cpp+" : "",
                            (v >= g_il2b && v < g_il2b + 0x2400000) ? (unsigned long)(v - g_il2b) : 0,
                            (v >= g_il2b && v < g_il2b + 0x2400000) ? ")" : "");
                }
                uintptr_t th9 = (uintptr_t)u->uc_mcontext.regs[19];
                if (trap_ptr_ok(th9)) {
                    uintptr_t k9 = *(uintptr_t *)th9;
                    char nm2[128], ns2[128];
                    const char *cname = "?", *nname = "?";
                    if (trap_ptr_ok(k9) && trap_mapped(k9)) {
                        nname = trap_cstr(*(uintptr_t *)(k9 + 0x10), nm2, sizeof nm2);
                        cname = trap_cstr(*(uintptr_t *)(k9 + 0x18), ns2, sizeof ns2);
                        fprintf(stderr, "[trap]   this(x19)=%#lx klass=%#lx %s.%s\n",
                                (unsigned long)th9, (unsigned long)k9, cname, nname);
                        for (uintptr_t sl = 0x180; sl <= 0x1b0; sl += 8) {
                            uintptr_t fn = *(volatile uintptr_t *)(k9 + sl);
                            fprintf(stderr, "[trap]     vtable+%#lx=%#lx (il2cpp+%#lx)\n",
                                    (unsigned long)sl, (unsigned long)fn,
                                    (fn >= g_il2b && fn < g_il2b + 0x2400000)
                                        ? (unsigned long)(fn - g_il2b) : 0);
                        }
                    }
                }
            } else if (p->kind == 13) {
                /* one line per throw: managed exception class + raise site.
                 * The raise thunk pushed its x30 just under the current sp;
                 * that value is the codegen raise call site (+4). */
                uintptr_t obj = (uintptr_t)u->uc_mcontext.regs[0];
                uintptr_t mex = (trap_ptr_ok(obj) && trap_mapped(obj))
                                ? *(uintptr_t *)obj : 0;
                const char *nm = "?", *ns = "?";
                char b1[96], b2[96];
                if (trap_ptr_ok(mex) && trap_mapped(mex)) {
                    uintptr_t k = *(uintptr_t *)mex;
                    if (trap_ptr_ok(k) && trap_mapped(k)) {
                        ns = trap_cstr(*(uintptr_t *)(k + 0x18), b2, sizeof b2);
                        nm = trap_cstr(*(uintptr_t *)(k + 0x10), b1, sizeof b1);
                    }
                }
                uintptr_t spv = (uintptr_t)u->uc_mcontext.sp;
                uintptr_t mra = (trap_ptr_ok(spv) && trap_mapped(spv))
                                ? *(uintptr_t *)spv : 0;
                if (!strstr(nm, "NullReference")) {
                    /* runtime AORE spam: log a few; the hit still counts and
                     * flow falls through to the step-over trampoline (an
                     * early return here re-trapped the same brk forever). */
                    static int g_skip_seen;
                    if (g_skip_seen < 4) {
                        g_skip_seen++;
                        fprintf(stderr,
                            "[trap]   throw-skip %s.%s ra=il2cpp+%#lx\n",
                            ns, nm,
                            (unsigned long)(trap_ptr_ok(mra) && mra >= g_il2b
                                            ? mra - g_il2b : mra));
                    }
                }
                fprintf(stderr,
                        "[trap]   NRE[%d] %s.%s ra=il2cpp+%#lx thunk=il2cpp+%#lx\n",
                        p->hit, ns, nm,
                        (unsigned long)(trap_ptr_ok(mra) && mra >= g_il2b ?
                                        mra - g_il2b : mra),
                        (unsigned long)(x30 >= g_il2b ? x30 - g_il2b : x30));
                if (trap_ptr_ok(spv) && trap_mapped(spv)) {
                    for (int q = 1; q < 8; q++) {
                        uintptr_t v = *(uintptr_t *)(spv + 8 * q);
                        if (!trap_ptr_ok(v))
                            continue;
                        const char *zone = v >= g_il2b && v < g_il2b + 0x2400000
                                           ? "il2cpp" : "";
                        fprintf(stderr, "[trap]     sp+%d=%#lx %s%#lx%s\n",
                                q * 8, (unsigned long)v, zone,
                                zone[0] ? (unsigned long)(v - g_il2b) : 0UL,
                                zone[0] ? ")" : "");
                    }
                }
            } else if (p->kind == 11) {
                /* managed exception at x29: dump cls + all fields +0x10..0x78;
                 * string-shaped values get text, array-shaped get elements */
                uintptr_t ex = (uintptr_t)u->uc_mcontext.regs[29];
                trap_brief(ex, "ex");
                if (trap_ptr_ok(ex) && trap_mapped(ex)) {
                    for (uintptr_t off = 0x10; off <= 0x78; off += 8) {
                        uintptr_t v = *(uintptr_t *)(ex + off);
                        char s[300];
                        fprintf(stderr, "[trap]   ex+%#lx = %#lx",
                                (unsigned long)off, (unsigned long)v);
                        if (trap_ptr_ok(v) && trap_mapped(v)) {
                            int len = *(volatile int *)(v + 0x10);
                            int alen = *(volatile int *)(v + 0x18);
                            if (len >= 0 && len <= 512) {
                                trap_il2str(v, s, sizeof s);
                                fprintf(stderr, "  str='%s'", s);
                            } else if (alen > 0 && alen <= 64 &&
                                       trap_mapped(v + 0x20)) {
                                fprintf(stderr, "  arr[%d]=", alen);
                                for (int i = 0; i < alen && i < 12; i++) {
                                    uintptr_t ip =
                                        *(uintptr_t *)(v + 0x20 + 8 * i);
                                    fprintf(stderr, " %#lx",
                                            (unsigned long)(ip >= g_il2b ?
                                                            ip - g_il2b : ip));
                                }
                            }
                        }
                        fprintf(stderr, "\n");
                    }
                }
            } else if (p->kind == 16 || p->kind == 22) {
                fprintf(stderr, "[trap] arming throw probe now (kind %d)\n", p->kind);
                arm_late_throw(0xd952b0);
            } else if (p->kind == 23) {
                disarm_throw();
            } else if (p->kind == 15) {
                /* Substring(this=str, startIndex, length): show inputs */
                char s2[620];
                uintptr_t st = (uintptr_t)u->uc_mcontext.regs[0];
                trap_il2str(st, s2, sizeof s2);
                long slen = -1;
                if (trap_ptr_ok(st) && trap_mapped(st))
                    slen = *(volatile int *)(st + 0x10);
                fprintf(stderr,
                        "[trap]   Substring(%ld, start=%ld, len=%ld)\n[trap]   src='%s'\n",
                        slen, (long)u->uc_mcontext.regs[1],
                        (long)u->uc_mcontext.regs[2], s2);
            } else if (p->kind == 17) {
                /* GetNotificationFilter runner-null site: resolve the chain
                 * the way the code does it: GOT slot -> rec -> rec[0]=klass
                 * -> klass+0xb8 statics -> statics[0] runner instance.  Dump
                 * for both KairoPlugin(0x1ebf660) and IApplication(0x1ebf2d8). */
                static const struct { uint32_t off; const char *who; } CH[] = {
                    {0x1ebf660, "KairoPlugin"}, {0x1ebf2d8, "IApplication?"},
                };
                for (int c = 0; c < 2; c++) {
                    uintptr_t slot = g_il2b + CH[c].off;
                    uintptr_t rec = trap_mapped(slot) ? *(uintptr_t *)slot : 0;
                    uintptr_t kla = (trap_ptr_ok(rec) && trap_mapped(rec))
                                    ? *(uintptr_t *)rec : 0;
                    char b1[96], b2[96];
                    fprintf(stderr, "[trap]   %s slot[+%x] rec=%#lx klass=%#lx %s.%s",
                            CH[c].who, CH[c].off,
                            (unsigned long)rec, (unsigned long)kla,
                            trap_ptr_ok(kla) && trap_mapped(kla)
                                ? trap_cstr(*(uintptr_t *)(kla + 0x18), b2, sizeof b2) : "?",
                            trap_ptr_ok(kla) && trap_mapped(kla)
                                ? trap_cstr(*(uintptr_t *)(kla + 0x10), b1, sizeof b1) : "?");
                    uintptr_t st = (trap_ptr_ok(kla) && trap_mapped(kla + 0xb8))
                                   ? *(uintptr_t *)(kla + 0xb8) : 0;
                    fprintf(stderr, " statics=%#lx", (unsigned long)st);
                    if (trap_ptr_ok(st) && trap_mapped(st)) {
                        uintptr_t inst = *(uintptr_t *)st;
                        fprintf(stderr, " [0]=%#lx", (unsigned long)inst);
                        if (trap_ptr_ok(inst) && trap_mapped(inst)) {
                            uintptr_t ik = *(uintptr_t *)inst;
                            char c1[96], c2[96];
                            if (trap_ptr_ok(ik) && trap_mapped(ik))
                                fprintf(stderr, " (%s.%s)",
                                        trap_cstr(*(uintptr_t *)(ik + 0x18), c2, sizeof c2),
                                        trap_cstr(*(uintptr_t *)(ik + 0x10), c1, sizeof c1));
                        }
                    }
                    fprintf(stderr, "\n");
                }
            } else if (p->kind == 18) {
                /* KairoPlugin::Init+0x160: str x22,[x8] -- the statics[0] store.
                 * x8 = KairoPlugin statics base, x22 = value copied from
                 * IApplication.statics[0]. */
                uintptr_t dst = (uintptr_t)u->uc_mcontext.regs[8];
                uintptr_t val = (uintptr_t)u->uc_mcontext.regs[22];
                fprintf(stderr,
                        "[trap]   Init.store: dst=%#lx val(x22)=%#lx\n",
                        (unsigned long)dst, (unsigned long)val);
                if (trap_ptr_ok(val) && trap_mapped(val)) {
                    uintptr_t ik = *(uintptr_t *)val;
                    char c1[96], c2[96];
                    if (trap_ptr_ok(ik) && trap_mapped(ik))
                        fprintf(stderr, "[trap]   stored obj class=%s.%s\n",
                                trap_cstr(*(uintptr_t *)(ik + 0x18), c2, sizeof c2),
                                trap_cstr(*(uintptr_t *)(ik + 0x10), c1, sizeof c1));
                } else {
                    fprintf(stderr, "[trap]   stored value is NULL/invalid\n");
                }
            } else if (p->kind == 24) {
                /* Storage::Open(folder=w0, w1, x2, x3): multi-hit marker */
                fprintf(stderr,
                        "[trap]   Storage.Open folder=%ld w1=%ld caller=il2cpp+%#lx\n",
                        (long)u->uc_mcontext.regs[0],
                        (long)u->uc_mcontext.regs[1],
                        (unsigned long)(x30 >= g_il2b ? x30 - g_il2b : x30));
            } else if (p->kind == 25) {
                /* GetFolder base-path dump: x20 = IApplication.statics[0]->+0xf0 */
                char b1[600];
                uintptr_t s = (uintptr_t)u->uc_mcontext.regs[20];
                trap_il2str(s, b1, sizeof b1);
                fprintf(stderr,
                        "[trap]   GetFolder: folder(w19)=%ld basepath(x20)='%s'\n",
                        (long)u->uc_mcontext.regs[19], b1);
            } else if (p->kind == 26 || p->kind == 27) {
                /* post-LastIndexOf: w0 = index; also dump the literal arg. */
                unsigned long slotva = p->kind == 26 ? 0x1ed2170 : 0x1ed2158;
                uintptr_t slot = g_il2b + slotva;
                uintptr_t rec = trap_mapped(slot) ? *(uintptr_t *)slot : 0;
                uintptr_t lit = (trap_ptr_ok(rec) && trap_mapped(rec))
                                ? *(uintptr_t *)rec : 0;
                char b1[300];
                trap_il2str(lit, b1, sizeof b1);
                fprintf(stderr,
                        "[trap]   GetFolder: %s=%ld literal='%s'\n",
                        p->kind == 26 ? "idx1" : "idx2",
                        (long)(int)u->uc_mcontext.regs[0], b1);
            } else if (p->kind == 28) {
                /* Substring(x0=this, w1=start, w2=len) call site */
                fprintf(stderr,
                        "[trap]   GetFolder: Substring start=%ld len=%ld\n",
                        (long)(int)u->uc_mcontext.regs[1],
                        (long)(int)u->uc_mcontext.regs[2]);
            } else if (p->kind == 29) {
                /* GetFolder shared epilogue: x0 = x20 = final folder string */
                char b1[600];
                trap_il2str((uintptr_t)u->uc_mcontext.regs[0], b1, sizeof b1);
                fprintf(stderr,
                        "[trap]   GetFolder: result='%s' folder(w19)=%ld\n",
                        b1, (long)u->uc_mcontext.regs[19]);
            } else if (p->kind == 21) {
                /* RecordStore::Setup store site: str x8, [x9, #0x18] --
                 * x8 = Storage::Open(4,1,0,0) result parked in X.statics[0x18]. */
                uintptr_t v8 = (uintptr_t)u->uc_mcontext.regs[8];
                fprintf(stderr,
                        "[trap]   Setup.store: X.statics[0x18] <- %#lx",
                        (unsigned long)v8);
                if (trap_ptr_ok(v8) && trap_mapped(v8)) {
                    uintptr_t ik = *(uintptr_t *)v8;
                    char c1[96], c2[96];
                    if (trap_ptr_ok(ik) && trap_mapped(ik))
                        fprintf(stderr, " (%s.%s)",
                                trap_cstr(*(uintptr_t *)(ik + 0x18), c2, sizeof c2),
                                trap_cstr(*(uintptr_t *)(ik + 0x10), c1, sizeof c1));
                } else {
                    fprintf(stderr, " (NULL!)");
                }
                fprintf(stderr, "\n");
            } else if (p->kind == 20) {
                /* RecordStore::GetNumRecords: x0 = files array just returned;
                 * x19 = this RecordStore.  Which storage mode, which path,
                 * and did the file list come back NULL? */
                uintptr_t files = (uintptr_t)u->uc_mcontext.regs[0];
                uintptr_t ths = (uintptr_t)u->uc_mcontext.regs[19];
                char pth[600];
                fprintf(stderr,
                        "[trap]   GetNumRecords: files=%#lx this=%#lx\n",
                        (unsigned long)files, (unsigned long)ths);
                if (trap_ptr_ok(files) && trap_mapped(files + 0x18))
                    fprintf(stderr, "[trap]   files.len=%d\n",
                            *(volatile int *)(files + 0x18));
                if (trap_ptr_ok(ths) && trap_mapped(ths + 0x18)) {
                    fprintf(stderr, "[trap]   this.mode(0x10)=%d\n",
                            *(volatile int *)(ths + 0x10));
                    trap_il2str(*(uintptr_t *)(ths + 0x18), pth, sizeof pth);
                    fprintf(stderr, "[trap]   this.path='%s'\n", pth);
                }
                /* Config (slot 0x1ebf338) storage-mode byte statics[0x20] */
                uintptr_t slot = g_il2b + 0x1ebf338;
                uintptr_t rec = trap_mapped(slot) ? *(uintptr_t *)slot : 0;
                uintptr_t kla = (trap_ptr_ok(rec) && trap_mapped(rec))
                                ? *(uintptr_t *)rec : 0;
                if (trap_ptr_ok(kla) && trap_mapped(kla + 0xb8)) {
                    uintptr_t st = *(uintptr_t *)(kla + 0xb8);
                    if (trap_ptr_ok(st) && trap_mapped(st + 0x20))
                        fprintf(stderr,
                                "[trap]   Config.statics[0x20](pref-mode)=%d\n",
                                *(volatile unsigned char *)(st + 0x20));
                }
            } else if (p->kind == 19) {
                /* NRE-raise stub entry: x30 = codegen raise call site +4.
                 * That is the instruction right after `bl raiseNRE`, so the
                 * guarding null check sits a couple of insns earlier. */
                fprintf(stderr,
                        "[trap]   NRE-raise site = il2cpp+%#lx\n",
                        (unsigned long)(x30 >= g_il2b ? x30 - g_il2b : x30));
            } else if (p->kind == 14) {
                /* raise-helper entry: x30 = codegen out-of-range check site */
                fprintf(stderr, "[trap]   ra=il2cpp+%#lx",
                        (unsigned long)(x30 >= g_il2b ? x30 - g_il2b : x30));
                uintptr_t o = (uintptr_t)u->uc_mcontext.regs[0];
                if (trap_ptr_ok(o) && trap_mapped(o)) {
                    uintptr_t k = *(uintptr_t *)o;
                    if (trap_ptr_ok(k) && trap_mapped(k)) {
                        char b1[96], b2[96];
                        fprintf(stderr, " arg=%s.%s",
                                trap_cstr(*(uintptr_t *)(k + 0x18), b2, sizeof b2),
                                trap_cstr(*(uintptr_t *)(k + 0x10), b1, sizeof b1));
                    }
                }
                fprintf(stderr, "\n");
                uintptr_t fp = (uintptr_t)u->uc_mcontext.regs[29];
                for (int f = 0; f < 10 && trap_ptr_ok(fp) && trap_mapped(fp); f++) {
                    uintptr_t ra = *(uintptr_t *)(fp + 8);
                    fprintf(stderr, "[trap]     frame[%d] ra=%#lx (il2cpp+%#lx)\n",
                            f, (unsigned long)ra,
                            (unsigned long)(ra >= g_il2b ? ra - g_il2b : ra));
                    uintptr_t next = *(uintptr_t *)fp;
                    if (!trap_ptr_ok(next) || next <= fp || next - fp > 0x100000)
                        break;
                    fp = next;
                }
            } else if (p->kind == 10) {
                /* catch-site: guarded briefs of thrown object / wrapper */
                uintptr_t r23 = (uintptr_t)u->uc_mcontext.regs[23];
                uintptr_t r25 = (uintptr_t)u->uc_mcontext.regs[25];
                uintptr_t r29 = (uintptr_t)u->uc_mcontext.regs[29];
                trap_brief(r23, "x23");
                trap_brief(r25, "x25");
                trap_brief(r29, "x29");
                if (trap_ptr_ok(r23) && trap_mapped(r23))
                    trap_brief(*(uintptr_t *)r23, "*x23");
                if (trap_ptr_ok(r25) && trap_mapped(r25))
                    trap_brief(*(uintptr_t *)r25, "*x25");
            } else if (p->kind == 7) {
                /* Update entry-form creation: x22 = Type from GetType(),
                 * x20 = created form (later regs); entry-name static slot. */
                uintptr_t t = (uintptr_t)u->uc_mcontext.regs[22];
                char nm2[128], ns2[128], es[600];
                const char *cname = "?", *nname = "?";
                if (trap_ptr_ok(t)) {
                    uintptr_t k = *(uintptr_t *)t;
                    if (trap_ptr_ok(k)) {
                        nname = trap_cstr(*(uintptr_t *)(k + 0x10), nm2, sizeof nm2);
                        cname = trap_cstr(*(uintptr_t *)(k + 0x18), ns2, sizeof ns2);
                    }
                }
                fprintf(stderr, "[trap]   x22(Type)=%#lx (%s.%s)\n",
                        (unsigned long)t, cname, nname);
                uintptr_t sp2 = *(uintptr_t *)(g_il2b + 0x1ecd798);
                trap_il2str(trap_ptr_ok(sp2) ? *(uintptr_t *)sp2 : 0, es, sizeof es);
                fprintf(stderr, "[trap]   entry-name slot[0x1ecd798]: '%s'\n", es);
            } else if (p->kind == 4) {
                /* value probe: dump x0 and x1 as raw ptr + klass name */
                for (int r = 0; r <= 1; r++) {
                    uintptr_t o = (uintptr_t)u->uc_mcontext.regs[r];
                    char nm2[128], ns2[128];
                    const char *cname = "?", *nname = "?";
                    if (trap_ptr_ok(o)) {
                        uintptr_t k = *(uintptr_t *)o;
                        if (trap_ptr_ok(k)) {
                            nname = trap_cstr(*(uintptr_t *)(k + 0x10), nm2, sizeof nm2);
                            cname = trap_cstr(*(uintptr_t *)(k + 0x18), ns2, sizeof ns2);
                        }
                    }
                    fprintf(stderr, "[trap]   x%d=%#lx (%s.%s)\n", r,
                            (unsigned long)o, cname, nname);
                }
            }
            g_in_dump = 0;
            if (p->cap && p->hit < p->cap) {
                /* multi-hit: restore our original insn and park a transient
                 * one-shot at +4 that re-arms us -- the old "leave brk armed"
                 * simply re-trapped the same instruction cap-times, which
                 * slowed storm paths to a crawl. */
                uintptr_t pg2 = p->addr & ~0xfffUL;
                if (mprotect((void *)pg2, 0x1000, PROT_READ | PROT_WRITE | PROT_EXEC) == 0) {
                    *(uint32_t *)p->addr = p->orig;
                    __builtin___clear_cache((char *)p->addr, (char *)p->addr + 4);
                    mprotect((void *)pg2, 0x1000, PROT_READ | PROT_EXEC);
                }
                if (g_nprobes < MAX_PROBES) {
                    struct probe *q = &g_probes[g_nprobes];
                    uintptr_t a = p->addr + 4;
                    uintptr_t qpg = a & ~0xfffUL;
                    if (mprotect((void *)qpg, 0x1000, PROT_READ | PROT_WRITE | PROT_EXEC) == 0) {
                        q->addr = a;
                        q->orig = *(uint32_t *)a;
                        q->hit = 0;
                        q->cap = 0;
                        q->kind = 98;
                        q->tag = "step-over";
                        *(uint32_t *)a = 0xd4200000;
                        __builtin___clear_cache((char *)a, (char *)a + 4);
                        mprotect((void *)qpg, 0x1000, PROT_READ | PROT_EXEC);
                        g_nprobes++;
                    }
                }
                return; /* PC at probe addr -> original insn executes */
            }
            uintptr_t pg = p->addr & ~0xfffUL;
            if (mprotect((void *)pg, 0x1000,
                         PROT_READ | PROT_WRITE | PROT_EXEC) == 0) {
                *(uint32_t *)p->addr = p->orig;
                __builtin___clear_cache((char *)p->addr, (char *)p->addr + 4);
                mprotect((void *)pg, 0x1000, PROT_READ | PROT_EXEC);
            }
            return; /* PC still at probe addr -> re-execute original insn */
        }
    }
    fprintf(stderr, "\n[gds] signal %d (si_code=%d) at pc=%#lx addr=%p\n",
            sig, si ? si->si_code : 0, pc, si ? si->si_addr : NULL);
    /* crash forensics (0.85.1): the OSK-DONE crash gave pc+addr+regs that
     * didn't uniquely identify the faulting instruction.  Dump the raw
     * insn word at pc and walk the x29 frame chain so the caller sequence
     * is captured too.  Probe readability via write-to-/dev/null (EFAULT)
     * so a garbage pc/fp can't nest a second fault in the dump. */
    {
        static int nullfd = -2;
        if (nullfd == -2) nullfd = open("/dev/null", O_WRONLY);
        g_in_dump = 1;
        int readable = nullfd >= 0 &&
            write(nullfd, (void *)pc, 4) == 4;
        if (readable)
            fprintf(stderr, "[gds]   insn@%#lx = %08lx\n", pc,
                    (unsigned long)*(volatile uint32_t *)pc);
        unsigned long fp = (unsigned long)u->uc_mcontext.regs[29];
        unsigned long sp = (unsigned long)u->uc_mcontext.sp;
        fprintf(stderr, "[gds]   backtrace:");
        for (int d = 0; d < 10 && fp; d++) {
            /* frame must sit on this stack, above sp, aligned */
            if (fp < sp || fp - sp > (64UL << 20) || (fp & 0xf)) break;
            if (nullfd < 0 ||
                write(nullfd, (void *)fp, 16) != 16) break;
            unsigned long prev = *(volatile unsigned long *)fp;
            unsigned long lr = *(volatile unsigned long *)(fp + 8);
            if (!lr) break;
            fprintf(stderr, " #%d=%#lx", d, lr);
            if (prev <= fp) break;
            fp = prev;
        }
        fprintf(stderr, "\n");
        g_in_dump = 0;
    }
    for (size_t i = 0; i < sizeof LIBS / sizeof *LIBS; i++) {
        nx_mod *m = nx_find_mod(LIBS[i].soname);
        if (!m)
            continue;
        unsigned long b = (unsigned long)m->base;
        if (pc >= b && pc < b + m->span)
            fprintf(stderr, "[gds]   pc is %s+%#lx\n", m->name, pc - b);
        fprintf(stderr, "[gds]   %-24s %#lx..%#lx\n", m->name, b, b + m->span);
    }
    for (int i = 0; i < 28; i += 4)
        fprintf(stderr, "[gds]   x%-2d=%016lx x%-2d=%016lx x%-2d=%016lx x%-2d=%016lx\n",
                i, (unsigned long)u->uc_mcontext.regs[i],
                i + 1, (unsigned long)u->uc_mcontext.regs[i + 1],
                i + 2, (unsigned long)u->uc_mcontext.regs[i + 2],
                i + 3, (unsigned long)u->uc_mcontext.regs[i + 3]);
    fprintf(stderr, "[gds]   x28=%016lx x29=%016lx x30=%016lx\n",
            (unsigned long)u->uc_mcontext.regs[28],
            (unsigned long)u->uc_mcontext.regs[29],
            (unsigned long)u->uc_mcontext.regs[30]);
    fprintf(stderr, "[gds]   lr=%016lx sp=%016lx probe_slot=%u\n",
            (unsigned long)u->uc_mcontext.regs[30],
            (unsigned long)u->uc_mcontext.sp, nx_probe_slot);
    fflush(stderr);
    _exit(2);
}

static void on_exit_signal(int sig)
{
    (void)sig;
    gds_input_request_exit();
}

static void install_fault_handler(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_fault;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);
    /* 0.61: SIGTRAP/SIGABRT/SIGSYS were missed - a bare `brk` (Unity/il2cpp
     * fail-fast paths use __builtin_trap) used to take the default action and
     * left a silent "Trace/breakpoint trap" in the launcher log.  bionic.c's
     * my_sigaction blocks the engine from overwriting these. */
    sigaction(SIGTRAP, &sa, NULL);
    sigaction(SIGABRT, &sa, NULL);
    sigaction(SIGSYS, &sa, NULL);
    sigaction(SIGFPE, &sa, NULL);

    /* SIGTERM/SIGINT seguem o caminho do SELECT+START (pause/save/saída),
     * nunca morte seca: frontends e supervisores mandam TERM primeiro. */
    struct sigaction quit;
    memset(&quit, 0, sizeof quit);
    quit.sa_handler = on_exit_signal;
    sigemptyset(&quit.sa_mask);
    sigaction(SIGTERM, &quit, NULL);
    sigaction(SIGINT, &quit, NULL);
}

static void run_unity(void)
{
    void *env = gds_jni_env();
    void *player = gds_jret_obj("com/unity3d/player/UnityPlayer");
    void *activity = gds_jni_activity();
    void *surface = gds_jret_obj("android/view/Surface");
    void *fn;

    gds_jni_set_unity_player(player);

    fn = gds_jni_native("com/unity3d/player/UnityPlayer", "initJni");
    if (!fn)
        nx_die("Unity did not register initJni");
    fprintf(stderr, "[gds] initJni...\n");
    ((void (*)(void *, void *, void *))fn)(env, player, activity);
    fprintf(stderr, "[gds] initJni OK\n");

    fn = gds_jni_native("com/unity3d/player/UnityPlayer",
                        "nativeRecreateGfxState");
    if (!fn)
        nx_die("Unity did not register nativeRecreateGfxState");
    fprintf(stderr, "[gds] nativeRecreateGfxState(surfaceCreated)...\n");
    ((void (*)(void *, void *, int, void *))fn)(env, player, 0, surface);
    fprintf(stderr, "[gds] nativeRecreateGfxState(surfaceCreated) OK\n");

    /* UnityPlayer's SurfaceHolder callback immediately repeats updateGLDisplay
     * for the initial surfaceChanged notification before forwarding the size
     * change.  Preserve that ordering even though both callbacks carry the
     * same native Surface in the fbdev host. */
    fprintf(stderr, "[gds] nativeRecreateGfxState(surfaceChanged)...\n");
    ((void (*)(void *, void *, int, void *))fn)(env, player, 0, surface);
    fprintf(stderr, "[gds] nativeRecreateGfxState(surfaceChanged) OK\n");

    fn = gds_jni_native("com/unity3d/player/UnityPlayer",
                        "nativeSendSurfaceChangedEvent");
    if (fn) {
        ((void (*)(void *, void *))fn)(env, player);
        fprintf(stderr, "[gds] nativeSendSurfaceChangedEvent OK\n");
    }

    fn = gds_jni_native("com/unity3d/player/UnityPlayer",
                        "nativeFocusChanged");
    if (fn) {
        ((void (*)(void *, void *, int))fn)(env, player, 1);
        fprintf(stderr, "[gds] nativeFocusChanged(true) OK\n");
    }
    fn = gds_jni_native("com/unity3d/player/UnityPlayer", "nativeResume");
    if (fn) {
        ((void (*)(void *, void *))fn)(env, player);
        fprintf(stderr, "[gds] nativeResume OK\n");
    }

    gds_audio_start(env);

    void *render = gds_jni_native("com/unity3d/player/UnityPlayer",
                                  "nativeRender");
    if (!render)
        nx_die("Unity did not register nativeRender");
    fprintf(stderr, "[gds] nativeRender loop%s\n",
            gds_max_frames > 0 ? " (test frame limit active)" : "");

    gds_input_init();

    /* Watchdog for a hung frame.  Unity installs its own crash handler, which
     * prints a symbolised backtrace of whichever thread faults -- so the way to
     * see where a stuck frame is stuck is to fault that exact thread on purpose.
     * Off unless GDS_WATCHDOG names a timeout in seconds. */
    gds_arm_frame_watchdog();

    unsigned long frame = 0;
    for (;;) {
        gds_watchdog_frame();
        gds_input_poll(env, player, frame);
        if (gds_input_exit_requested()) {
            fprintf(stderr, "[gds] controller requested lifecycle exit\n");
            break;
        }
        uint8_t keep = ((uint8_t (*)(void *, void *))render)(env, player);
        frame++;
        if (frame <= 10 || frame % 300 == 0)
            fprintf(stderr, "[gds] frame %lu keep=%u\n", frame, keep);
        if (!keep) {
            fprintf(stderr, "[gds] Unity requested render-loop stop at frame %lu\n",
                    frame);
            break;
        }
        if (gds_max_frames > 0 && frame >= (unsigned long)gds_max_frames) {
            fprintf(stderr, "[gds] test frame limit reached (%lu)\n", frame);
            break;
        }
        const char *frame_us = getenv("GDS_FRAME_US");
        usleep(frame_us ? (useconds_t)strtoul(frame_us, NULL, 10) : 16667);
    }

    fn = gds_jni_native("com/unity3d/player/UnityPlayer", "nativeFocusChanged");
    if (fn) {
        ((void (*)(void *, void *, int))fn)(env, player, 0);
        fprintf(stderr, "[gds] nativeFocusChanged(false) OK\n");
    }
    fn = gds_jni_native("com/unity3d/player/UnityPlayer", "nativePause");
    if (fn) {
        ((void (*)(void *, void *))fn)(env, player);
        fprintf(stderr, "[gds] nativePause OK\n");
    }
    gds_input_close();
    gds_audio_stop();
}

int main(int argc, char **argv)
{
    setvbuf(stderr, NULL, _IOLBF, 0);
    /* 0.61: stdout fully unbuffered.  egl_shim.c logs via printf; when the
     * launcher redirected stdout into the log FILE, libc block-buffered it and
     * every EGL diagnostic silently vanished when the process was SIGTRAP'd. */
    setvbuf(stdout, NULL, _IONBF, 0);

    /* EmulationStation's application wrapper exports C.UTF-8.  This Android
     * Unity player was built against Bionic's locale ABI; when its native
     * startup crosses the host glibc C.UTF-8 locale, a small-string object is
     * overwritten and its stack canary fires before frame one.  Android's
     * invariant/POSIX locale is the matching behaviour for this port. */
    setenv("LANG", "C", 1);
    setenv("LC_ALL", "C", 1);
    setenv("GC_DISABLE_INCREMENTAL", "1", 0);
    setenv("MALLOC_ARENA_MAX", "2", 0);

    read_env();
    install_fault_handler();
    setup_paths(argc > 1 ? argv[1] : NULL);
    gds_fs_set_data_dir(gds_datadir);

    /* 0.75: on-device experiment switches.  KEY=VAL lines in
     * <gamedir>/gds_env.cfg apply as environment DEFAULTS (launcher env
     * wins), so display/input theories iterate over ssh without
     * redeploying the loader.  '#' starts a comment. */
    {
        char cfgpath[1200];
        snprintf(cfgpath, sizeof cfgpath, "%s/gds_env.cfg", gds_gamedir);
        FILE *cf = fopen(cfgpath, "r");
        if (!cf) { cf = fopen("gds_env.cfg", "r"); cfgpath[0] = 0; }
        if (cf) {
            fprintf(stderr, "[gds] env cfg: %s\n",
                    cfgpath[0] ? cfgpath : "gds_env.cfg");
            char line[512];
            while (fgets(line, sizeof line, cf)) {
                char *p = line;
                while (*p == ' ' || *p == '\t') p++;
                if (*p == '#' || *p == '\n' || !*p) continue;
                char *eq = strchr(p, '=');
                if (!eq) continue;
                *eq = 0;
                size_t kn = strlen(p);
                while (kn && (p[kn-1] == ' ' || p[kn-1] == '\t')) p[--kn] = 0;
                char *v = eq + 1;
                size_t n = strlen(v);
                while (n && (v[n-1] == '\n' || v[n-1] == '\r' ||
                             v[n-1] == ' ' || v[n-1] == '\t')) v[--n] = 0;
                if (!kn) continue;
                if (getenv(p)) {
                    fprintf(stderr, "[gds]   %s (kept: launcher env)\n", p);
                    continue;
                }
                setenv(p, v, 0);
                fprintf(stderr, "[gds]   %s=%s\n", p, v);
            }
            fclose(cf);
        }
    }

    fprintf(stderr, "[gds] Game Dev Story for NextOS -- gamedir %s (reference-port 0.85.1-fmodfix)\n", gds_gamedir);

    gds_jni_init();
    gds_egl_init();
    /* 0.62 lesson from the old loader (0.30): SDL_Init(VIDEO)/kmsdrm and the
     * Mali blob reinstall default crash-signal handlers over ours.  Re-arm. */
    install_fault_handler();
    build_imports();

    int missing = gds_load_modules();
    fprintf(stderr, "[gds] modules loaded, %d relocations unresolved\n", missing);

    /* 0.82: install address-table input/landscape hooks NOW (before any
     * managed frame).  0.81 installed them on frame ~3; disassembly shows
     * SurfaceManager::Setup (called from main.AppData::Init during boot)
     * reads IApplication::IsSide once and latches the layout byte, so the
     * ret-1 patch landed too late and the game stayed portrait. */
    gds_input_install_now();

    nx_mod *main_mod = nx_find_mod("libmain.so");
    nx_mod *uni = nx_find_mod("libunity.so");
    nx_mod *il2 = nx_find_mod("libil2cpp.so");
    if (!main_mod || !uni || !il2)
        nx_die("required Unity module disappeared after relocation");
    if (getenv("GDS_DEBUGBASE"))
        fprintf(stderr, "[gds] DEBUGBASE il2cpp=%p unity=%p main=%p\n",
                (void *)il2->base, (void *)uni->base, (void *)main_mod->base);
    arm_traps((uintptr_t)il2->base);

    /* System.load(libmain.so): its constructors run before JNI_OnLoad. */
    nx_run_init(main_mod);
    typedef int (*onload)(void *vm, void *reserved);
    onload main_onload = (onload)nx_lookup_in(main_mod, "JNI_OnLoad");
    if (!main_onload)
        nx_die("libmain.so has no JNI_OnLoad");
    int main_version = main_onload(gds_jni_vm(), NULL);
    if (main_version < 0)
        nx_die("JNI_OnLoad(libmain.so) failed: %#x", main_version);
    fprintf(stderr, "[gds] JNI_OnLoad(libmain.so) -> %#x\n", main_version);

    /* UnityPlayer.loadNative now calls the exact native method registered by
     * libmain.  That method dlopens libunity first and libil2cpp second; our
     * handle-aware dlopen bridge runs each real init array immediately before
     * its own JNI_OnLoad, matching this APK's NativeLoader implementation. */
    void *native_load =
        gds_jni_native("com/unity3d/player/NativeLoader", "load");
    if (!native_load)
        nx_die("libmain did not register NativeLoader.load");
    char libdir[1200];
    snprintf(libdir, sizeof libdir, "%s", gds_gamedir);
    void *loader_class =
        gds_jret_class("com/unity3d/player/NativeLoader");
    void *loader_path = gds_jret_str(libdir);
    int loaded = ((int (*)(void *, void *, void *))native_load)(
        gds_jni_env(), loader_class, loader_path);
    if (!loaded || !uni->inited || !il2->inited)
        nx_die("NativeLoader.load failed (result=%d unity_init=%d il2cpp_init=%d)",
               loaded, uni->inited, il2->inited);

    fprintf(stderr,
            "[gds] NativeLoader.load completed: libunity -> libil2cpp\n");
    run_unity();
    return 0;
}
