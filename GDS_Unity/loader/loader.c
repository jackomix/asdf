/* loader.c - load an Android arm64 .so the way the device linker would, inside
 * our own process, so libil2cpp.so / libunity.so run on a glibc Linux box (the
 * R36S) without Android's linker.
 *
 * Strategy (matching terraria-nextos / native Android loaders):
 *   1. map PT_LOAD segments of libil2cpp.so at a relocatable base
 *   2. build a symbol table; for every GLOB_DAT/JUMP_SLOT/COPY relocation that
 *      points at a libc/libm/bionic symbol, resolve it via dlsym() against the
 *      host (glibc on the device; musl under the Unicorn test bench)
 *   3. apply RELATIVE relocs
 *   4. run DT_INIT_ARRAY / DT_INIT
 *   5. hand control to the engine (libunity.so) in stage 2
 *
 * Stage 1 here: load libil2cpp.so, run its init array, prove the relocation +
 * symbol resolution works, and call il2cpp_runtime_invoke() to show the C
 * runtime inside the shipped .so is alive.  Tested headless under Unicorn via
 * tools/run_aarch64.py (no GPU needed).
 */
#include "kv_elf.h"
#include "kv_libc.h"
#include <stdint.h>
#include <stddef.h>

/* JNI shim (jni_shim.c) - provides the JavaVM/JNIEnv the engine's JNI_OnLoad
 * needs.  Declared here so loader.c can drive the Unity boot. */
void *kv_jni_java_vm(void);
void *kv_jni_env(void);
void *kv_jni_find_native(const char *name);
int kv_log_open(const char *path);
void kv_install_crash_handler(void);

#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif

typedef struct Module {
    char           name[128];
    uint8_t       *base;
    uint64_t       bias;
    uint64_t       load_vaddr;
    Elf64_Dyn     *dynamic;
    Elf64_Sym     *symtab;
    const char    *strtab;
    size_t         strsz;
    Elf64_Rela    *rela;
    size_t         rela_count;
    Elf64_Rela    *jmprel;
    size_t         jmprel_count;
    uint8_t       *init_array;
    size_t         init_array_sz;
    void (*init)(void);
    struct Module *next;
} Module;

static Module *g_modules;
static uint8_t *g_brk = (uint8_t *)0x200000000UL;

static void *xmmap(uint8_t *hint, size_t len, int prot) {
    void *p = mmap(hint, len, prot, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE,
                   -1, 0);
    if (p == MAP_FAILED) {
        printf( "[loader] mmap(%p,%zu) failed: %m\n", hint, len);
        exit(2);
    }
    return p;
}

static void *read_all(const char *path, size_t *out_len) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) { printf( "[loader] open %s: %m\n", path); exit(2); }
    /* Size the file first (lseek END) and allocate exactly once.  The real .so
     * is tens of MB (libil2cpp.so 33 MB, libunity.so 16 MB), so this must not
     * leak a doubling-growth buffer for every .so or the bump allocator
     * exhausts and later modules get malloc(0). */
    long sz = lseek(fd, 0, SEEK_END);
    if (sz < 0) sz = (1 << 20);
    lseek(fd, 0, SEEK_SET);
    uint8_t *buf = malloc(sz ? (size_t)sz : 1);
    if (!buf) { printf("[loader] malloc(%ld) failed for %s\n", sz, path); exit(2); }
    ssize_t got = 0;
    while (got < sz) {
        ssize_t r = read(fd, buf + got, sz - got);
        if (r <= 0) break;
        got += r;
    }
    close(fd);
    *out_len = got;
    return buf;
}

static Module *module_new(const char *name) {
    Module *m = calloc(1, sizeof *m);
    strncpy(m->name, name, sizeof m->name - 1);
    m->next = g_modules;
    g_modules = m;
    return m;
}

/* Look up a DEFINED dynamic symbol (e.g. an export) in module `m` and return
 * its runtime address, or 0.  Used to find entry points like JNI_OnLoad. */
static void *module_export(Module *m, const char *wanted) {
    if (!m->symtab || !m->strtab || !m->strsz) return 0;
    uintptr_t s0 = (uintptr_t)m->strtab;
    uintptr_t s1 = s0 + m->strsz;
    for (size_t i = 0; i < 1000000; i++) {
        Elf64_Sym *sym = &m->symtab[i];
        const char *nm = m->strtab + sym->st_name;
        /* the dynamic symtab ends at a null (st_name=0, st_shndx=0, st_value=0)
         * entry - stop there; also stop if the name points outside the strtab */
        if (sym->st_name == 0 && sym->st_shndx == 0 && sym->st_value == 0 &&
            sym->st_size == 0 && i > 4) break;
        if ((uintptr_t)nm < s0 || (uintptr_t)nm >= s1) break;
        if (sym->st_shndx != SHN_UNDEF && sym->st_value != 0 &&
            strcmp(nm, wanted) == 0) {
            return (void *)(m->bias + sym->st_value);
        }
    }
    return 0;
}

/* Look up a DEFINED dynamic symbol across ALL loaded modules.  This is what
 * dlopen/dlsym need so libmain.so's JNI_OnLoad can dlsym into libunity.so /
 * libil2cpp.so the way Android's linker would. */
void *loader_lookup_export(const char *wanted) {
    for (Module *m = g_modules; m; m = m->next) {
        void *p = module_export(m, wanted);
        if (p) return p;
    }
    return 0;
}

static void *resolve(const char *sym) {
    return dlsym(0, sym);
}

/* Resolve relocation symbol `symidx` in module `m`.  Prefer the module's own
 * dynamic symbol table (a GLOB_DAT/JUMP_SLOT may reference a symbol that is
 * DEFINED in this same .so - e.g. kairo_marker), else fall back to the host
 * symbol table (libc/libm/bionic shims) for genuinely imported symbols. */
static void *resolve_sym(Module *m, uint64_t symidx, const char *name) {
    Elf64_Sym *sym = &m->symtab[symidx];
    if (sym->st_shndx != SHN_UNDEF && sym->st_value != 0) {
        return (void *)(m->bias + sym->st_value);
    }
    return resolve(name);
}

static Module *load_object(const char *path) {
    size_t len = 0;
    uint8_t *file = read_all(path, &len);
    Elf64_Ehdr *eh = (Elf64_Ehdr *)file;
    if (memcmp(eh->e_ident, "\177ELF", 4) != 0) { printf("[loader] %s: not ELF\n", path); exit(2); }
    if (eh->e_machine != 0xB7) { printf("[loader] %s: not aarch64\n", path); exit(2); }

    uint64_t minv = ~0ULL, maxv = 0;
    for (Elf64_Half i = 0; i < eh->e_phnum; i++) {
        Elf64_Phdr *ph = (Elf64_Phdr *)(file + eh->e_phoff + i * eh->e_phentsize);
        if (ph->p_type != 1) continue;
        if (ph->p_vaddr < minv) minv = ph->p_vaddr;
        if (ph->p_vaddr + ph->p_memsz > maxv) maxv = ph->p_vaddr + ph->p_memsz;
    }
    uint64_t aligned_min = minv & ~(KV_PAGE - 1);
    size_t span = (maxv - aligned_min + KV_PAGE - 1) & ~(KV_PAGE - 1);

    uint8_t *base = xmmap(g_brk, span, PROT_READ | PROT_WRITE | PROT_EXEC);
    g_brk += span + KV_PAGE * 16;
    mprotect(base, span, PROT_READ | PROT_WRITE | PROT_EXEC);

    Module *m = module_new(path);
    m->base = base;
    m->load_vaddr = aligned_min;
    m->bias = (uint64_t)base - aligned_min;

    for (Elf64_Half i = 0; i < eh->e_phnum; i++) {
        Elf64_Phdr *ph = (Elf64_Phdr *)(file + eh->e_phoff + i * eh->e_phentsize);
        if (ph->p_type != 1) continue;
        uint8_t *dst = base + (ph->p_vaddr - aligned_min);
        if (ph->p_filesz) memcpy(dst, file + ph->p_offset, ph->p_filesz);
        if (ph->p_memsz > ph->p_filesz) memset(dst + ph->p_filesz, 0, ph->p_memsz - ph->p_filesz);
    }

    Elf64_Dyn *dyn = NULL;
    for (Elf64_Half i = 0; i < eh->e_phnum; i++) {
        Elf64_Phdr *ph = (Elf64_Phdr *)(file + eh->e_phoff + i * eh->e_phentsize);
        if (ph->p_type != 2) continue;
        dyn = (Elf64_Dyn *)(base + (ph->p_vaddr - aligned_min));
    }
    m->dynamic = dyn;
    for (Elf64_Dyn *d = dyn; d->d_tag != DT_NULL; d++) {
        switch (d->d_tag) {
        case DT_SYMTAB: m->symtab = (Elf64_Sym *)(m->bias + d->d_un.d_ptr); break;
        case DT_STRTAB: m->strtab = (const char *)(m->bias + d->d_un.d_ptr); break;
        case DT_STRSZ:  m->strsz = d->d_un.d_val; break;
        case DT_RELA:   m->rela    = (Elf64_Rela *)(m->bias + d->d_un.d_ptr); break;
        case DT_RELASZ: m->rela_count = d->d_un.d_val / sizeof(Elf64_Rela); break;
        case DT_JMPREL: m->jmprel  = (Elf64_Rela *)(m->bias + d->d_un.d_ptr); break;
        case DT_PLTRELSZ: m->jmprel_count = d->d_un.d_val / sizeof(Elf64_Rela); break;
        case DT_INIT:   m->init = (void (*)(void))(m->bias + d->d_un.d_ptr); break;
        case DT_INIT_ARRAY: m->init_array = (uint8_t *)(m->bias + d->d_un.d_ptr); break;
        case DT_INIT_ARRAYSZ: m->init_array_sz = d->d_un.d_val; break;
        }
    }

    /* Relocations.  Apply .rela.dyn (DT_RELA) then .rela.plt (DT_JMPREL). */
    int unresolved = 0;
    const Elf64_Rela *rela_sets[2] = { m->rela, m->jmprel };
    const size_t rela_lens[2] = { m->rela_count, m->jmprel_count };
    for (int set = 0; set < 2; set++) {
        for (size_t i = 0; i < rela_lens[set]; i++) {
            Elf64_Rela *r = (Elf64_Rela *)rela_sets[set] + i;
            uint32_t type = r->r_info & 0xffffffffULL;
            uint64_t symidx = r->r_info >> 32;
            uint64_t *slot = (uint64_t *)(m->bias + r->r_offset);
            if (type == R_AARCH64_RELATIVE) {
                *slot = m->bias + r->r_addend;
            } else if (type == R_AARCH64_GLOB_DAT || type == R_AARCH64_JUMP_SLOT ||
                       type == R_AARCH64_ABS64 || type == R_AARCH64_ABS32) {
                const char *name = m->strtab + m->symtab[symidx].st_name;
                void *p = resolve_sym(m, symidx, name);
                if (!p) { if (unresolved++ < 12) printf( "[loader]   unresolved %s\n", name); *slot = 0; }
                else *slot = (uint64_t)p;
            } else if (type == R_AARCH64_COPY) {
                /* handled by host linker; skip */
            }
            /* other types (IRELATIVE, TLS_*) are skipped - not needed to boot
             * init_array in stage 1. */
        }
    }
    if (unresolved) printf( "[loader] %d unresolved symbols in %s\n", unresolved, m->name);

    printf("[loader] %s mapped @%p span=%#zx relas=%zu\n", m->name, (void *)base, span, m->rela_count);

    if (m->init) m->init();
    if (m->init_array) {
        size_t n = m->init_array_sz / sizeof(uint64_t);
        for (size_t i = 0; i < n; i++) {
            uint64_t fn = ((uint64_t *)m->init_array)[i];
            /* Each entry was RELATIVE-relocated to an absolute address above;
             * call it directly (adding m->bias again would double-shift). */
            if (fn) ((void (*)(void))fn)();
        }
        printf("[loader] %s init_array ran (%zu ctors)\n", m->name, n);
    }
    return m;
}

/* aarch64 Linux passes argc/argv in x0/x1 at the entry point (this is true both
 * under the real kernel and under run_aarch64.py, which now sets them).  So the
 * same binary takes the .so path from the command line, e.g.
 *   python3 tools/run_aarch64.py loader/loader2 /abs/path/libil2cpp.so
 */
static void sys_exit0(void) {
    register long x8 __asm__("x8") = 93;   /* exit */
    register long x0 __asm__("x0") = 0;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8) : "memory");
    for (;;) {}
}

/* ---- Stage 2b: drive the IL2CPP runtime directly ----
 * libil2cpp.so exports the il2cpp C embedding API.  Call it the same way
 * kairovm/boot.py does, so the *actual game runtime* initialises and, if we can
 * reach it, the managed entry point runs - even without a GPU (simulation
 * logic, not rendering).  Under the bench fopen/fread resolve via the loader's
 * fd syscalls to the extracted APK, so global-metadata.dat + assemblies load.
 */
typedef void *(*kv_f1)(void);
typedef void *(*kv_f2)(void *, void *);
typedef void *(*kv_f3)(void *, void *, void *);
typedef int   (*kv_fi)(void);
typedef int   (*kv_fc)(const char *);
typedef void *(*kv_fc2)(const char *, const char *);
typedef void *(*kv_fv2)(void *, void *);

static void *kv_il_sym(const char *name) {
    void *p = loader_lookup_export(name);
    if (!p) printf("[il2cpp] missing export %s\n", name);
    return p;
}

/* Set data/config/temp dirs then il2cpp_init, mirroring kairovm/session.py.
 * il2cpp_init returns 1 on success.  data_dir is relative so the bench's openat
 * (which resolves relative paths) finds the assets; on the device it's the
 * data/ dir next to the loader. */
static int kv_il_boot(const char *data_dir) {
    void *(*set_data)(const char *) = kv_il_sym("il2cpp_set_data_dir");
    void *(*set_cfg)(const char *) = kv_il_sym("il2cpp_set_config_dir");
    void *(*set_temp)(const char *) = kv_il_sym("il2cpp_set_temp_dir");
    void *(*set_cli)(int, void *, void *) = kv_il_sym("il2cpp_set_commandline_arguments");
    int (*init)(const char *) = kv_il_sym("il2cpp_init");
    if (!set_data || !init) return -1;
    if (data_dir) {
        printf("[il2cpp] data_dir=%s\n", data_dir);
        set_data(data_dir);
    }
    if (set_cfg) set_cfg("etc");
    if (set_temp) set_temp("/tmp");
    if (set_cli) {
        /* argv = {"GameDevStory"} in guest memory */
        char **av = calloc(2, sizeof(char *));
        av[0] = strdup("GameDevStory");
        set_cli(1, av, 0);
    }
    printf("[il2cpp] calling il2cpp_init ...\n");
    int rc = init("IL2CPP Root Domain");
    printf("[il2cpp] il2cpp_init -> %d\n", rc);
    if (rc != 1) { printf("[il2cpp] WARNING: expected rc==1\n"); }
    return rc;
}

/* After init: disable the stop-the-world GC (green threads; collection stays
 * off, as in kairovm), attach the current thread, then find and invoke the
 * game's managed entry point. */
static void kv_il_run_entry(void) {
    void *(*gc_disable)(void) = kv_il_sym("il2cpp_gc_disable");
    void *(*domain_get)(void) = kv_il_sym("il2cpp_domain_get");
    void *(*thread_attach)(void *) = kv_il_sym("il2cpp_thread_attach");
    void *(*asm_open)(void *, const char *) = kv_il_sym("il2cpp_domain_assembly_open");
    void *(*asm_img)(void *) = kv_il_sym("il2cpp_assembly_get_image");
    void *(*img_entry)(void *) = kv_il_sym("il2cpp_image_get_entry_point");
    void *(*runtime_invoke)(void *, void *, void *, void **) = kv_il_sym("il2cpp_runtime_invoke");
    void *(*method_name)(void *) = kv_il_sym("il2cpp_method_get_name");
    if (gc_disable) gc_disable();
    void *dom = domain_get ? domain_get() : 0;
    if (!dom) { printf("[il2cpp] no domain\n"); return; }
    if (thread_attach) { void *t = thread_attach(dom); (void)t; }
    if (!asm_open || !asm_img || !img_entry || !runtime_invoke) {
        printf("[il2cpp] entry-point symbols unavailable, skipping\n");
        return;
    }
    /* Try the game's own assembly first, then the default "Assembly-CSharp". */
    const char *asms[] = { "Assembly-CSharp", "Assembly-CSharp-firstpass", 0 };
    void *img = 0;
    for (int i = 0; asms[i] && !img; i++) {
        void *a = asm_open(dom, asms[i]);
        if (a) img = asm_img(a);
    }
    if (!img) { printf("[il2cpp] no game image opened\n"); return; }
    void *mth = img_entry(img);
    if (!mth) { printf("[il2cpp] no entry point\n"); return; }
    printf("[il2cpp] invoking entry point (%s)\n",
           method_name ? (const char *)method_name(mth) : "?");
    void **exc = (void **)calloc(1, 8);
    void *r = runtime_invoke(mth, 0, 0, exc);
    if (exc && *exc) printf("[il2cpp] managed exception during entry: %p\n", *exc);
    else printf("[il2cpp] entry point returned %p\n", r);
}

/* ---- Stage 2c: drive Unity's player loop (the correct boot path) ----
 * Following terraria-nextos: after JNI_OnLoad registers the native methods,
 * call initJni then loop nativeRender.  This drives the whole engine (including
 * il2cpp init internally) instead of calling il2cpp_init directly, which hits
 * uninitialized globals. */
static void kv_unity_boot(void) {
    void *env = kv_jni_env();
    long thiz = 0xA1;
    unsigned char (*render)(void *, void *) = (unsigned char (*)(void *, void *))kv_jni_find_native("nativeRender");
    if (!render) { printf("[unity] no nativeRender registered - cannot drive player loop\n"); return; }
    void *fn;
    if ((fn = kv_jni_find_native("initJni"))) {
        printf("[unity] initJni...\n");
        ((void (*)(void *, void *, void *))fn)(env, &thiz, &thiz);
        printf("[unity] initJni OK\n");
    }
    /* Surface lifecycle: nativeRecreateGfxState installs the GL surface that
     * nativeRender draws into; without it Unity hits a null surface.  Called
     * twice (surfaceCreated + surfaceChanged), then surface-changed event. */
    void *surf = (void *)0x5F, *ctx = (void *)0xC0;
    if ((fn = kv_jni_find_native("nativeRecreateGfxState"))) {
        printf("[unity] nativeRecreateGfxState...\n");
        ((void (*)(void *, void *, int, void *))fn)(env, &thiz, 0, &surf);
        ((void (*)(void *, void *, int, void *))fn)(env, &thiz, 0, &surf);
        printf("[unity] nativeRecreateGfxState OK\n");
    }
    if ((fn = kv_jni_find_native("nativeSendSurfaceChangedEvent"))) {
        ((void (*)(void *, void *))fn)(env, &thiz);
    }
    if ((fn = kv_jni_find_native("nativeResume"))) {
        ((void (*)(void *, void *))fn)(env, &thiz);
    }
    if ((fn = kv_jni_find_native("nativeFocusChanged"))) {
        ((void (*)(void *, void *, int))fn)(env, &thiz, 1);
    }
    printf("[unity] nativeRender loop...\n");
    for (int f = 0; f < 1000000; f++) {
        unsigned char keep = render(env, &thiz);
        if (!keep) { printf("[unity] nativeRender requested quit at frame %d\n", f); break; }
    }
    printf("[unity] player loop ended\n");
}

int real_main(int argc, char **argv);

/* ---- TLS setup ----
 * libunity.so's init_array reads the current thread's stack bounds + stack
 * guard from thread-local storage via tpidr_el0 (mrs x19, tpidr_el0; then
 * reads slots).  On the bench this was set up by the harness; on the REAL
 * device the loader must set up its own TLS block and point tpidr_el0 at it,
 * because the kernel's initial TLS (intended for glibc) has no thread info.
 * tpidr_el0 is user-writable, so we do it directly.
 *
 * bionic/il2cpp TLS layout (8-byte slots from tpidr_el0):
 *   slot 1 (off 0x08) thread id
 *   slot 5 (off 0x28) stack guard magic (GC checks this)
 *   slot 6 (off 0x30) stack lo
 *   slot 7 (off 0x38) stack hi
 */
static void kv_setup_tls(void) {
    static char tls[0x100] __attribute__((aligned(16)));
    /* current stack pointer (stack grows down; main thread stack is below it) */
    unsigned long sp;
    __asm__ volatile("mov %0, sp" : "=r"(sp));
    unsigned long hi = (sp + 0x400000) & ~0xffffUL;   /* ~4 MB headroom above sp */
    unsigned long lo = (sp - 0x800000) & ~0xffffUL;   /* ~8 MB below sp */
    *(unsigned long *)(tls + 0x08) = 1;                          /* thread id */
    *(unsigned long *)(tls + 0x28) = 0x0BADC0DEDEADBEEFUL;       /* stack guard */
    *(unsigned long *)(tls + 0x30) = lo;
    *(unsigned long *)(tls + 0x38) = hi;
    unsigned long tp = (unsigned long)tls;
    __asm__ volatile("msr tpidr_el0, %0" :: "r"(tp));
    printf("[loader] TLS set: tp=%#lx lo=%#lx hi=%#lx\n", tp, lo, hi);
}

void _start(void) {
    /* Read the aarch64 Linux entry registers before anything can clobber x0/x1.
     * (register-asm locals get spilled to the stack at -O0, so we read x0/x1
     * directly via `mov` and pass the values as plain C locals.) */
    long argc;
    char **argv;
    __asm__ volatile("mov %0, x0" : "=r"(argc));
    __asm__ volatile("mov %0, x1" : "=r"(argv));
    /* Terminate the frame-pointer chain: glibc's _start zeroes x29 so that a
     * GC stack-walk (which follows saved x29/frame records) stops at NULL
     * instead of chasing garbage past the top of the stack.  Without this the
     * IL2CPP GC scan runs off the mapped stack. */
    __asm__ volatile("mov x29, xzr");
    kv_install_crash_handler();
    kv_setup_tls();
    real_main((int)argc, argv);
    sys_exit0();   /* stage-1 success path: return cleanly with exit code 0 */
}

/* Resolve `path` against the directory of argv[0] (the loader executable), so
 * the game can be dropped in a folder next to the loader without cwd magic.
 * Returns the joined path in a static buffer. */
static const char *kv_abspath(const char *argv0, const char *path) {
    static char buf[3][512];   /* one per slot so the three calls don't clobber */
    static int idx = 0;
    char *dst = buf[idx++ % 3];
    const char *slash = 0;
    for (const char *p = argv0; *p; p++) if (*p == '/') slash = p;
    unsigned i = 0;
    if (slash) {
        unsigned n = (unsigned)(slash - argv0) + 1;
        if (n > 480) n = 480;
        for (i = 0; i < n; i++) dst[i] = argv0[i];
    }
    const char *s = path;
    while (*s && i < 510) dst[i++] = *s++;
    dst[i] = 0;
    return dst;
}

int real_main(int argc, char **argv) {
    /* Device layout (drop next to the loader on the R36S SD card):
     *   loader2
     *   libil2cpp.so   libunity.so   libmain.so
     *   data/          (the APK's assets/bin/Data, or the whole extracted APK)
     * Defaults resolve relative to argv[0]; override with explicit args:
     *   loader2 libil2cpp.so libunity.so libmain.so
     */
    const char *argv0 = argc > 0 && argv[0] ? argv[0] : "loader2";
    /* Mirror all output into a log file next to the executable, so a device
     * test always produces a diagnostic log even if the shell can't capture it.
     */
    kv_log_open(kv_abspath(argv0, "loader.log"));
    printf("[loader] === Game Dev Story native loader ===\n");
    const char *libs[3];
    int libc = 0;
    if (argc >= 4) {
        libs[0] = argv[1]; libs[1] = argv[2]; libs[2] = argv[3]; libc = 3;
    } else {
        libs[0] = kv_abspath(argv0, "libil2cpp.so");
        libs[1] = kv_abspath(argv0, "libunity.so");
        libs[2] = kv_abspath(argv0, "libmain.so");
        libc = 3;
    }
    int n = 0;
    Module *m_il2cpp = 0, *m_unity = 0, *m_main = 0;
    for (int i = 0; i < libc; i++) {
        printf("[loader] loading %s\n", libs[i]);
        Module *m = load_object(libs[i]);
        if (m) {
            n++;
            if (strstr(m->name, "libil2cpp")) m_il2cpp = m;
            else if (strstr(m->name, "libunity")) m_unity = m;
            else if (strstr(m->name, "libmain")) m_main = m;
        }
    }
    printf("[loader] OK: %d module(s) loaded and initialised\n", n);

    /* Stage 2: Unity boot.  JNI_OnLoad for EACH lib is what registers the
     * native methods.  libunity's JNI_OnLoad registers initJni/nativeRender/
     * nativeResume/... (needed to drive the player loop); libil2cpp's and
     * libmain's register their own.  Order matters (libunity first). */
    void *vm = kv_jni_java_vm();
    void *(*onload)(void *, void *);
    if (m_unity && (onload = module_export(m_unity, "JNI_OnLoad"))) {
        printf("[loader] libunity JNI_OnLoad...\n");
        printf("[loader]   -> %#lx\n", (unsigned long)onload(vm, 0));
    }
    if (m_il2cpp && (onload = module_export(m_il2cpp, "JNI_OnLoad"))) {
        printf("[loader] libil2cpp JNI_OnLoad...\n");
        printf("[loader]   -> %#lx\n", (unsigned long)onload(vm, 0));
    }
    if (m_main && (onload = module_export(m_main, "JNI_OnLoad"))) {
        printf("[loader] libmain JNI_OnLoad...\n");
        printf("[loader]   -> %#lx\n", (unsigned long)onload(vm, 0));
    }

    /* Stage 2b/c: drive Unity's player loop (correct boot path per
     * terraria-nextos).  initJni + nativeRender loop boots il2cpp internally. */
    kv_unity_boot();
    return 0;
}
