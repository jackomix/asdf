/* so_probe.c - synthetic freestanding aarch64 shared object used to exercise
 * loader.c's .so loading path without needing the 33 MB game .so asset.
 *
 * It deliberately carries the same shapes the real libil2cpp.so has:
 *   * a writable global with an address-taking reference (RELATIVE reloc),
 *   * an imported symbol resolved against the host symbol table (GLOB_DAT),
 *   * a .init_array constructor that proves DT_INIT_ARRAY runs after relocs.
 *
 * Build (freestanding, like the loader itself):
 *   python3 -m ziglang cc -target aarch64-linux-gnu -shared -fPIC \
 *       -ffreestanding -nostdlib -fno-sanitize=undefined \
 *       loader/so_probe.c -o /tmp/so_probe.so
 *
 * Then load it under the bench:
 *   python3 tools/run_aarch64.py /tmp/loader2 /tmp/so_probe.so
 */
#include <stdint.h>

static void wr(const char *s) {
    register long x8 __asm__("x8") = 64;   /* write */
    register long x0 __asm__("x0") = 1;
    register long x1 __asm__("x1") = (long)s;
    register long x2 __asm__("x2") = 0;
    const char *t = s;
    while (*t) t++;
    x2 = (long)(t - s);
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2) : "memory");
}

static void puthex(unsigned long v) {
    char buf[18];
    const char *hx = "0123456789abcdef";
    int i = 0;
    do { buf[i++] = hx[v & 0xf]; v >>= 4; } while (v);
    buf[i] = 0;
    /* print reversed */
    char out[20]; int j = 0; while (i) out[j++] = buf[--i]; out[j] = 0;
    wr(out);
}

/* --- the .so's "game" globals --- */
int kairo_marker = 0x1234abcd;
int kairo_touched = 0;

/* Imported from the host symbol table (host_syms.c resolves this to kv_strlen).
 * In a real .so this is what the loader turns into a GLOB_DAT/JUMP_SLOT reloc
 * resolved at load time. */
unsigned long strlen(const char *s);

/* A constructor.  The loader must apply relocations *before* running this, so
 * taking &kairo_marker here exercises the RELATIVE path. */
__attribute__((constructor))
static void _ctor(void) {
    unsigned long n = strlen("probe");   /* must resolve via host_dlsym */
    wr("[so_probe] ctor ran; strlen(\"probe\")=");
    puthex(n);
    wr(" marker=");
    puthex((unsigned long)kairo_marker);
    wr(" marker_addr=");
    puthex((unsigned long)&kairo_marker);
    wr("\n");
    kairo_touched = (int)n;              /* proves the global is writable */
}
