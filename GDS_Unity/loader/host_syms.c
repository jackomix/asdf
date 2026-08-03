/* host_syms.c - symbols the loaded .so resolves against.
 *
 * libil2cpp.so is built for bionic and imports a large slice of libc/libm/
 * pthread/locale from the system.  The native loader resolves them here.  On
 * the device these come from glibc; under the Unicorn bench we provide
 * freestanding versions so the .so's init_array can run headless.
 *
 * The implementations live in freestdlib.c (allocator, mem/str, printf, time,
 * pthread stubs) and in this file (math/locale/etc. stubs).  host_dlsym()
 * maps the imported name to the function pointer by an explicit table.
 */
#include <stddef.h>
#include <stdint.h>
#include "kv_elf.h"
#include "kv_libc.h"

/* ---- math stubs (return values good enough to keep init_array alive) ---- */
static double kv_nan(void) { union { double d; uint64_t u; } x; x.u = 0x7ff8000000000000ULL; return x.d; }
double cos(double x) { (void)x; return 1.0; }
float  cosf(float x) { (void)x; return 1.0f; }
double sin(double x) { (void)x; return 0.0; }
float  sinf(float x) { (void)x; return 0.0f; }
double tan(double x) { (void)x; return 0.0; }
float  tanf(float x) { (void)x; return 0.0f; }
double acos(double x) { (void)x; return 0.0; }
float  acosf(float x) { (void)x; return 0.0f; }
double asin(double x) { (void)x; return 0.0; }
double atan(double x) { (void)x; return 0.0; }
float  atanf(float x) { (void)x; return 0.0f; }
double atan2(double y, double x) { (void)y;(void)x; return 0.0; }
float  atan2f(float y, float x) { (void)y;(void)x; return 0.0f; }
double log(double x) { (void)x; return 0.0; }
double log10(double x) { (void)x; return 0.0; }
float  log10f(float x) { (void)x; return 0.0f; }
double log2(double x) { (void)x; return 0.0; }
double logb(double x) { (void)x; return 0.0; }
double exp2f(float x) { (void)x; return 1.0f; }
double pow(double a, double b) { (void)a;(void)b; return 1.0; }
float  powf(float a, float b) { (void)a;(void)b; return 1.0f; }
double sqrt(double x) { (void)x; return 1.0; }
double fmod(double a, double b) { (void)a;(void)b; return 0.0; }
float  fmodf(float a, float b) { (void)a;(void)b; return 0.0f; }
double hypot(double a, double b) { (void)a;(void)b; return 0.0; }
double modf(double x, double *i) { (void)x; if (i) *i = 0; return 0.0; }
double scalbn(double x, int n) { (void)x;(void)n; return 0.0; }
double difftime(double a, double b) { return a - b; }
void sincosf(float x, float *s, float *c) { (void)x; if (s) *s = 0; if (c) *c = 1; }
void *android_set_abort_message(void *m) { (void)m; return 0; }

/* ---- locale / wchar stubs ---- */
void *newlocale(int m, const char *l, void *b) { (void)m;(void)l;(void)b; return 0; }
void *freelocale(void *l) { (void)l; return 0; }
void *uselocale(void *l) { (void)l; return 0; }
void *localeconv(void) { return 0; }
void *setlocale(int c, const char *l) { (void)c;(void)l; return 0; }
int isdigit_l(int c, void *l) { (void)l; return c >= '0' && c <= '9'; }
int isxdigit_l(int c, void *l) { (void)l; return (c>='0'&&c<='9')||(c>='a'&&c<='f')||(c>='A'&&c<='F'); }
int islower_l(int c, void *l) { (void)l; return c >= 'a' && c <= 'z'; }
int isupper_l(int c, void *l) { (void)l; return c >= 'A' && c <= 'Z'; }
int iswalpha_l(int c, void *l) { (void)l; (void)c; return 0; }
int iswblank_l(int c, void *l) { (void)l;(void)c; return 0; }
int iswcntrl_l(int c, void *l) { (void)l;(void)c; return 0; }
int iswdigit_l(int c, void *l) { (void)l;(void)c; return 0; }
int iswlower_l(int c, void *l) { (void)l;(void)c; return 0; }
int iswprint_l(int c, void *l) { (void)l;(void)c; return 0; }
int iswpunct_l(int c, void *l) { (void)l;(void)c; return 0; }
int iswspace_l(int c, void *l) { (void)l;(void)c; return 0; }
int iswupper_l(int c, void *l) { (void)l;(void)c; return 0; }
int iswxdigit_l(int c, void *l) { (void)l;(void)c; return 0; }
int towlower(int c) { (void)c; return c; }
int towupper(int c) { (void)c; return c; }
int towlower_l(int c, void *l) { (void)l; return c; }
int towupper_l(int c, void *l) { (void)l; return c; }
int tolower_l(int c, void *l) { (void)l; return c >= 'A' && c <= 'Z' ? c + 32 : c; }
int toupper_l(int c, void *l) { (void)l; return c >= 'a' && c <= 'z' ? c - 32 : c; }
int btowc(int c) { return c; }
int wctob(int c) { return c; }
unsigned long wcslen(const void *s) { (void)s; return 0; }
void *wmemchr(const void *s, int c, unsigned long n) { (void)s;(void)c;(void)n; return 0; }
int wmemcmp(const void *a, const void *b, unsigned long n) { (void)a;(void)b;(void)n; return 0; }
void *wmemcpy(void *d, const void *s, unsigned long n) { (void)s;(void)n; return d; }
void *wmemmove(void *d, const void *s, unsigned long n) { (void)s;(void)n; return d; }
void *wmemset(void *d, int c, unsigned long n) { (void)c;(void)n; return d; }
unsigned long mbrlen(const char *s, unsigned long n, void *st) { (void)s;(void)n;(void)st; return 1; }
unsigned long mbrtowc(void *wc, const char *s, unsigned long n, void *st) { (void)wc;(void)s;(void)n;(void)st; return 1; }
unsigned long mbtowc(void *wc, const char *s, unsigned long n) { (void)wc;(void)s;(void)n; return 1; }
unsigned long wcrtomb(char *s, int wc, void *st) { (void)s;(void)wc;(void)st; return 1; }
unsigned long mbsrtowcs(void *d, const void *ss, unsigned long n, void *st) { (void)d;(void)ss;(void)n;(void)st; return 1; }
unsigned long mbsnrtowcs(void *d, const void *ss, unsigned long n, void *st) { (void)d;(void)ss;(void)n;(void)st; return 1; }
unsigned long wcsnrtombs(char *d, const void *ss, unsigned long n, void *st) { (void)d;(void)ss;(void)n;(void)st; return 1; }
int __ctype_get_mb_cur_max(void) { return 1; }

/* ---- misc libc ---- */
int __system_property_get(const char *name, char *value) {
    (void)name; if (value) value[0] = 0; return 0;
}
int div(int a, int b) { return a / b; }
void *bsearch(const void *k, const void *b, unsigned long n, unsigned long sz,
              int (*cmp)(const void *, const void *)) {
    (void)k;(void)b;(void)n;(void)sz;(void)cmp; return 0;
}
void qsort(void *b, unsigned long n, unsigned long sz, int (*cmp)(const void *, const void *)) {
    (void)b;(void)n;(void)sz;(void)cmp;
}
long lrand48(void) { return 0; }
void srand48(long x) { (void)x; }
void *gmtime(const void *t) { (void)t; return 0; }
void *localtime(const void *t) { (void)t; return 0; }
unsigned long mktime(void *t) { (void)t; return 0; }
unsigned long strftime(char *s, unsigned long n, const char *f, const void *tm) {
    (void)s;(void)n;(void)f;(void)tm; return 0;
}
void *strftime_l(void *s, unsigned long n, void *f, void *tm, void *l) { (void)s;(void)n;(void)f;(void)tm;(void)l; return 0; }
int strcoll_l(const char *a, const char *b, void *l) { (void)l; return strcmp(a, b); }
unsigned long strxfrm_l(char *d, const char *s, unsigned long n, void *l) { (void)d;(void)s;(void)n;(void)l; return 0; }
int wcscoll_l(const void *a, const void *b, void *l) { (void)a;(void)b;(void)l; return 0; }
unsigned long wcsxfrm_l(void *d, const void *s, unsigned long n, void *l) { (void)d;(void)s;(void)n;(void)l; return 0; }
void *fopen(const char *p, const char *m) { (void)p;(void)m; return 0; }
int fclose(void *f) { (void)f; return 0; }
int fflush(void *f) { (void)f; return 0; }
unsigned long fread(void *p, unsigned long s, unsigned long n, void *f) { (void)p;(void)s;(void)n;(void)f; return 0; }
unsigned long fwrite(const void *p, unsigned long s, unsigned long n, void *f) { (void)p;(void)s;(void)n;(void)f; return 0; }
int fputc(int c, void *f) { (void)f; return c; }
int fseek(void *f, long o, int w) { (void)f;(void)o;(void)w; return 0; }
int fseeko(void *f, long o, int w) { (void)f;(void)o;(void)w; return 0; }
long ftello(void *f) { (void)f; return 0; }
int access(const char *p, int m) { (void)p;(void)m; return -1; }
int unlink(const char *p) { (void)p; return -1; }
int rename(const char *a, const char *b) { (void)a;(void)b; return -1; }
int chmod(const char *p, int m) { (void)p;(void)m; return 0; }
int fchmod(int fd, int m) { (void)fd;(void)m; return 0; }
int mkdir(const char *p, int m) { (void)p;(void)m; return 0; }
int rmdir(const char *p) { (void)p; return 0; }
int link(const char *a, const char *b) { (void)a;(void)b; return -1; }
int symlink(const char *a, const char *b) { (void)a;(void)b; return -1; }
int readlink(const char *p, char *b, unsigned long n) { (void)p;(void)b;(void)n; return -1; }
int lseek(int fd, long o, int w) { (void)fd;(void)o;(void)w; return 0; }
int dup(int fd) { (void)fd; return -1; }
int ftruncate(int fd, long l) { (void)fd;(void)l; return 0; }
int futimens(int fd, void *t) { (void)fd;(void)t; return 0; }
int utimes(const char *p, void *t) { (void)p;(void)t; return 0; }
int ioctl(int fd, unsigned long r, ...) { (void)fd;(void)r; return 0; }
int pipe(int *p) { (void)p; return -1; }
int select(int n, void *r, void *w, void *e, void *t) { (void)n;(void)r;(void)w;(void)e;(void)t; return 0; }
long sendfile(int o, int i, long *off, unsigned long n) { (void)o;(void)i;(void)off;(void)n; return 0; }
void *opendir(const char *p) { (void)p; return 0; }
void *readdir(void *d) { (void)d; return 0; }
int closedir(void *d) { (void)d; return 0; }
int stat(const char *p, void *st) { (void)p; if (st) memset(st, 0, 144); return -1; }
int lstat(const char *p, void *st) { (void)p; if (st) memset(st, 0, 144); return -1; }
int sigaction(int s, const void *a, void *o) { (void)s;(void)a;(void)o; return 0; }
int signal(int s, void *h) { (void)s;(void)h; return 0; }
void *sigemptyset(void *s) { (void)s; return s; }
void *sigfillset(void *s) { (void)s; return s; }
void *sigaddset(void *s, int n) { (void)s;(void)n; return s; }
void *sigdelset(void *s, int n) { (void)s;(void)n; return s; }
int sigsuspend(void *m) { (void)m; return -1; }
int madvise(void *a, unsigned long l, int adv) { (void)a;(void)l;(void)adv; return 0; }
int dladdr(const void *a, void *i) { (void)a;(void)i; return 0; }
int dlclose(void *h) { (void)h; return 0; }
char *dlerror(void) { return 0; }
int dl_iterate_phdr(void *c, void *d) { (void)c;(void)d; return 0; }
long syscall(long n, ...) { (void)n; return -1; }
int setjmp(void *e) { (void)e; return 0; }
void longjmp(void *e, int v) { (void)e;(void)v; for (;;) {} }
int strerror_r(int e, char *b, unsigned long n) { (void)e;(void)n; if (b) b[0]=0; return 0; }
int snprintf(char *s, unsigned long n, const char *f, ...) { (void)s;(void)n;(void)f; return 0; }
int sprintf(char *s, const char *f, ...) { (void)s;(void)f; return 0; }
int vsnprintf(char *s, unsigned long n, const char *f, void *a) { (void)s;(void)n;(void)f;(void)a; return 0; }
int swprintf(void *s, unsigned long n, const void *f, ...) { (void)s;(void)n;(void)f; return 0; }
int vfprintf(void *f, const char *fmt, void *a) { (void)f;(void)fmt;(void)a; return 0; }
int vasprintf(char **o, const char *f, void *a) { (void)o;(void)f;(void)a; return -1; }
int sscanf(const char *s, const char *f, ...) { (void)s;(void)f; return 0; }
int vsscanf(const char *s, const char *f, void *a) { (void)s;(void)f;(void)a; return 0; }
int openlog(const char *i, int o, int f) { (void)i;(void)o;(void)f; return 0; }
int syslog(int p, const char *f, ...) { (void)p;(void)f; return 0; }
int closelog(void) { return 0; }
int __android_log_print(int prio, const char *tag, const char *fmt, ...) {
    (void)prio;(void)tag;(void)fmt; return 0;
}
int uname(void *u) {
    /* sys_utsname: 6 fixed char fields + version; zero it so .so boot doesn't
     * read garbage. */
    memset(u, 0, 390);
    return 0;
}
double strtod(const char *s, char **e) { (void)e; return 0.0; }
float strtof(const char *s, char **e) { (void)e; return 0.0f; }
long double strtold(const char *s, char **e) { (void)e; return 0.0; }
long double strtold_l(const char *s, char **e, void *l) { (void)e;(void)l; return 0.0; }
long long strtoll_l(const char *s, char **e, int b, void *l) { (void)l; return strtoll(s, e, b); }
unsigned long long strtoull_l(const char *s, char **e, int b, void *l) { (void)l; return strtoull(s, e, b); }
double wcstod(const void *s, void **e) { (void)s;(void)e; return 0.0; }
float wcstof(const void *s, void **e) { (void)s;(void)e; return 0.0f; }
long double wcstold(const void *s, void **e) { (void)s;(void)e; return 0.0; }
long wcstol(const void *s, void **e, int b) { (void)s;(void)e;(void)b; return 0; }
long long wcstoll(const void *s, void **e, int b) { (void)s;(void)e;(void)b; return 0; }
unsigned long wcstoul(const void *s, void **e, int b) { (void)s;(void)e;(void)b; return 0; }
unsigned long long wcstoull(const void *s, void **e, int b) { (void)s;(void)e;(void)b; return 0; }

void *host_dlsym(const char *name) {
    /* exact-name table; keep in sync with what libil2cpp.so imports.  Each
     * pointer is the real freestanding implementation (freestdlib.c or this
     * file). */
    static const struct { const char *n; void *p; } tab[] = {
        {"memcpy", (void *)memcpy}, {"memset", (void *)memset},
        {"memcmp", (void *)memcmp}, {"memmove", (void *)memmove},
        {"memchr", (void *)memchr}, {"strlen", (void *)strlen},
        {"strcmp", (void *)strcmp}, {"strncmp", (void *)strncmp},
        {"strcpy", (void *)strcpy}, {"strncpy", (void *)strncpy},
        {"strchr", (void *)strchr}, {"strrchr", (void *)strrchr},
        {"strstr", (void *)strstr}, {"strdup", (void *)strdup},
        {"strlcpy", (void *)strlcpy},
        {"__memcpy_chk", (void *)__memcpy_chk},
        {"__memmove_chk", (void *)__memmove_chk},
        {"__memset_chk", (void *)__memset_chk},
        {"__strlen_chk", (void *)__strlen_chk},
        {"__vsnprintf_chk", (void *)__vsnprintf_chk},
        {"__FD_SET_chk", (void *)__FD_SET_chk},
        {"__system_property_get", (void *)__system_property_get},
        {"__errno", (void *)__errno},
        {"environ", (void *)&environ},
        {"__sF", (void *)&__sF},
        {"__cxa_atexit", (void *)__cxa_atexit},
        {"__cxa_finalize", (void *)__cxa_finalize},
        {"__stack_chk_fail", (void *)__stack_chk_fail},
        {"abort", (void *)abort},
        {"exit", (void *)exit}, {"_exit", (void *)_exit}, {"_Exit", (void *)_Exit},
        {"malloc", (void *)malloc}, {"free", (void *)free},
        {"calloc", (void *)calloc}, {"realloc", (void *)realloc},
        {"memalign", (void *)memalign}, {"posix_memalign", (void *)posix_memalign},
        {"atoi", (void *)atoi}, {"atol", (void *)atol},
        {"strtol", (void *)strtol}, {"strtoul", (void *)strtoul},
        {"strtoll", (void *)strtoll}, {"strtoull", (void *)strtoull},
        {"fprintf", (void *)fprintf}, {"perror", (void *)perror},
        {"printf", (void *)printf},
        {"time", (void *)time}, {"gettimeofday", (void *)gettimeofday},
        {"clock", (void *)clock}, {"clock_gettime", (void *)clock_gettime},
        {"clock_getres", (void *)clock_getres}, {"nanosleep", (void *)nanosleep},
        {"usleep", (void *)usleep}, {"getpid", (void *)getpid},
        {"getuid", (void *)getuid}, {"geteuid", (void *)geteuid},
        {"getegid", (void *)getegid}, {"sched_yield", (void *)sched_yield},
        {"getpagesize", (void *)getpagesize}, {"sysconf", (void *)sysconf},
        {"isatty", (void *)isatty}, {"getenv", (void *)getenv},
        {"setenv", (void *)setenv}, {"unsetenv", (void *)unsetenv},
        {"gethostname", (void *)gethostname}, {"getcwd", (void *)getcwd},
        {"pthread_mutex_init", (void *)pthread_mutex_init},
        {"pthread_mutex_destroy", (void *)pthread_mutex_destroy},
        {"pthread_mutex_lock", (void *)pthread_mutex_lock},
        {"pthread_mutex_unlock", (void *)pthread_mutex_unlock},
        {"pthread_mutex_trylock", (void *)pthread_mutex_trylock},
        {"pthread_mutexattr_init", (void *)pthread_mutexattr_init},
        {"pthread_mutexattr_destroy", (void *)pthread_mutexattr_destroy},
        {"pthread_mutexattr_settype", (void *)pthread_mutexattr_settype},
        {"pthread_cond_destroy", (void *)pthread_cond_destroy},
        {"pthread_cond_broadcast", (void *)pthread_cond_broadcast},
        {"pthread_cond_signal", (void *)pthread_cond_signal},
        {"pthread_cond_wait", (void *)pthread_cond_wait},
        {"pthread_cond_timedwait", (void *)pthread_cond_timedwait},
        {"pthread_create", (void *)pthread_create},
        {"pthread_join", (void *)pthread_join},
        {"pthread_detach", (void *)pthread_detach},
        {"pthread_self", (void *)pthread_self},
        {"pthread_equal", (void *)pthread_equal},
        {"pthread_once", (void *)pthread_once},
        {"pthread_key_create", (void *)pthread_key_create},
        {"pthread_key_delete", (void *)pthread_key_delete},
        {"pthread_getspecific", (void *)pthread_getspecific},
        {"pthread_setspecific", (void *)pthread_setspecific},
        {"pthread_sigmask", (void *)pthread_sigmask},
        {"pthread_kill", (void *)pthread_kill},
        {"pthread_atfork", (void *)pthread_atfork},
        {"pthread_attr_init", (void *)pthread_attr_init},
        {"pthread_attr_destroy", (void *)pthread_attr_destroy},
        {"pthread_attr_getstack", (void *)pthread_attr_getstack},
        {"pthread_getattr_np", (void *)pthread_getattr_np},
        {"pthread_setname_np", (void *)pthread_setname_np},
        {"pthread_rwlock_rdlock", (void *)pthread_rwlock_rdlock},
        {"pthread_rwlock_wrlock", (void *)pthread_rwlock_wrlock},
        {"pthread_rwlock_unlock", (void *)pthread_rwlock_unlock},
        {"sem_init", (void *)sem_init}, {"sem_post", (void *)sem_post},
        {"sem_wait", (void *)sem_wait}, {"sem_timedwait", (void *)sem_timedwait},
        {"sem_getvalue", (void *)sem_getvalue},
        {"cos", (void *)cos}, {"cosf", (void *)cosf},
        {"sin", (void *)sin}, {"sinf", (void *)sinf},
        {"tan", (void *)tan}, {"tanf", (void *)tanf},
        {"acos", (void *)acos}, {"acosf", (void *)acosf},
        {"asin", (void *)asin}, {"atan", (void *)atan},
        {"atanf", (void *)atanf}, {"atan2", (void *)atan2},
        {"atan2f", (void *)atan2f}, {"log", (void *)log},
        {"log10", (void *)log10}, {"log10f", (void *)log10f},
        {"log2", (void *)log2}, {"logb", (void *)logb},
        {"exp2f", (void *)exp2f}, {"pow", (void *)pow},
        {"powf", (void *)powf}, {"sqrt", (void *)sqrt},
        {"fmod", (void *)fmod}, {"fmodf", (void *)fmodf},
        {"hypot", (void *)hypot}, {"modf", (void *)modf},
        {"scalbn", (void *)scalbn}, {"difftime", (void *)difftime},
        {"sincosf", (void *)sincosf},
        {"android_set_abort_message", (void *)android_set_abort_message},
        {"newlocale", (void *)newlocale}, {"freelocale", (void *)freelocale},
        {"uselocale", (void *)uselocale}, {"localeconv", (void *)localeconv},
        {"setlocale", (void *)setlocale},
        {"isdigit_l", (void *)isdigit_l}, {"isxdigit_l", (void *)isxdigit_l},
        {"islower_l", (void *)islower_l}, {"isupper_l", (void *)isupper_l},
        {"iswalpha_l", (void *)iswalpha_l}, {"iswblank_l", (void *)iswblank_l},
        {"iswcntrl_l", (void *)iswcntrl_l}, {"iswdigit_l", (void *)iswdigit_l},
        {"iswlower_l", (void *)iswlower_l}, {"iswprint_l", (void *)iswprint_l},
        {"iswpunct_l", (void *)iswpunct_l}, {"iswspace_l", (void *)iswspace_l},
        {"iswupper_l", (void *)iswupper_l}, {"iswxdigit_l", (void *)iswxdigit_l},
        {"towlower", (void *)towlower}, {"towupper", (void *)towupper},
        {"towlower_l", (void *)towlower_l}, {"towupper_l", (void *)towupper_l},
        {"tolower_l", (void *)tolower_l}, {"toupper_l", (void *)toupper_l},
        {"btowc", (void *)btowc}, {"wctob", (void *)wctob},
        {"wcslen", (void *)wcslen}, {"wmemchr", (void *)wmemchr},
        {"wmemcmp", (void *)wmemcmp}, {"wmemcpy", (void *)wmemcpy},
        {"wmemmove", (void *)wmemmove}, {"wmemset", (void *)wmemset},
        {"mbrlen", (void *)mbrlen}, {"mbrtowc", (void *)mbrtowc},
        {"mbtowc", (void *)mbtowc}, {"wcrtomb", (void *)wcrtomb},
        {"mbsrtowcs", (void *)mbsrtowcs}, {"mbsnrtowcs", (void *)mbsnrtowcs},
        {"wcsnrtombs", (void *)wcsnrtombs},
        {"__ctype_get_mb_cur_max", (void *)__ctype_get_mb_cur_max},
        {"div", (void *)div}, {"bsearch", (void *)bsearch},
        {"qsort", (void *)qsort}, {"lrand48", (void *)lrand48},
        {"srand48", (void *)srand48}, {"gmtime", (void *)gmtime},
        {"localtime", (void *)localtime}, {"mktime", (void *)mktime},
        {"strftime", (void *)strftime}, {"strftime_l", (void *)strftime_l},
        {"strcoll_l", (void *)strcoll_l}, {"strxfrm_l", (void *)strxfrm_l},
        {"wcscoll_l", (void *)wcscoll_l}, {"wcsxfrm_l", (void *)wcsxfrm_l},
        {"fopen", (void *)fopen}, {"fclose", (void *)fclose},
        {"fflush", (void *)fflush}, {"fread", (void *)fread},
        {"fwrite", (void *)fwrite}, {"fputc", (void *)fputc},
        {"fseek", (void *)fseek}, {"fseeko", (void *)fseeko},
        {"ftello", (void *)ftello}, {"access", (void *)access},
        {"unlink", (void *)unlink}, {"rename", (void *)rename},
        {"chmod", (void *)chmod}, {"fchmod", (void *)fchmod},
        {"mkdir", (void *)mkdir}, {"rmdir", (void *)rmdir},
        {"link", (void *)link}, {"symlink", (void *)symlink},
        {"readlink", (void *)readlink}, {"lseek", (void *)lseek},
        {"dup", (void *)dup}, {"ftruncate", (void *)ftruncate},
        {"futimens", (void *)futimens}, {"utimes", (void *)utimes},
        {"ioctl", (void *)ioctl}, {"pipe", (void *)pipe},
        {"select", (void *)select}, {"sendfile", (void *)sendfile},
        {"opendir", (void *)opendir}, {"readdir", (void *)readdir},
        {"closedir", (void *)closedir}, {"stat", (void *)stat},
        {"lstat", (void *)lstat},
        {"sigaction", (void *)sigaction}, {"signal", (void *)signal},
        {"sigemptyset", (void *)sigemptyset}, {"sigfillset", (void *)sigfillset},
        {"sigaddset", (void *)sigaddset}, {"sigdelset", (void *)sigdelset},
        {"sigsuspend", (void *)sigsuspend}, {"madvise", (void *)madvise},
        {"munmap", (void *)munmap}, {"dladdr", (void *)dladdr},
        {"dlclose", (void *)dlclose}, {"dlerror", (void *)dlerror},
        {"dl_iterate_phdr", (void *)dl_iterate_phdr},
        {"syscall", (void *)syscall}, {"setjmp", (void *)setjmp},
        {"longjmp", (void *)longjmp}, {"strerror_r", (void *)strerror_r},
        {"snprintf", (void *)snprintf}, {"sprintf", (void *)sprintf},
        {"vsnprintf", (void *)vsnprintf}, {"swprintf", (void *)swprintf},
        {"vfprintf", (void *)vfprintf}, {"vasprintf", (void *)vasprintf},
        {"sscanf", (void *)sscanf}, {"vsscanf", (void *)vsscanf},
        {"openlog", (void *)openlog}, {"syslog", (void *)syslog},
        {"closelog", (void *)closelog},
        {"__android_log_print", (void *)__android_log_print},
        {"uname", (void *)uname},
        {"strtod", (void *)strtod}, {"strtof", (void *)strtof},
        {"strtold", (void *)strtold}, {"strtold_l", (void *)strtold_l},
        {"strtoll_l", (void *)strtoll_l}, {"strtoull_l", (void *)strtoull_l},
        {"wcstod", (void *)wcstod}, {"wcstof", (void *)wcstof},
        {"wcstold", (void *)wcstold}, {"wcstol", (void *)wcstol},
        {"wcstoll", (void *)wcstoll}, {"wcstoul", (void *)wcstoul},
        {"wcstoull", (void *)wcstoull},
        {"dlsym", (void *)dlsym}, {"dlopen", (void *)dlopen},
        {"read", (void *)read}, {"write", (void *)write},
        {"close", (void *)close}, {"open", (void *)open},
        {"mmap", (void *)mmap}, {"munmap", (void *)munmap},
        {"fstat", (void *)fstat},
        {0, 0},
    };
    for (int i = 0; tab[i].n; i++)
        if (!strcmp(tab[i].n, name)) return tab[i].p;
    return 0;
}
