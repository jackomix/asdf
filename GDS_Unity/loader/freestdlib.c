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
#include "kv_libc.h"

#define SYS_mmap       222
#define SYS_mprotect   226
#define SYS_write      64
#define SYS_exit       93
#define SYS_openat     257
#define SYS_read       63
#define SYS_close      57
#define SYS_brk        214
#define SYS_fstatat    79
#define SYS_lseek      62
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

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
ssize_t write(int fd, const void *buf, unsigned long n) {
    return (ssize_t)raw_syscall(SYS_write, fd, (long)buf, (long)n, 0, 0, 0);
}
int close(int fd) { return (int)raw_syscall(SYS_close, fd, 0, 0, 0, 0, 0); }
int munmap(void *a, unsigned long l) { (void)a;(void)l; return 0; }
long lseek(int fd, long off, int whence) {
    return (long)raw_syscall(SYS_lseek, fd, off, whence, 0, 0, 0);
}

/* ---- bump allocator ---- */
static uint8_t *heap_ptr = 0;
static uint8_t *heap_end = 0;
#define HEAP_START ((uint8_t *)0x9e000000UL)
/* big enough for both 33 MB libil2cpp.so + 16 MB libunity.so file buffers and
 * the .so boot-time allocations. */
#define HEAP_SIZE  (256UL * 1024 * 1024)

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
void *realloc(void *old, unsigned long sz) {
    if (!old) return malloc(sz);
    /* bump allocator can't free old, so allocate fresh and copy (kept small:
     * realloc is rare in the .so boot path; leak the old block). */
    void *n = malloc(sz);
    if (n && old) { unsigned long csz = sz; memcpy(n, old, csz); }
    return n;
}
void *memalign(unsigned long align, unsigned long sz) { (void)align; return malloc(sz); }
int posix_memalign(void **memptr, unsigned long align, unsigned long sz) {
    *memptr = malloc(sz); (void)align; return *memptr ? 0 : 12; /* ENOMEM */
}

/* ---- string / mem helpers ---- */
void *memset(void *d, int c, unsigned long n) {
    unsigned char *p = d;
    /* word-wise fill (the loader copies multi-MB segments into mapped memory;
     * a byte loop is ~8x slower and dominates load time under the bench). */
    unsigned long v = (unsigned char)c;
    v |= v << 8; v |= v << 16; v |= v << 32;
    unsigned long i = 0;
    while (i + 8 <= n) { *(unsigned long *)(p + i) = v; i += 8; }
    while (i < n) p[i++] = (unsigned char)c;
    return d;
}
void *memcpy(void *d, const void *s, unsigned long n) {
    unsigned char *p = d; const unsigned char *q = s;
    /* word-wise copy, 8 bytes at a time (see memset note). */
    unsigned long i = 0;
    while (i + 8 <= n) { *(unsigned long *)(p + i) = *(const unsigned long *)(q + i); i += 8; }
    while (i < n) { p[i] = q[i]; i++; }
    return d;
}
int memcmp(const void *a, const void *b, unsigned long n) {
    const unsigned char *x = a, *y = b;
    for (unsigned long i = 0; i < n; i++)
        if (x[i] != y[i]) return (int)x[i] - (int)y[i];
    return 0;
}
void *memmove(void *d, const void *s, unsigned long n) {
    unsigned char *p = d; const unsigned char *q = s;
    if (p == q || n == 0) return d;
    if (p < q) return memcpy(d, s, n);
    /* overlap with dst after src: copy backwards */
    for (unsigned long i = n; i > 0; i--) p[i - 1] = q[i - 1];
    return d;
}
void *memchr(const void *s, int c, unsigned long n) {
    const unsigned char *p = s;
    for (unsigned long i = 0; i < n; i++) if (p[i] == (unsigned char)c) return (void *)(p + i);
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
char *strcpy(char *d, const char *s) { return memcpy(d, s, strlen(s) + 1); }
char *strncpy(char *d, const char *s, unsigned long n) {
    unsigned long i = 0;
    for (; i < n && s[i]; i++) d[i] = s[i];
    for (; i < n; i++) d[i] = 0;
    return d;
}
char *strchr(const char *s, int c) {
    do { if (*s == (char)c) return (char *)s; } while (*s++);
    return 0;
}
char *strrchr(const char *s, int c) {
    const char *last = 0;
    do { if (*s == (char)c) last = s; } while (*s++);
    return (char *)last;
}
char *strstr(const char *h, const char *n) {
    if (!*n) return (char *)h;
    for (; *h; h++) {
        const char *a = h, *b = n;
        while (*b && *a == *b) { a++; b++; }
        if (!*b) return (char *)h;
    }
    return 0;
}
char *strdup(const char *s) {
    unsigned long n = strlen(s) + 1; char *p = malloc(n); if (p) memcpy(p, s, n); return p;
}
char *strlcpy(char *d, const char *s, unsigned long n) {
    unsigned long sl = strlen(s);
    if (n) { unsigned long c = sl < n - 1 ? sl : n - 1; memcpy(d, s, c); d[c] = 0; }
    return d;
}
int atoi(const char *s) { return (int)strtol(s, 0, 10); }
long atol(const char *s) { return strtol(s, 0, 10); }
long strtol(const char *s, char **end, int base) {
    long v = 0; int neg = 0; const char *p = s;
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '-') { neg = 1; p++; } else if (*p == '+') p++;
    if (base == 0) { base = 10; if (p[0]=='0' && (p[1]=='x'||p[1]=='X')) { base=16; p+=2; } }
    while ((*p >= '0' && *p <= '9') ||
           (base == 16 && ((*p>='a'&&*p<='f')||(*p>='A'&&*p<='F')))) {
        int d; if (*p>='0'&&*p<='9') d=*p-'0';
        else if (*p>='a'&&*p<='f') d=*p-'a'+10; else d=*p-'A'+10;
        v = v * base + d; p++;
    }
    if (end) *end = (char *)p;
    return neg ? -v : v;
}
unsigned long strtoul(const char *s, char **end, int base) {
    long v = strtol(s, end, base); return (unsigned long)v;
}
unsigned long long strtoull(const char *s, char **end, int base) {
    return (unsigned long long)strtoul(s, end, base);
}
long long strtoll(const char *s, char **end, int base) {
    return (long long)strtol(s, end, base);
}
int fstat(int fd, void *st) {
    (void)fd; (void)st;
    if (st) memset(st, 0, 128);
    return 0;
}

/* ---- real stdio backed by the loader's fd syscalls ----
 * il2cpp_init reads global-metadata.dat and the managed assemblies through
 * fopen/fread/fseek.  Under the bench these resolve to the same openat/read/
 * lseek syscalls the loader uses, so the extracted APK assets are reachable.
 * FILE is a small struct holding the underlying fd + current offset + flags. */
typedef struct { int fd; int err; int eof; int mode; } kv_FILE;
#define KV_FOPEN_READ 1
static kv_FILE kv_files[64];
static int kv_nfiles = 1;  /* 0 is reserved (NULL) */

static kv_FILE *kv_file_new(int fd) {
    if (kv_nfiles >= 64) return 0;
    kv_FILE *f = &kv_files[kv_nfiles++];
    f->fd = fd; f->err = 0; f->eof = 0; f->mode = KV_FOPEN_READ;
    return f;
}

void *fopen(const char *path, const char *mode) {
    int flags = KV_FOPEN_READ;
    if (mode && mode[0] == 'w') flags = 0x201;      /* O_WRONLY|O_CREAT|O_TRUNC */
    int fd = open(path, flags);
    if (fd < 0) return 0;
    return kv_file_new(fd);
}
int fclose(void *fp) {
    kv_FILE *f = fp; if (!f) return 0;
    close(f->fd); return 0;
}
unsigned long fread(void *ptr, unsigned long sz, unsigned long nmemb, void *fp) {
    kv_FILE *f = fp; if (!f) return 0;
    long n = (long)(sz * nmemb);
    ssize_t r = read(f->fd, ptr, (unsigned long)n);
    if (r <= 0) { if (r == 0) f->eof = 1; else f->err = 1; return 0; }
    return (unsigned long)r / sz;
}
unsigned long fwrite(const void *ptr, unsigned long sz, unsigned long nmemb, void *fp) {
    kv_FILE *f = fp; if (!f) return 0;
    long n = (long)(sz * nmemb);
    ssize_t r = write(f->fd, ptr, (unsigned long)n);
    if (r <= 0) { f->err = 1; return 0; }
    return (unsigned long)r / sz;
}
int fflush(void *fp) { (void)fp; return 0; }
int fseek(void *fp, long off, int whence) {
    kv_FILE *f = fp; if (!f) return -1;
    f->eof = 0;
    return (int)lseek(f->fd, off, whence);
}
int fseeko(void *fp, long off, int whence) { return fseek(fp, off, whence); }
long ftell(void *fp) {
    kv_FILE *f = fp; if (!f) return -1;
    return lseek(f->fd, 0, SEEK_CUR);
}
long ftello(void *fp) { return ftell(fp); }
int feof(void *fp) { kv_FILE *f = fp; return f ? f->eof : 0; }
int ferror(void *fp) { kv_FILE *f = fp; return f ? f->err : 0; }
int clearerr(void *fp) { kv_FILE *f = fp; if (f) { f->err=0; f->eof=0; } return 0; }
int fileno(void *fp) { kv_FILE *f = fp; return f ? f->fd : -1; }
void *fdopen(int fd, const char *mode) { (void)mode; return kv_file_new(fd); }
int fputc(int c, void *fp) { kv_FILE *f=fp; if(!f) return -1; unsigned char b=(unsigned char)c; return write(f->fd,&b,1)==1?c:-1; }
int fputs(const char *s, void *fp) { kv_FILE *f=fp; if(!f) return -1; return write(f->fd,s,strlen(s))<0?-1:0; }
char *fgets(char *b, int n, void *fp) {
    kv_FILE *f=fp; if(!f||n<=0) return 0;
    int i=0;
    while (i<n-1) {
        unsigned char c;
        ssize_t r=read(f->fd,&c,1);
        if (r<=0) { if (i==0) return 0; break; }
        b[i++]=(char)c;
        if (c=='\n') break;
    }
    b[i]=0;
    if (i==0) return 0;
    return b;
}
int setbuf(void *fp, char *b) { (void)fp;(void)b; return 0; }
int setvbuf(void *fp, char *b, int m, unsigned long s) { (void)fp;(void)b;(void)m;(void)s; return 0; }
int fscanf(void *fp, const char *fmt, ...) { (void)fp;(void)fmt; return 0; }
int fprintf_real(int fd, const char *fmt, ...); /* see below */

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
void _Exit(int code) { exit(code); }
void _exit(int code) { exit(code); }
void abort(void) { raw_syscall(SYS_write, 1, (long)"abort()\n", 8, 0, 0, 0); raw_syscall(SYS_exit, 134, 0,0,0,0,0); for (;;) {} }

/* TLS errno: the .so reads *__errno() (bionic exports __errno as a function).
 * Give it a stable slot. */
static int kv_errno_slot;
int *__errno(void) { return &kv_errno_slot; }

/* environ (bionic global; glibc has it as a symbol). */
char **environ = 0;

/* __sF: glibc's file array.  libil2cpp references stderr/stdout via it; a
 * minimal 3-entry table lets it not crash on early stderr writes. */
static char kv_stdin_buf, kv_stdout_buf, kv_stderr_buf;
static void *kv_sF[3] = { &kv_stdin_buf, &kv_stdout_buf, &kv_stderr_buf };
void *__sF = kv_sF;

/* cxa runtime: only need to count/ignore for boot (no objects actually need
 * their destructors in stage 1). */
static unsigned kv_atexit_count;
int __cxa_atexit(void (*f)(void *), void *arg, void *dso) {
    (void)f; (void)arg; (void)dso; kv_atexit_count++; return 0;
}
void __cxa_finalize(void *dso) { (void)dso; }
void __stack_chk_fail(void) {
    raw_syscall(SYS_write, 1, (long)"stack smashing detected\n", 24, 0, 0, 0);
    abort();
}
void *__memmove_chk(void *d, const void *s, unsigned long n, unsigned long dlen) {
    (void)dlen; return memmove(d, s, n);
}
void *__memcpy_chk(void *d, const void *s, unsigned long n, unsigned long dlen) {
    (void)dlen; return memcpy(d, s, n);
}
void *__memset_chk(void *d, int c, unsigned long n, unsigned long dlen) {
    (void)dlen; return memset(d, c, n);
}
unsigned long __strlen_chk(const char *s, unsigned long slen) { (void)slen; return strlen(s); }
int __vsnprintf_chk(char *s, unsigned long n, int flag, unsigned long slen, const char *fmt, void *ap) {
    (void)flag; (void)slen; (void)ap; (void)s; (void)fmt; (void)n; return 0;
}
void __FD_SET_chk(int fd, void *set) { (void)fd; (void)set; }

/* ---- time / syscall-facing libc ---- */
long time(long *t) { return (long)raw_syscall(169, 0,0,0,0,0,0); } /* clock_gettime? use gettimeofday */
int gettimeofday(void *tv, void *tz) { (void)tz; return (int)raw_syscall(169, (long)tv, 0,0,0,0,0); }
long clock(void) { return -1; }
int clock_gettime(int c, void *tp) { return (int)raw_syscall(113, c, (long)tp, 0,0,0,0); }
int clock_getres(int c, void *tp) { return (int)raw_syscall(114, c, (long)tp, 0,0,0,0); }
int nanosleep(void *req, void *rem) { return (int)raw_syscall(101, (long)req, (long)rem, 0,0,0,0); }
int usleep(unsigned long u) {
    long ts[2] = { (long)(u / 1000000), (long)((u % 1000000) * 1000) };
    return nanosleep(ts, 0);
}
int getpid(void) { return (int)raw_syscall(172, 0,0,0,0,0,0); }
int getuid(void) { return (int)raw_syscall(174, 0,0,0,0,0,0); }
int geteuid(void) { return (int)raw_syscall(175, 0,0,0,0,0,0); }
int getegid(void) { return (int)raw_syscall(177, 0,0,0,0,0,0); }
int sched_yield(void) { return (int)raw_syscall(124, 0,0,0,0,0,0); }
int getpagesize(void) { return 4096; }
long sysconf(int name) { return name == 30 ? 4096 : -1; } /* _SC_PAGESIZE */
int isatty(int fd) { (void)fd; return 0; }
int getenv_probe;

char *getenv(const char *name) {
    (void)name; return 0;  /* no env in the bench */
}
int setenv(const char *n, const char *v, int o) { (void)n;(void)v;(void)o; return 0; }
int unsetenv(const char *n) { (void)n; return 0; }
int gethostname(char *n, unsigned long len) { (void)len; if (n) n[0]=0; return 0; }
int getcwd(char *b, unsigned long n) { (void)n; if (b) b[0]='/'; return 0; }

/* pthread: minimal no-op stubs so the .so's threaded boot doesn't crash. */
typedef unsigned long kv_pthread_t;
int pthread_mutex_init(void *m, void *a) { (void)m;(void)a; return 0; }
int pthread_mutex_destroy(void *m) { (void)m; return 0; }
int pthread_mutex_lock(void *m) { (void)m; return 0; }
int pthread_mutex_unlock(void *m) { (void)m; return 0; }
int pthread_mutex_trylock(void *m) { (void)m; return 0; }
int pthread_mutexattr_init(void *a) { (void)a; return 0; }
int pthread_mutexattr_destroy(void *a) { (void)a; return 0; }
int pthread_mutexattr_settype(void *a, int t) { (void)a;(void)t; return 0; }
int pthread_cond_init2(void *c) { (void)c; return 0; }
int pthread_cond_destroy(void *c) { (void)c; return 0; }
int pthread_cond_broadcast(void *c) { (void)c; return 0; }
int pthread_cond_signal(void *c) { (void)c; return 0; }
int pthread_cond_wait(void *c, void *m) { (void)c;(void)m; return 0; }
int pthread_cond_timedwait(void *c, void *m, void *t) { (void)c;(void)m;(void)t; return 0; }
int pthread_create(kv_pthread_t *t, void *a, void *(*fn)(void *), void *arg) {
    (void)t;(void)a;(void)fn;(void)arg; return 0;  /* pretend thread already ran */
}
int pthread_join(kv_pthread_t t, void **r) { (void)t;(void)r; return 0; }
int pthread_detach(kv_pthread_t t) { (void)t; return 0; }
kv_pthread_t pthread_self(void) { return 0; }
int pthread_equal(kv_pthread_t a, kv_pthread_t b) { return a == b; }
int pthread_once(void *c, void (*fn)(void)) { (void)c; if (fn) fn(); return 0; }
int pthread_key_create(unsigned *k, void (*d)(void *)) { (void)d; *k = 1; return 0; }
int pthread_key_delete(unsigned k) { (void)k; return 0; }
void *pthread_getspecific(unsigned k) { (void)k; return 0; }
int pthread_setspecific(unsigned k, const void *v) { (void)k;(void)v; return 0; }
int pthread_sigmask(int h, void *s, void *o) { (void)h;(void)s;(void)o; return 0; }
int pthread_kill(kv_pthread_t t, int s) { (void)t;(void)s; return 0; }
int pthread_atfork(void (*a)(void), void (*b)(void), void (*c)(void)) { (void)a;(void)b;(void)c; return 0; }
int pthread_attr_init(void *a) { (void)a; return 0; }
int pthread_attr_destroy(void *a) { (void)a; return 0; }
/* The main thread's stack bounds live in the TLS block (slots 6/7), which the
 * bench sets from the real stack region.  Return them so the IL2CPP GC's stack
 * scan stays within the mapped stack instead of walking off the end. */
static void kv_get_main_stack(unsigned long *lo, unsigned long *hi) {
    unsigned long tls = 0;
    __asm__ volatile("mrs %0, tpidr_el0" : "=r"(tls));
    if (tls) {
        *lo = *(volatile unsigned long *)(tls + 48);
        *hi = *(volatile unsigned long *)(tls + 56);
        return;
    }
    *lo = 0; *hi = 0;
}
int pthread_attr_getstack(void *a, void **s, void **sz) {
    (void)a;
    unsigned long lo, hi; kv_get_main_stack(&lo, &hi);
    if (s) *s = (void *)(uintptr_t)lo;
    if (sz) *sz = (void *)(uintptr_t)(hi - lo);
    return 0;
}
int pthread_getattr_np(kv_pthread_t t, void *a) {
    (void)t; (void)a;
    return 0;
}
int pthread_setname_np(kv_pthread_t t, const char *n) { (void)t;(void)n; return 0; }
int pthread_rwlock_rdlock(void *l) { (void)l; return 0; }
int pthread_rwlock_wrlock(void *l) { (void)l; return 0; }
int pthread_rwlock_unlock(void *l) { (void)l; return 0; }
int sem_init(void *s, int p, unsigned v) { (void)s;(void)p;(void)v; return 0; }
int sem_destroy(void *s) { (void)s; return 0; }
int sem_post(void *s) { (void)s; return 0; }
int sem_wait(void *s) { (void)s; return 0; }
int sem_timedwait(void *s, void *t) { (void)s;(void)t; return 0; }
int sem_getvalue(void *s, int *v) { (void)s; if (v) *v = 0; return 0; }

/* ---- dlsym / dlopen ---- */
/* loader_lookup_export (defined in loader.c) resolves against loaded .so files
 * so libmain's JNI_OnLoad can find libunity/libil2cpp symbols via dlsym. */
void *loader_lookup_export(const char *wanted);
void *dlsym(void *handle, const char *name) {
    (void)handle;
    /* 1) our own freestanding libc, 2) any loaded .so export (libunity etc.) */
    void *p = host_dlsym(name);
    if (p) return p;
    return loader_lookup_export(name);
}
void *dlopen(const char *name, int flags) {
    (void)name; (void)flags; return (void *)1; /* always "succeeds" */
}
