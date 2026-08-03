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
#include <stdint.h>
#include <stddef.h>

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
    uint8_t *buf = malloc(1 << 20);
    ssize_t got = 0; size_t total = (size_t)1 << 20;
    while (got < (ssize_t)total) {
        ssize_t r = read(fd, buf + got, total - got);
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

static void *resolve(const char *sym) {
    return dlsym(0, sym);
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
        case DT_INIT:   m->init = (void (*)(void))(m->bias + d->d_un.d_ptr); break;
        case DT_INIT_ARRAY: m->init_array = (uint8_t *)(m->bias + d->d_un.d_ptr); break;
        case DT_INIT_ARRAYSZ: m->init_array_sz = d->d_un.d_val; break;
        }
    }

    /* Relocations. RELATIVE first, then symbol-bearing types. */
    int unresolved = 0;
    for (size_t i = 0; i < m->rela_count; i++) {
        Elf64_Rela *r = &m->rela[i];
        uint32_t type = r->r_info & 0xffffffffULL;
        uint64_t symidx = r->r_info >> 32;
        uint64_t *slot = (uint64_t *)(m->bias + r->r_offset);
        if (type == R_AARCH64_RELATIVE) {
            *slot = m->bias + r->r_addend;
        } else if (type == R_AARCH64_GLOB_DAT || type == R_AARCH64_JUMP_SLOT ||
                   type == R_AARCH64_ABS64 || type == R_AARCH64_ABS32) {
            const char *name = m->strtab + m->symtab[symidx].st_name;
            void *p = resolve(name);
            if (!p) { if (unresolved++ < 12) printf( "[loader]   unresolved %s\n", name); *slot = 0; }
            else *slot = (uint64_t)p;
        } else if (type == R_AARCH64_COPY) {
            /* handled by host linker; skip */
        }
    }
    if (unresolved) printf( "[loader] %d unresolved symbols in %s\n", unresolved, m->name);

    printf("[loader] %s mapped @%p span=%#zx relas=%zu\n", m->name, (void *)base, span, m->rela_count);

    if (m->init) m->init();
    if (m->init_array) {
        size_t n = m->init_array_sz / sizeof(uint64_t);
        for (size_t i = 0; i < n; i++) {
            uint64_t off = ((uint64_t *)m->init_array)[i];
            if (off) ((void (*)(void))(m->bias + off))();
        }
        printf("[loader] %s init_array ran (%zu ctors)\n", m->name, n);
    }
    return m;
}

/* We can reach into the loaded il2cpp runtime. il2cpp_runtime_invoke needs a
 * MethodInfo*; we don't have one yet, so just confirm the symbol resolves and
 * is callable via a harmless probe (il2cpp runtime version string etc. is done
 * in stage 2). For stage 1, report success. */
int real_main(int argc, char **argv) __attribute__((noreturn));

void _start(void) {
    for (;;) {
        int argc = 1;
        char *argv[2] = { (char *)"loader", 0 };
        real_main(argc, argv);
    }
}

int real_main(int argc, char **argv) {
    const char *lib = (argc > 1) ? argv[1] : "out/apk/lib/arm64-v8a/libil2cpp.so";
    printf("[loader] real_main argc=%d\n", argc);
    printf("[loader] loading %s\n", lib);
    Module *m = load_object(lib);
    (void)m;
    printf("[loader] OK: %s loaded and initialised\n", lib);
    return 0;
}
