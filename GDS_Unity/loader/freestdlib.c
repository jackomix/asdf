/* freestdlib.c - the handful of libc services the native loader needs.
 *
 * The R36S loader must not depend on Android's bionic or the distro libc at
 * load time (it is the thing that provides the platform), so it carries its
 * own minimal libc.  Only what loader.c uses: raw-syscall mmap/mprotect/open/
 * read/close, a bump allocator, printf to a write syscall, memcpy/memset/
 * memcmp/strlen/strcmp/strncpy, fstat (size only) and dlsym against a small
 * host symbol table supplied in host_syms.c.
 */
#include <stdarg.h>
#include <stdint.h>
#include "kv_elf.h"

#define SYS_mmap       222
#define SYS_mprotect   226
#define SYS_write      64
#define SYS_exit       93
#define SYS_openat     257
#define SYS_read       63
#define SYS_close      57
#define SYS_brk        214
#define SYS_fstatat    79

static long raw_syscall(int n, long a, long b, long c, long d, long e, long f) {
    register long x8 __asm__("x8") = n;
    register long x0 __asm__("x0") = a;
    register long x1 __asm__("x1") = b;
    register long x2 __asm__("x2") = c;
    register long x3 __asm__("x3") = d;
    register long x4 __asm__("x4") = e;
    register long x5 __asm__("x5") = f;
    __asm__ volatile("svc #0"
                     : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5)
                     : "memory");
    return x0;
}

void *mmap(void *addr, unsigned long len, int prot, int flags, int fd, long off) {
    return (void *)raw_syscall(SYS_mmap, (long)addr, (long)len, prot, flags, fd, off);
}
int mprotect(void *addr, unsigned long len, int prot) {
    return (int)raw_syscall(SYS_mprotect, (long)addr, (long)len, prot, 0, 0, 0);
}
int open(const char *path, int flags) {
    return (int)raw_syscall(SYS_openat, -100, (long)path, flags, 0, 0, 0);
}
ssize_t read(int fd, void *buf, unsigned long n) {
    return (ssize_t)raw_syscall(SYS_read, fd, (long)buf, (long)n, 0, 0, 0);
}
int close(int fd) { return (int)raw_syscall(SYS_close, fd, 0, 0, 0, 0, 0); }

/* ---- bump allocator ---- */
static uint8_t *heap_ptr = 0;
static uint8_t *heap_end = 0;
#define HEAP_START ((uint8_t *)0x9e000000UL)
#define HEAP_SIZE  (64UL * 1024 * 1024)

static void ensure_heap(void) {
    if (!heap_ptr) {
        heap_ptr = (uint8_t *)mmap(HEAP_START, HEAP_SIZE,
                                    PROT_READ | PROT_WRITE,
                                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        heap_end = heap_ptr + HEAP_SIZE;
    }
}
void *malloc(unsigned long sz) {
    ensure_heap();
    sz = (sz + 15) & ~(15UL);
    if (heap_ptr + sz > heap_end) return 0;
    void *p = heap_ptr; heap_ptr += sz; return p;
}
void free(void *p) { (void)p; }
void *calloc(unsigned long n, unsigned long sz) {
    unsigned long t = n * sz; void *p = malloc(t); if (p) memset(p, 0, t); return p;
}

/* ---- string / mem helpers ---- */
void *memset(void *d, int c, unsigned long n) {
    unsigned char *p = d; for (unsigned long i = 0; i < n; i++) p[i] = (unsigned char)c;
    return d;
}
void *memcpy(void *d, const void *s, unsigned long n) {
    unsigned char *p = d; const unsigned char *q = s;
    for (unsigned long i = 0; i < n; i++) p[i] = q[i];
    return d;
}
int memcmp(const void *a, const void *b, unsigned long n) {
    const unsigned char *x = a, *y = b;
    for (unsigned long i = 0; i < n; i++)
        if (x[i] != y[i]) return (int)x[i] - (int)y[i];
    return 0;
}
unsigned long strlen(const char *s) { unsigned long n = 0; while (s[n]) n++; return n; }
int strcmp(const char *a, const char *b) { return memcmp(a, b, strlen(a) + 1); }
int strncmp(const char *a, const char *b, unsigned long n) {
    for (unsigned long i = 0; i < n; i++) {
        if (a[i] != b[i]) return (int)(unsigned char)a[i] - (int)(unsigned char)b[i];
        if (a[i] == 0) return 0;
    }
    return 0;
}
char *strncpy(char *d, const char *s, unsigned long n) {
    unsigned long i = 0;
    for (; i < n && s[i]; i++) d[i] = s[i];
    for (; i < n; i++) d[i] = 0;
    return d;
}
int fstat(int fd, void *st) {
    (void)fd; (void)st;
    if (st) memset(st, 0, 128);
    return 0;
}

/* ---- printf (no float) -> write syscall ---- */
static void putdec(long v, int neg) {
    char buf[24]; int i = 0;
    unsigned long u = neg ? (unsigned long)(-v) : (unsigned long)v;
    do { buf[i++] = '0' + (u % 10); u /= 10; } while (u);
    if (neg) buf[i++] = '-';
    while (i) { char c = buf[--i]; raw_syscall(SYS_write, 1, (long)&c, 1, 0, 0, 0); }
}
static void puthex(unsigned long v, int alt) {
    char buf[18]; int i = 0; const char *hx = "0123456789abcdef";
    do { buf[i++] = hx[v & 0xf]; v >>= 4; } while (v);
    if (alt) { raw_syscall(SYS_write, 1, (long)"0x", 2, 0, 0, 0); }
    while (i) { char c = buf[--i]; raw_syscall(SYS_write, 1, (long)&c, 1, 0, 0, 0); }
}
int printf(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    for (const char *s = fmt; *s; s++) {
        if (*s != '%') { raw_syscall(SYS_write, 1, (long)s, 1, 0, 0, 0); continue; }
        s++;
        /* flags + length modifiers (%#x, %zu, %ld, %lx ...): skip them */
        int alt = 0;
        while (*s == '#' || *s == '+' || *s == '-' || *s == '0' || *s == ' ' ||
               *s == 'z' || *s == 'l' || *s == 'h' || *s == 'L') {
            if (*s == '#') alt = 1;
            s++;
        }
        if (*s == 's') { char *t = va_arg(ap, char *); if (!t) t = "(null)"; while (*t) raw_syscall(SYS_write,1,(long)t++,1,0,0,0); }
        else if (*s == 'd' || *s == 'i') { long v = va_arg(ap, long); putdec(v, v < 0); }
        else if (*s == 'p') { void *p = va_arg(ap, void *); puthex((unsigned long)p, 1); }
        else if (*s == 'x' || *s == 'X' || *s == 'u') { unsigned long v = va_arg(ap, unsigned long); puthex(v, alt); }
        else if (*s == 'c') { int c = va_arg(ap, int); raw_syscall(SYS_write,1,(long)&c,1,0,0,0); }
        else if (*s == '%') { raw_syscall(SYS_write,1,(long)"%",1,0,0,0); }
        else { raw_syscall(SYS_write,1,(long)s,1,0,0,0); }
    }
    va_end(ap);
    return 0;
}
int fprintf(int fd, const char *fmt, ...) { (void)fd; va_list ap; va_start(ap,fmt); (void)ap; return printf(fmt, ap); }
void perror(const char *s) { if (s) printf("%s: error\n", s); }
void exit(int code) { raw_syscall(SYS_exit, code, 0, 0, 0, 0, 0); for (;;) {} }

/* ---- dlsym against the host symbol table (host_syms.c) ---- */
void *dlsym(void *handle, const char *name) { (void)handle; return host_dlsym(name); }
void *dlopen(const char *name, int flags) { (void)name; (void)flags; return (void *)1; }
