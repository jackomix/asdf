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

/* kv_libc.h declares fopen/printf/etc. with freestanding signatures, so we
 * can't include the real <stdio.h>.  It does declare setvbuf/fflush (void*
 * FILE, which is ABI-compatible).  Declare just the stdout/stderr globals we
 * need to keep the fresh loader.log unbuffered (so a hard crash doesn't drop
 * the banner). */
extern void *stdout;
extern void *stderr;
#define _IONBF 2

/* Raw syscall for kv_pin_to_one_cpu (kv_libc.h doesn't declare syscall). */
extern long syscall(long n, ...);

/* Declarations for the watchdog (manual, no <dirent.h> due to the
 * freestanding kv_libc.h decls).  pthread_create/pthread_detach/nanosleep/
 * snprintf are already declared in kv_libc.h.  ABI-compatible with glibc. */
typedef struct DIR DIR;
struct dirent {
    long d_ino;                /* offset 0  */
    long d_off;                /* offset 8  */
    unsigned short d_reclen;   /* offset 16 */
    unsigned char d_type;      /* offset 18 */
    char d_name[256];          /* offset 19 (glibc aarch64 layout) */
};
extern DIR *opendir(const char *path);
extern struct dirent *readdir(DIR *dirp);
extern int closedir(DIR *dirp);
typedef unsigned long kv_pthread_t;   /* matches kv_libc.h */
struct timespec { long tv_sec; long tv_nsec; };

/* ptrace approach failed on darkOSre R36S (every attach returns EPERM even
 * from same-process sibling threads — prctl PR_SET_PTRACER doesn't help
 * without yama either; the kernel appears hardened against self-attach).
 *
 * BUT: /proc/<tid>/syscall already prints the user-space PC + SP as its
 * LAST TWO fields (after the 6 syscall args: syscall#, arg0..arg5, sp, pc).
 * So we have the PC.  To turn it into a (file, offset) we just need the
 * current /proc/self/maps snapshot; we read it ONCE per dump and resolve
 * each thread's PC against it. */
/* Stream /proc/self/maps line-by-line on demand.  Previous static buffer
 * approach truncated at 64KB/256KB and missed the libc.so.6 r-xp segment
 * at the top of the file (printed rw-p/r--p tail only), so the resolved
 * offset fell in the wrong segment.  See also maps_resolve(). below. */

static const char *maps_resolve(unsigned long addr, unsigned long *off_out) {
    /* Stream /proc/self/maps line-by-line.  Avoids buffer truncation, gives
     * full coverage of all mapped segments including libc.so.6 r-xp line at
     * the top (which the static 64/256KB snapshot was eating).  Path stored
     * in static per-call line buffer, NUL-terminated, returned to caller. */
    static char line[640];
    int fd = open("/proc/self/maps", O_RDONLY);
    if (fd < 0) return 0;
    const char *lib = 0; unsigned long off = 0;
    int total = 0;
    char readbuf[1024];
    /* read chunks; newline ends a line; parse each. */
    while (1) {
        ssize_t n = read(fd, readbuf, sizeof readbuf);
        if (n <= 0) break;
        for (ssize_t i = 0; i < n; i++) {
            if (readbuf[i] == '\n' || total >= (int)sizeof line - 1) {
                line[total] = 0;
                total = 0;
            } else {
                line[total++] = readbuf[i];
                continue;
            }
            /* parse one maps line: "lo-hi perm offset dev inode pathname".
             * Use 5 single-space-delimited tokens then skip whitespace to pathname. */
            char *p = line, *t[5];
            for (int ti = 0; ti < 5; ti++) {
                while (*p == ' ') p++;
                t[ti] = p;
                while (*p && *p != ' ') p++;
                if (!*p) goto next_line;
                *p++ = 0;
            }
            /* t[0]='lo-hi', t[1]=perm, t[2]=offset, t[3]=dev, t[4]=inode.
             * parse lo-hi by splitting on '-'. */
            char *dash = strchr(t[0], '-'); if (!dash) goto next_line;
            *dash = 0;
            unsigned long lo = strtoul(t[0], 0, 16);
            unsigned long hi = strtoul(dash + 1, 0, 16);
            while (*p == ' ') p++;
            if (addr >= lo && addr < hi && *p) {
                lib = p; off = addr - lo;
                char *nl = strchr(p, '\n'); if (!nl) nl = p + strlen(p);
                *nl = 0;
                printf("[wd-resolve] hit lo=%lx hi=%lx path=%s off=%lx\n", lo, hi, p, off);
            }
        next_line:;
        }
    }
    close(fd);
    if (off_out) *off_out = off;
    return lib;
}


/* Bump this on every release so gds_deploy.sh can verify the device has the
 * latest loader (and so we can tell stale zips apart in logs). */
#define GDS_BUILD_VERSION "0.42.1-glibc"

/* JNI shim (jni_shim.c) - provides the JavaVM/JNIEnv the engine's JNI_OnLoad
 * needs.  Declared here so loader.c can drive the Unity boot. */
void *kv_jni_java_vm(void);
void *kv_jni_env(void);
void *kv_jni_find_native(const char *name);
void kv_set_asset_dir(const char *dir);
void kv_set_game_dir(const char *dir);      /* jni_shim.c */
void kv_fs_set_data_dir(const char *dir);   /* fs_redirect.c */
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

void *kv_egl_route(const char *name);
void *kv_bionic_route(const char *name);
void *kv_fs_route(const char *name);
int kv_is_main_thread(void);

void *kv_dlsym(void *handle, const char *name) {
    if (name) {
        if (strcmp(name, "dlsym") == 0) return (void *)kv_dlsym;
        void *r = kv_egl_route(name);
        if (r) return r;
        r = kv_bionic_route(name);
        if (r) return r;
        r = kv_fs_route(name);
        if (r) return r;
    }
    return dlsym(handle, name);
}

static void *resolve(const char *sym) {
    if (sym) {
        if (strcmp(sym, "dlsym") == 0) return (void *)kv_dlsym;
        void *r = kv_egl_route(sym);
        if (r) return r;
        r = kv_bionic_route(sym);
        if (r) return r;
        r = kv_fs_route(sym);
        if (r) return r;
    }
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

void *kv_il_sym(const char *name) {
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
void egl_shim_create_window(void);   /* egl_shim.c */
int egl_shim_ensure_current(void);

/* ---- watchdog: dump all threads' blocked syscall after a timeout ----
 * The boot hangs inside the first nativeRender with NO further JNI calls and NO
 * crash - Unity is blocked on a native wait (pthread/futex/cond) that no JNI
 * shim reaches.  This thread wakes after N seconds and prints each thread's
 * /proc/self/task/<tid>/syscall (the syscall + args, e.g. futex/cond_wait) and
 * wchan, so we can see exactly where the process is stuck. */
static void *kv_watchdog(void *arg) {
    (void)arg;
    for (int dump = 0; dump < 5; dump++) {
        struct timespec ts; ts.tv_sec = 12; ts.tv_nsec = 0;
        nanosleep(&ts, 0);
        printf("[watchdog] === thread dump #%d (blocked syscalls + wchan + kstack) ===\n", dump);
    DIR *d = opendir("/proc/self/task");
    if (!d) { printf("[watchdog] cannot open /proc/self/task\n"); return 0; }
    struct dirent *de;
    while ((de = readdir(d))) {
        if (de->d_name[0] == '.') continue;
        char p[128], buf[512];
        char sysbuf[512] = "";   /* raw /proc/<tid>/syscall text */
        /* state: /proc/self/task/<tid>/stat, field 3 (R running, S sleeping,
         * D io-wait).  comm is field 2 in parens; find last ')' then skip space. */
        snprintf(p, sizeof p, "/proc/self/task/%s/stat", de->d_name);
        int fd = open(p, O_RDONLY);
        if (fd >= 0) {
            ssize_t n = read(fd, buf, sizeof buf - 1);
            close(fd);
            if (n > 0) {
                buf[n] = 0;
                char *rp = strrchr(buf, ')');
                if (rp) printf("[watchdog] tid=%s state=%c\n", de->d_name, rp[2]);
                else printf("[watchdog] tid=%s state=?\n", de->d_name);
            }
        }
        snprintf(p, sizeof p, "/proc/self/task/%s/syscall", de->d_name);
        fd = open(p, O_RDONLY);
        if (fd >= 0) {
            ssize_t n = read(fd, buf, sizeof buf - 1);
            close(fd);
            if (n > 0) { buf[n] = 0; printf("[watchdog] tid=%s syscall: %s", de->d_name, buf); snprintf(sysbuf, sizeof sysbuf, "%s", buf); }
        }
        /* From the raw syscall we can extract PC (user-space return IP after
         * `svc`).  Format: "98 arg0 arg1 arg2 arg3 arg4 arg5 sp pc\n".
         * Tokenize by whitespace, count to 9th, parse as hex. */
        if (sysbuf[0]) {
            char *tok = sysbuf; int idx = 0; unsigned long pc = 0, sp = 0;
            for (; idx < 9; idx++) {
                while (*tok == ' ') tok++;
                if (!*tok || *tok == '\n') break;
                char *e = tok;
                while (*e && *e != ' ' && *e != '\n') e++;
                if (idx == 7) sp = strtoul(tok, 0, 16);
                if (idx == 8) pc = strtoul(tok, 0, 16);
                tok = e;
            }
            if (pc) {
                unsigned long off = 0;
                const char *lib = maps_resolve(pc, &off);
                printf("[watchdog] tid=%s user-pc=0x%lx sp=0x%lx  in %s +0x%lx\n",
                       de->d_name, pc, sp, lib ? lib : "[unknown]", off);
            }
        }
        /* wchan: kernel wait site name (e.g. "futex_wait_queue_me"). */
        snprintf(p, sizeof p, "/proc/self/task/%s/wchan", de->d_name);
        fd = open(p, O_RDONLY);
        if (fd >= 0) {
            ssize_t n = read(fd, buf, sizeof buf - 1);
            close(fd);
            if (n > 0) { buf[n] = 0; if (buf[n-1] == '\n') buf[n-1] = 0; printf(" wchan=%s\n", buf); }
            else printf(" wchan=?\n");
        }
        /* kernel stack: shows the call chain inside the kernel (must be root
         * or have /proc/sys/kernel/yama/ptrace_scope <= 1; on darkOSre R36S the
         * ark user typically has it).  Truncated to 4 lines to bound log. */
        snprintf(p, sizeof p, "/proc/self/task/%s/stack", de->d_name);
        fd = open(p, O_RDONLY);
        if (fd >= 0) {
            ssize_t n = read(fd, buf, sizeof buf - 1);
            close(fd);
            if (n > 0) {
                buf[n] = 0;
                int lines = 0;
                char *q = buf, *nl;
                while (lines < 5 && q && *q && (nl = strchr(q, '\n'))) {
                    *nl = 0;
                    printf("[watchdog] tid=%s kstack: %s\n", de->d_name, q);
                    q = nl + 1; lines++;
                }
            }
        }
    }
    closedir(d);
    printf("[watchdog] === end dump #%d ===\n", dump);
    }
    return 0;
}
static void kv_start_watchdog(void) {
    kv_pthread_t t;
    if (pthread_create(&t, 0, kv_watchdog, 0) == 0) pthread_detach(t);
}

/* ---- TER_JOBWORKERS0: force Unity's job system to run inline ----
 * Unity sizes its job-worker pool from the CPU count, but under our loader the
 * workers deadlock (they call glibc's internal futex which we can't intercept).
 * The horizonchase-nextos port solves this by calling
 *   JobsUtility.set_JobWorkerCount(0)
 *   JobsUtility.SetJobQueueMaximumActiveThreadCount(0)
 *   JobsUtility.SetJobQueueMaximumWarpThreadCount(0)
 * via il2cpp_runtime_invoke after the runtime is initialized.  This tells Unity
 * to run all jobs inline on the calling thread — no workers needed.
 *
 * We call this lazily from the nativeRender loop because il2cpp_init runs
 * inside the first nativeRender call, so the domain/assemblies aren't available
 * until after at least one render frame. */
static int kv_jobworkers_done = 0;
/* Called from kv_syscall (bionic_bridge.c) to avoid re-firing the jobfix once
 * it has already been attempted/completed. */
int kv_jobworkers_is_done(void) { return kv_jobworkers_done; }
void *kv_set_job_workers_zero(void *unused) {
    (void)unused;
    int (*il_init_void)(const char *) = (int (*)(const char *))kv_il_sym("il2cpp_init");
    /* Unity's il2cpp_init returns int (0=ok), but il2cpp_domain_get returns
     * void* — we just check non-NULL.  il2cpp_init may already be in flight by
     * Unity's main thread under nativeRender; calling it again from a sibling
     * thread is safe (it's idempotent and guarded). */
    void *(*dom_get)(void) = kv_il_sym("il2cpp_domain_get");
    void *(*dom_asms)(void *, size_t *) = kv_il_sym("il2cpp_domain_get_assemblies");
    void *(*asm_img)(void *) = kv_il_sym("il2cpp_assembly_get_image");
    void *(*cls_from_name)(void *, const char *, const char *) = kv_il_sym("il2cpp_class_from_name");
    void *(*cls_method)(void *, const char *, int) = kv_il_sym("il2cpp_class_get_method_from_name");
    void *(*rt_invoke)(void *, void *, void **, void **) = kv_il_sym("il2cpp_runtime_invoke");
    void *(*thread_attach)(void *) = kv_il_sym("il2cpp_thread_attach");
    if (!dom_get || !dom_asms || !asm_img || !cls_from_name || !cls_method || !rt_invoke) {
        printf("[jobfix] IL2CPP symbols unavailable, skipping\n");
        return 0;
    }
    /* Stage A: Wait until il2cpp_init succeeds.  Calling il2cpp_init from a
     * sibling thread is safe; it returns immediately if already initialized
     * (Unity calls it from nativeRender on main thread).  This guarantees the
     * runtime + metadata is fully loaded BEFORE we call class_from_name —
     * without this gate, the lookup races with the in-flight init and
     * mono_class_get_checked gets a NULL MonoClass* and SIGSEGVs (0.32, 0.38a).
     * Poll for up to 30s — the crash handler won't catch us if we disappear. */
    printf("[jobfix] waiting for il2cpp_init...\n");
    /* On the MAIN thread: drive il2cpp_init directly (idempotent with Unity's
     * own later call inside nativeRender).  This guarantees dom_get() returns
     * a valid domain and we have a thread context attached. */
    if (kv_is_main_thread()) {
        /* Unity's il2cpp_set_data_dir isn't exported, so we rely on Unity's
         * OWN il2cpp_init inside nativeRender (which has already started by
         * the time kv_pthread_cond_wait fires on main).  No pre-init here. */
    }
    /* Stage A: Wait for il2cpp domain to exist (Unity's own il2cpp_init runs
     * inside nativeRender, in parallel with this polling spin).  Do NOT call
     * il2cpp_init ourselves—rerunning it mid-Unity-init causes deadlock /
     * corruption. Just wait for dom_get() to return non-NULL. */
    printf("[jobfix] waiting for Unity's il2cpp_init (dom_get)...\n");
    for (int tries = 0; tries < 600; tries++) {
        if (dom_get && dom_get()) break;
        struct timespec ts = {0,50000000}; nanosleep(&ts, 0);
    }
    if (!dom_get || !dom_get()) {
        printf("[jobfix] no domain after 30s — giving up\n");
        return 0;
    }
    printf("[jobfix] il2cpp_init done, scanning assemblies for JobsUtility\n");
    fflush(stdout);
    /* Stage A.5: il2cpp_thread_attach is REQUIRED before cls_from_name can
     * safely run on a NON-MAIN thread.  Haupt-thread is auto-attached by
     * Unity's own il2cpp_init (which we drove above).  Sibling threads must
     * call il2cpp_thread_attach(NULL) — but that triggers Unity's assertion
     * "Threads explicit registering is not previously enabled" and aborts.
     * So we DON'T run this from the sibling thread; we run from main only. */
    if (!kv_is_main_thread()) {
        printf("[jobfix] not main thread - skipping class scan to avoid mono assertion\n");
        fflush(stdout);
        return 0;
    }
    printf("[jobfix] main thread - proceeding to class scan\n"); fflush(stdout);
    /* Stage B: scan assemblies, find JobsUtility, invoke setters. */
    for (int tries = 0; tries < 200 && !kv_jobworkers_done; tries++) {
        if (tries == 0) { printf("[jobfix] calling dom_get()...\n"); fflush(stdout); }
        void *domain = dom_get();
        if (tries == 0) { printf("[jobfix] dom_get() -> %p\n", domain); fflush(stdout); }
        if (!domain) { struct timespec ts={0,50000000}; nanosleep(&ts,0); continue; }
        size_t na = 0;
        if (tries == 0) { printf("[jobfix] calling dom_asms()...\n"); fflush(stdout); }
        void **asms = (void **)dom_asms(domain, &na);
        if (tries == 0) { printf("[jobfix] dom_asms() -> %p na=%zu\n", asms, na); fflush(stdout); }
        if (!asms || !na) { struct timespec ts={0,50000000}; nanosleep(&ts,0); continue; }
        printf("[jobfix] assembly count=%zu\n", na); fflush(stdout);
        int found_class = 0;
        for (size_t i = 0; i < na; i++) {
            void *img = asm_img(asms[i]);
            if (!img) continue;
            if (tries == 0) { printf("[jobfix] asm[%zu] img=%p\n", i, img); fflush(stdout); }
            void *cls = cls_from_name(img, "Unity.Jobs.LowLevel.Unsafe", "JobsUtility");
            if (!cls) continue;
            found_class = 1;
            int zero = 0;
            void *params[1] = { &zero };
            void *exc = NULL;
            const char *setters[] = {
                "set_JobWorkerCount",
                "SetJobQueueMaximumActiveThreadCount",
                "SetJobQueueMaximumWarpThreadCount"
            };
            int any = 0;
            for (unsigned s = 0; s < sizeof(setters)/sizeof(setters[0]); s++) {
                void *m = cls_method(cls, setters[s], 1);
                if (!m) continue;
                exc = NULL;
                rt_invoke(m, NULL, params, &exc);
                printf("[jobfix] %s(0) invoked (exc=%p)\n", setters[s], exc);
                any = 1;
            }
            if (any) {
                printf("[jobfix] job workers set to 0 — jobs will run inline\n");
                kv_jobworkers_done = 1;
                return 0;
            }
        }
        if (found_class) {
            printf("[jobfix] JobsUtility found but no setter invoked — giving up\n");
            kv_jobworkers_done = 1;
            return 0;
        }
        struct timespec ts = {0,50000000}; nanosleep(&ts,0);
    }
    if (!kv_jobworkers_done) printf("[jobfix] gave up (il2cpp never ready or JobsUtility not found)\n");
    return 0;
}

static void kv_unity_boot(void) {
    void *env = kv_jni_env();
    /* Give Unity real, zeroed thiz/ctx/surf objects instead of tiny fake
     * addresses.  nativeRecreateGfxState does things like
     *   x0 = [thiz+0x148]; strb 1,[x0]
     * so thiz must be a writable buffer and [thiz+0x148] must be a valid
     * pointer (here: 0).  A zeroed 0x200-byte buffer keeps those reads safe. */
    static unsigned char thiz[0x200];
    static unsigned char ctx[0x200], surf[0x200];
    void *thizp = thiz;
    unsigned char (*render)(void *, void *) = (unsigned char (*)(void *, void *))kv_jni_find_native("nativeRender");
    if (!render) { printf("[unity] no nativeRender registered - cannot drive player loop\n"); return; }

    /* THE fix for nativeRecreateGfxState (the graphics-init wall): per
     * terraria-nextos, create the REAL SDL2 window + GLES2 context and make it
     * current BEFORE Unity's graphics init runs, and route libunity's
     * egl + ANativeWindow imports to that real context.  Without a current GL
     * context Unity's graphics init derefs garbage and SIGSEGVs. */
    egl_shim_create_window();
    egl_shim_ensure_current();
    /* terraria-nextos RE-ARMS on_crash here: SDL_Init(VIDEO) on kmsdrm and the
     * Mali blob's libEGL.so loader both install their OWN default SIGSEGV
     * handler during window/context creation, OVERWRITING ours.  Without this
     * re-install, a segfault in nativeRecreateGfxState or nativeRender goes to
     * the SDL/Mali default action (exit 139, silent), not our on_crash dumper.
     * Even though we set ours in main(), we have to set it again RIGHT here,
     * between GPU init and the graphics-heavy native calls. */
    kv_install_crash_handler();

    void *fn;
    if ((fn = kv_jni_find_native("initJni"))) {
        printf("[unity] initJni...\n");
        ((void (*)(void *, void *, void *))fn)(env, thizp, ctx);
        printf("[unity] initJni OK\n");
    }
    /* Surface lifecycle: nativeRecreateGfxState installs the GL surface that
     * nativeRender draws into; without it Unity hits a null surface.  Called
     * twice (surfaceCreated + surfaceChanged), then surface-changed event. */
    if ((fn = kv_jni_find_native("nativeRecreateGfxState"))) {
        printf("[unity] nativeRecreateGfxState...\n");
        ((void (*)(void *, void *, int, void *))fn)(env, thizp, 0, surf);
        ((void (*)(void *, void *, int, void *))fn)(env, thizp, 0, surf);
        printf("[unity] nativeRecreateGfxState OK\n");
    }
    if ((fn = kv_jni_find_native("nativeSendSurfaceChangedEvent"))) {
        ((void (*)(void *, void *))fn)(env, thizp);
    }
    if ((fn = kv_jni_find_native("nativeResume"))) {
        ((void (*)(void *, void *))fn)(env, thizp);
    }
    if ((fn = kv_jni_find_native("nativeFocusChanged"))) {
        ((void (*)(void *, void *, int))fn)(env, thizp, 1);
    }
    printf("[unity] nativeRender loop...\n");
    kv_start_watchdog();   /* dump blocked threads if we hang in the loop */
    /* 0.39.5: jobzero is now invoked from kv_pthread_cond_wait the FIRST time
     * it's called on the main thread (main reaches cond_wait inside the
     * first nativeRender AFTER Unity's il2cpp_init has completed).  By then
     * cur main thread context is fully attached, dom_get works, and
     * il2cpp_runtime_invoke(set_JobWorkerCount, 0) succeeds.  Spurious-return-0
     * from cond_wait also unblocks Unity's predicate loop. */
    for (int f = 0; f < 1000000; f++) {
        unsigned char keep = render(env, thizp);
        if (!keep) { printf("[unity] nativeRender requested quit at frame %d\n", f); break; }
        /* job fix is handled by the fixer thread spawned before the loop */

        /* Periodic liveness: confirms nativeRender is actually looping (and not
         * stuck inside one call).  Also tells us how fast frames are being
         * produced relative to real time. */
        if (f == 1 || f == 60 || f == 600 || (f > 0 && f % 6000 == 0))
            printf("[unity] nativeRender frame %d alive\n", f);
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
/* ---- Bionic TLS guard pad ---- *
 * libil2cpp/libunity are bionic binaries: they read the bionic thread-info
 * slots directly off tpidr_el0 (thread id @+0x08, stack guard @+0x28, stack
 * lo @+0x30, stack hi @+0x38).  Under glibc, tpidr_el0 points at glibc's TLS,
 * which has a different layout - so those reads get garbage, and the GC can
 * make bad allocations.  We CANNOT overwrite tpidr_el0 (that breaks glibc's
 * TLS -> pc=0 crash).  Instead, reserve a 256-byte TLS variable that, being
 * the first TLS var in this executable, lands right after the glibc TCB at
 * tpidr_el0+0x28 - exactly where bionic reads its slots - and pre-fill the
 * bionic slots inside it (same trick as terraria-nextos g_bionic_guard_pad). */
__attribute__((aligned(16))) static _Thread_local unsigned char kv_bionic_pad[256] = {1};

static void kv_setup_tls(void) {
    /* Keep glibc's tpidr_el0 (do NOT msr it - that broke glibc).  The bionic
     * slots that libil2cpp reads are at tp+0x08/+0x28/+0x30/+0x38; our
     * 256-byte TLS pad must cover the ones after the TCB.  Initialize the
     * bionic stack-guard + stack bounds within it. */
    unsigned long tp;
    __asm__ volatile("mrs %0, tpidr_el0" : "=r"(tp));
    unsigned long pad_lo = (unsigned long)kv_bionic_pad;
    unsigned long pad_hi = pad_lo + sizeof(kv_bionic_pad);
    /* bionic stack-guard slot @ tp+0x28, stack lo/hi @ tp+0x30/+0x38 */
    if (tp + 0x38 + 8 <= pad_hi && tp + 0x28 >= pad_lo) {
        unsigned long sp;
        __asm__ volatile("mov %0, sp" : "=r"(sp));
        unsigned long hi = (sp + 0x400000) & ~0xffffUL;
        unsigned long lo = (sp - 0x800000) & ~0xffffUL;
        *(unsigned long *)(tp + 0x28) = 0x0BADC0DEDEADBEEFUL;   /* stack guard */
        *(unsigned long *)(tp + 0x30) = lo;
        *(unsigned long *)(tp + 0x38) = hi;
        printf("[loader] bionic TLS slots set in guard pad (tp=%#lx pad=[%#lx..%#lx])\n",
               tp, pad_lo, pad_hi);
    } else {
        printf("[loader] WARNING: bionic TLS slots (tp+0x28=%#lx) outside guard pad [%#lx..%#lx]\n",
               tp + 0x28, pad_lo, pad_hi);
    }
}

void kv_egl_dlopen(void);   /* glibc_shims.c: load real Mali GPU drivers */
void kv_ctype_init(void);  /* glibc_shims.c: fill bionic _ctype_/_tolower_tab_/_toupper_tab_ */
int main(int argc, char **argv) {
    kv_ctype_init();            /* fill tables BEFORE any libunity/libil2cpp ctor runs
                                 * (ctors read these via the GOT).  Without this the
                                 * old empty-function stub crashes the loader in
                                 * nativeRender during string processing. */
    kv_install_crash_handler();
    kv_setup_tls();
    kv_egl_dlopen();        /* dlopen libEGL/libGLESv2/SDL2 RTLD_GLOBAL */
    return real_main(argc, argv);
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
    /* stdout/stderr now point at the fresh loader.log (kv_log_open dup2'd
     * them).  Keep stdout unbuffered so a hard crash doesn't lose the lines
     * that led up to it. */
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    kv_set_asset_dir(kv_abspath(argv0, "data"));
    kv_set_game_dir(kv_abspath(argv0, "."));
    kv_fs_set_data_dir(kv_abspath(argv0, "data"));
    printf("[loader] === Game Dev Story native loader ===\n");
    printf("[loader] build: %s\n", GDS_BUILD_VERSION);
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

    /* Stage 1.5: GOT route audit - print GOT slot values for key symbols. */
    static const char *audit_syms[] = {"pthread_create","sysconf","sem_wait","sem_post","syscall","sched_getaffinity","pthread_cond_wait","pthread_cond_timedwait"};
    for (Module *m = m_il2cpp; m; m = (m == m_il2cpp) ? m_unity : (m == m_unity ? m_main : 0)) {
        if (!m) break;
        for (int set = 0; set < 2; set++) {
            Elf64_Rela *rels = set ? m->jmprel : m->rela;
            size_t cnt = set ? m->jmprel_count : m->rela_count;
            for (size_t i = 0; i < cnt; i++) {
                uint32_t type = rels[i].r_info & 0xffffffffULL;
                if (type != 1024 && type != 1026) continue;  /* R_AARCH64_GLOB_DAT=1024, JUMP_SLOT=1026 */
                uint64_t symidx = rels[i].r_info >> 32;
                const char *nm = m->strtab + m->symtab[symidx].st_name;
                for (size_t k = 0; k < sizeof audit_syms/sizeof*audit_syms; k++) {
                    if (strcmp(nm, audit_syms[k]) == 0) {
                        uint64_t *slot = (uint64_t *)(m->bias + rels[i].r_offset);
                        printf("[audit] %s GOT[%s] = %p (kv ptr range=0x10000000-0x13000000, libc=0x7f...)\n",
                               m->name, nm, (void *)*slot);
                    }
                }
            }
        }
    }

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
