/* host_syms.c - symbols the loaded .so resolves against.
 *
 * libil2cpp.so is built for bionic and imports a handful of libc/libm/bionic
 * helpers.  The native loader resolves them here.  On the device these come
 * from glibc; under the Unicorn bench we provide freestanding versions so the
 * .so's init code can run headless.
 */
#include <stddef.h>
#include <stdint.h>
#include "kv_elf.h"

/* libc memory/string builtins the .so imports. */
void *kv_memcpy(void *d, const void *s, size_t n) { return memcpy(d, s, n); }
void *kv_memset(void *d, int c, size_t n) { return memset(d, c, n); }
int   kv_memcmp(const void *a, const void *b, size_t n) { return memcmp(a, b, n); }
size_t kv_strlen(const char *s) { return strlen(s); }
int   kv_strcmp(const char *a, const char *b) {
    for (;; a++, b++) {
        unsigned char ca = *a, cb = *b;
        if (ca != cb) return (int)ca - (int)cb;
        if (ca == 0) return 0;
    }
}
int   kv_strncmp(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (a[i] != b[i]) return (unsigned char)a[i] - (unsigned char)b[i];
        if (a[i] == 0) return 0;
    }
    return 0;
}
char *kv_strcpy(char *d, const char *s) { char *r = d; while ((*r++ = *s++)); return d; }
void  kv_abort(void) { for (;;) {} }

/* bionic property read: return empty (the device shim fills real props). */
int __system_property_get(const char *name, char *value) {
    if (value) value[0] = 0;
    (void)name;
    return 0;
}
const char *__system_property_get_nocache(const char *name, const char *n2, char *v) {
    (void)name; (void)n2; if (v) v[0] = 0; return v;
}

void *host_dlsym(const char *name) {
    /* exact-name table; keep in sync with what libil2cpp.so imports. */
    static const struct { const char *n; void *p; } tab[] = {
        {"memcpy", (void *)kv_memcpy},
        {"memset", (void *)kv_memset},
        {"memcmp", (void *)kv_memcmp},
        {"strlen", (void *)kv_strlen},
        {"strcmp", (void *)kv_strcmp},
        {"strncmp", (void *)kv_strncmp},
        {"strcpy", (void *)kv_strcpy},
        {"__memcpy_chk", (void *)kv_memcpy},
        {"__memset_chk", (void *)kv_memset},
        {"__system_property_get", (void *)__system_property_get},
        {"abort", (void *)kv_abort},
        {0, 0},
    };
    for (int i = 0; tab[i].n; i++)
        if (!kv_strcmp(tab[i].n, name)) return tab[i].p;
    return 0;
}
