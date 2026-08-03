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
    if (!m->symtab || !m->strtab) return 0;
    for (size_t i = 0; i < 200000; i++) {
        Elf64_Sym *sym = &m->symtab[i];
        const char *nm = m->strtab + sym->st_name;
        /* stop when we run off into garbage; dynamic symtab ends where st_name
         * is huge - guard with a cheap check */
        if (sym->st_name > 0xffffffffULL || (uintptr_t)nm < (uintptr_t)m->strtab) break;
        if (sym->st_shndx != SHN_UNDEF && sym->st_value != 0 &&
            strcmp(nm, wanted) == 0) {
            return (void *)(m->bias + sym->st_value);
        }
        if (sym->st_name == 0 && sym->st_shndx == 0 && sym->st_value == 0 &&
            sym->st_size == 0 && i > 8) break; /* hit the null entry */
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
    if (memcmp(eh->e_ident, "\177ELF", 4) != 0) { printf("[loader] %s: not ELF\n", path); for (;;) {} }
    if (eh->e_machine != 0xB7) { printf("[loader] %s: not aarch64\n", path); for (;;) {} }

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

int real_main(int argc, char **argv);

void _start(void) {
    /* Read the aarch64 Linux entry registers before anything can clobber x0/x1.
     * (register-asm locals get spilled to the stack at -O0, so we read x0/x1
     * directly via `mov` and pass the values as plain C locals.) */
    long argc;
    char **argv;
    __asm__ volatile("mov %0, x0" : "=r"(argc));
    __asm__ volatile("mov %0, x1" : "=r"(argv));
    real_main((int)argc, argv);
    sys_exit0();   /* stage-1 success path: return cleanly with exit code 0 */
}

int real_main(int argc, char **argv) {
    /* Load every .so given on the command line, in order.  Passing two lets the
     * engine (libunity.so) and the game (libil2cpp.so) both load+init before
     * Unity's entry is driven in stage 2:
     *   python3 tools/run_aarch64.py loader/loader2 libil2cpp.so libunity.so
     */
    if (argc < 2) {
        printf("[loader] usage: loader2 <libil2cpp.so> [libunity.so ...]\n");
        return 0;
    }
    int n = 0;
    Module *last = 0;
    for (int i = 1; i < argc; i++) {
        printf("[loader] loading %s\n", argv[i]);
        Module *m = load_object(argv[i]);
        if (m) { n++; last = m; }
    }
    printf("[loader] OK: %d module(s) loaded and initialised\n", n);

    /* Stage 2: drive the Unity boot.  libunity.so's only entry is JNI_OnLoad;
     * hand it our JNI shim's JavaVM + JNIEnv (no real Android underneath). */
    if (last) {
        void *(*jni_onload)(void *, void *) = module_export(last, "JNI_OnLoad");
        if (jni_onload) {
            printf("[loader] calling JNI_OnLoad (vm=%p)\n", kv_jni_java_vm());
            void *r = jni_onload(kv_jni_java_vm(), 0);
            printf("[loader] JNI_OnLoad returned %p (0x%lx)\n", r, (unsigned long)r);
        } else {
            printf("[loader] no JNI_OnLoad in %s\n", last->name);
        }
    }
    return 0;
}
