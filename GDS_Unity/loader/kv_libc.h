/* kv_libc.h - shared declarations for the freestanding libc that backs the
 * loaded .so's imports (implemented in freestdlib.c and host_syms.c).
 *
 * These are the real libc symbol names libil2cpp.so imports; host_syms.c maps
 * the imported name to these via host_dlsym().  On the device they'd come from
 * glibc; under the bench we provide these freestanding versions.
 */
#ifndef KV_LIBC_H
#define KV_LIBC_H

typedef long ssize_t;
typedef unsigned long size_t;

/* mem / str */
void  *memcpy(void *d, const void *s, unsigned long n);
void  *memmove(void *d, const void *s, unsigned long n);
void  *memset(void *d, int c, unsigned long n);
int    memcmp(const void *a, const void *b, unsigned long n);
void  *memchr(const void *s, int c, unsigned long n);
unsigned long strlen(const char *s);
int    strcmp(const char *a, const char *b);
int    strncmp(const char *a, const char *b, unsigned long n);
char  *strcpy(char *d, const char *s);
char  *strncpy(char *d, const char *s, unsigned long n);
char  *strchr(const char *s, int c);
char  *strrchr(const char *s, int c);
char  *strstr(const char *h, const char *n);
char  *strdup(const char *s);
char  *strlcpy(char *d, const char *s, unsigned long n);

/* alloc */
void  *malloc(unsigned long sz);
void   free(void *p);
void  *calloc(unsigned long n, unsigned long sz);
void  *realloc(void *old, unsigned long sz);
void  *memalign(unsigned long align, unsigned long sz);
int    posix_memalign(void **memptr, unsigned long align, unsigned long sz);

/* int parsing */
int    atoi(const char *s);
long   atol(const char *s);
long   strtol(const char *s, char **end, int base);
unsigned long strtoul(const char *s, char **end, int base);
long long strtoll(const char *s, char **end, int base);
unsigned long long strtoull(const char *s, char **end, int base);

/* stdio */
int    printf(const char *fmt, ...);
int    fprintf(int fd, const char *fmt, ...);
int    snprintf(char *s, unsigned long n, const char *fmt, ...);
int    sprintf(char *s, const char *fmt, ...);
void   perror(const char *s);

/* process / sys */
void   exit(int code);
void   _exit(int code);
void   _Exit(int code);
void   abort(void);
int   *__errno(void);
extern char **environ;
extern void *__sF;
int    __cxa_atexit(void (*f)(void *), void *arg, void *dso);
void   __cxa_finalize(void *dso);
void   __stack_chk_fail(void);
void  *__memcpy_chk(void *d, const void *s, unsigned long n, unsigned long dlen);
void  *__memmove_chk(void *d, const void *s, unsigned long n, unsigned long dlen);
void  *__memset_chk(void *d, int c, unsigned long n, unsigned long dlen);
unsigned long __strlen_chk(const char *s, unsigned long slen);
int    __vsnprintf_chk(char *s, unsigned long n, int flag, unsigned long slen, const char *fmt, void *ap);
void   __FD_SET_chk(int fd, void *set);

/* fd / mmap */
int    open(const char *path, int flags);
ssize_t read(int fd, void *buf, unsigned long n);
ssize_t write(int fd, const void *buf, unsigned long n);
int    close(int fd);
int    munmap(void *a, unsigned long l);
void  *mmap(void *addr, unsigned long len, int prot, int flags, int fd, long off);
int    mprotect(void *addr, unsigned long len, int prot);
int    fstat(int fd, void *st);

/* time */
long   time(long *t);
int    gettimeofday(void *tv, void *tz);
long   clock(void);
int    clock_gettime(int c, void *tp);
int    clock_getres(int c, void *tp);
int    nanosleep(void *req, void *rem);
int    usleep(unsigned long u);

/* proc / misc */
int    getpid(void);
int    getuid(void);
int    geteuid(void);
int    getegid(void);
int    sched_yield(void);
int    getpagesize(void);
long   sysconf(int name);
int    isatty(int fd);
char  *getenv(const char *name);
int    setenv(const char *n, const char *v, int o);
int    unsetenv(const char *n);
int    gethostname(char *n, unsigned long len);
int    getcwd(char *b, unsigned long n);

/* dlsym */
void  *dlsym(void *handle, const char *name);
void  *dlopen(const char *name, int flags);

/* pthread (stubs in freestdlib.c) */
typedef unsigned long kv_pthread_t;
int pthread_mutex_init(void *m, void *a);
int pthread_mutex_destroy(void *m);
int pthread_mutex_lock(void *m);
int pthread_mutex_unlock(void *m);
int pthread_mutex_trylock(void *m);
int pthread_mutexattr_init(void *a);
int pthread_mutexattr_destroy(void *a);
int pthread_mutexattr_settype(void *a, int t);
int pthread_cond_destroy(void *c);
int pthread_cond_broadcast(void *c);
int pthread_cond_signal(void *c);
int pthread_cond_wait(void *c, void *m);
int pthread_cond_timedwait(void *c, void *m, void *t);
int pthread_create(kv_pthread_t *t, void *a, void *(*fn)(void *), void *arg);
int pthread_join(kv_pthread_t t, void **r);
int pthread_detach(kv_pthread_t t);
kv_pthread_t pthread_self(void);
int pthread_equal(kv_pthread_t a, kv_pthread_t b);
int pthread_once(void *c, void (*fn)(void));
int pthread_key_create(unsigned *k, void (*d)(void *));
int pthread_key_delete(unsigned k);
void *pthread_getspecific(unsigned k);
int pthread_setspecific(unsigned k, const void *v);
int pthread_sigmask(int h, void *s, void *o);
int pthread_kill(kv_pthread_t t, int s);
int pthread_atfork(void (*a)(void), void (*b)(void), void (*c)(void));
int pthread_attr_init(void *a);
int pthread_attr_destroy(void *a);
int pthread_attr_getstack(void *a, void **s, void **sz);
int pthread_getattr_np(kv_pthread_t t, void *a);
int pthread_setname_np(kv_pthread_t t, const char *n);
int pthread_rwlock_rdlock(void *l);
int pthread_rwlock_wrlock(void *l);
int pthread_rwlock_unlock(void *l);
int sem_init(void *s, int p, unsigned v);
int sem_post(void *s);
int sem_wait(void *s);
int sem_timedwait(void *s, void *t);
int sem_getvalue(void *s, int *v);

#endif /* KV_LIBC_H */
