/* fs_redirect.c - redirect Unity's Android-style data paths to the local data/
 * dir for the GDS glibc loader.
 *
 * libunity.so reads its data files (unity_app_guid, globalgamemanagers,
 * level0, global-metadata.dat, sharedassets*, etc.) via DIRECT open()/fopen()/
 * stat() syscalls using Android paths like "assets/bin/Data/..." or
 * "bin/Data/..." (and the APK-relative "assets/...").  Our extracted data lives
 * in the "data/" folder next to the loader (the contents of assets/bin/Data),
 * so those opens fail -> Unity thinks unity_app_guid is empty -> re-extract ->
 * "Not enough storage space to install required resources." dialog -> boot hang.
 *
 * We intercept open/fopen/stat/lstat/access/fstat and rewrite the Android data
 * paths onto <asset_dir>/<relative>.  Same technique as terraria-nextos's
 * my_open/asset_redirect.
 */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <dirent.h>
#include <dlfcn.h>
#include <sys/types.h>

static char g_asset_dir[512] = "data";
static int g_inited;

/* Set by the loader (real_main) with the absolute path to the game's data dir. */
void kv_fs_set_data_dir(const char *dir) {
    if (dir) { strncpy(g_asset_dir, dir, sizeof g_asset_dir - 1); g_asset_dir[sizeof g_asset_dir - 1] = 0; }
    g_inited = 1;
}

/* Is this path something Unity reads from the data dir?  Rewrite it.  Returns
 * the rewritten absolute path in buf (static), or NULL if no redirect. */
static const char *fs_redirect(const char *p, char *buf, size_t bufsz) {
    if (!p || !*p) return NULL;
    /* strip Android data prefixes and locate the relative part */
    const char *rel = NULL;
    if (!strncmp(p, "assets/bin/Data/", 16)) rel = p + 16;
    else if (!strncmp(p, "bin/Data/", 9)) rel = p + 9;
    else if (!strncmp(p, "assets/", 7)) rel = p + 7;
    else if (!strncmp(p, "data/", 5)) rel = p + 5;   /* rare */
    /* also handle bare well-known data files Unity might open with no prefix */
    if (!rel) {
        const char *base = strrchr(p, '/'); base = base ? base + 1 : p;
        if (!strcmp(base, "unity_app_guid") || !strcmp(base, "global-metadata.dat") ||
            !strcmp(base, "globalgamemanagers") || !strcmp(base, "boot.config") ||
            !strcmp(base, "unity default resources") || !strcmp(base, "unity_builtin_extra") ||
            !strncmp(base, "level", 5) || !strncmp(base, "sharedassets", 12) ||
            strstr(base, ".assets") || strstr(base, ".resS") || strstr(base, ".resource") ||
            strstr(base, "-resources.dat")) {
            rel = base;
        }
    }
    if (!rel) return NULL;
    /* build <asset_dir>/<rel> */
    unsigned i = 0, j = 0;
    for (; g_asset_dir[j] && i < bufsz - 2; i++, j++) buf[i] = g_asset_dir[j];
    if (i && buf[i-1] != '/') buf[i++] = '/';
    for (; *rel && i < bufsz - 2; i++, rel++) buf[i] = *rel;
    buf[i] = 0;
    return buf;
}

/* ---- real glibc fns (resolved once) ---- */
static int (*r_open)(const char *, int, ...);
static int (*r_open64)(const char *, int, ...);
static FILE *(*r_fopen)(const char *, const char *);
static int (*r_stat)(const char *, struct stat *);
static int (*r_stat64)(const char *, struct stat *);
static int (*r_lstat)(const char *, struct stat *);
static int (*r_lstat64)(const char *, struct stat *);
static int (*r_access)(const char *, int);
static int (*r_fstat)(int, struct stat *);
static DIR *(*r_opendir)(const char *);
static struct dirent *(*r_readdir)(DIR *);
static int (*r_closedir)(DIR *);
static void fs_load_real(void) {
    if (g_inited && r_open) return;
    r_open   = (int (*)(const char *, int, ...))dlsym(RTLD_DEFAULT, "open");
    r_open64 = (int (*)(const char *, int, ...))dlsym(RTLD_DEFAULT, "open64");
    r_fopen  = (FILE *(*)(const char *, const char *))dlsym(RTLD_DEFAULT, "fopen");
    r_stat   = (int (*)(const char *, struct stat *))dlsym(RTLD_DEFAULT, "stat");
    r_stat64 = (int (*)(const char *, struct stat *))dlsym(RTLD_DEFAULT, "stat64");
    r_lstat  = (int (*)(const char *, struct stat *))dlsym(RTLD_DEFAULT, "lstat");
    r_lstat64= (int (*)(const char *, struct stat *))dlsym(RTLD_DEFAULT, "lstat64");
    r_access = (int (*)(const char *, int))dlsym(RTLD_DEFAULT, "access");
    r_fstat  = (int (*)(int, struct stat *))dlsym(RTLD_DEFAULT, "fstat");
    r_opendir= (DIR *(*)(const char *))dlsym(RTLD_DEFAULT, "opendir");
    r_readdir= (struct dirent *(*)(DIR *))dlsym(RTLD_DEFAULT, "readdir");
    r_closedir=(int (*)(DIR *))dlsym(RTLD_DEFAULT, "closedir");
    if (!r_open) r_open = r_open64;
    if (!r_stat) r_stat = r_stat64;
    if (!r_lstat) r_lstat = r_lstat64;
    if (!r_fopen) r_fopen = (FILE *(*)(const char *, const char *))dlsym(RTLD_DEFAULT, "fopen64");
    g_inited = 1;
}

/* ---- fake /sys/devices/system/cpu/ directory: only "cpu0" ----
 * Unity sizes its job-worker pool by scanning the /sys/devices/system/cpu/
 * cpu* directories via opendir/readdir (not just reading the online/present
 * files that kv_open already intercepts).  We return a fake dir containing a
 * single "cpu0" entry -> 1 CPU -> 0 job workers -> jobs run inline. */
struct kv_fake_dir { int active; int n; };
static struct kv_fake_dir g_fake_cpudir;
static int is_cpu_dir(const char *p) {
    return p && (strstr(p, "/sys/devices/system/cpu") != NULL);
}
static DIR *kv_opendir(const char *p) {
    fs_load_real();
    if (is_cpu_dir(p)) {
        g_fake_cpudir.active = 1; g_fake_cpudir.n = 0;
        printf("[fs] injected opendir /sys/devices/system/cpu (1 CPU)\n");
        return (DIR *)&g_fake_cpudir;
    }
    return r_opendir ? r_opendir(p) : NULL;
}
static struct dirent *kv_readdir(DIR *dir) {
    fs_load_real();
    if (dir == (DIR *)&g_fake_cpudir) {
        static struct dirent de;
        if (g_fake_cpudir.n++ == 0) {
            memset(&de, 0, sizeof de);
            strcpy(de.d_name, "cpu0");
            return &de;
        }
        return NULL;
    }
    return r_readdir ? r_readdir(dir) : NULL;
}
static int kv_closedir(DIR *dir) {
    if (dir == (DIR *)&g_fake_cpudir) { g_fake_cpudir.active = 0; return 0; }
    return r_closedir ? r_closedir(dir) : 0;
}

/* ---- command-line injection: force single-threaded rendering ---- */
/* Unity renders on a separate GfxDeviceWorker thread.  On Android the Java
 * Activity drives that worker; our loader only calls nativeRender on the main
 * thread, so the worker never runs and GL is never exercised (zero egl calls
 * in the log).  Terraria fixes this by injecting -force-gfx-direct + -force-gles20
 * into the process cmdline, which Unity reads from /proc/self/cmdline, forcing
 * rendering onto the main thread.  We intercept open() of ".../cmdline" and
 * return a synthetic file with those args. */
static int kv_cmdline_fd(void) {
    char buf[256]; int n = 0;
    n += sprintf(buf + n, "GameDevStory") + 1;
    n += sprintf(buf + n, "-force-gfx-direct") + 1;
    n += sprintf(buf + n, "-force-gles20") + 1;
    FILE *t = tmpfile();
    if (!t) return -1;
    fwrite(buf, 1, (size_t)n, t); fflush(t);
    int fd = dup(fileno(t)); fclose(t); lseek(fd, 0, SEEK_SET);
    printf("[fs] injected cmdline: -force-gfx-direct -force-gles20\n");
    return fd;
}

/* Synthetic /proc/cpuinfo that reports a SINGLE CPU.  Unity counts "processor:"
 * lines to size its job-worker pool (num_cpus - 1).  The sched_getaffinity/sysconf
 * shims (bionic_bridge.c) weren't enough - Unity also reads /proc/cpuinfo directly.
 * Reporting 1 processor -> 0 job workers -> jobs run inline (Terraria's approach). */
static int kv_cpuinfo_fd(void) {
    static const char cpuinfo[] =
        "processor\t: 0\n"
        "model name\t: ARMv8\n"
        "CPU implementer\t: 0x41\n"
        "CPU architecture: 8\n"
        "CPU variant\t: 0x0\n"
        "CPU part\t: 0xd05\n"
        "CPU revision\t: 1\n"
        "\n";
    FILE *t = tmpfile();
    if (!t) return -1;
    fwrite(cpuinfo, 1, sizeof(cpuinfo) - 1, t); fflush(t);
    int fd = dup(fileno(t)); fclose(t); lseek(fd, 0, SEEK_SET);
    printf("[fs] injected /proc/cpuinfo (1 CPU)\n");
    return fd;
}
/* Synthetic /sys/devices/system/cpu/{present,possible,online} = "0" (1 CPU). */
static int kv_sys_cpu_fd(void) {
    FILE *t = tmpfile();
    if (!t) return -1;
    fwrite("0\n", 1, 2, t); fflush(t);
    int fd = dup(fileno(t)); fclose(t); lseek(fd, 0, SEEK_SET);
    printf("[fs] injected /sys/.../cpu (1 CPU)\n");
    return fd;
}

/* ---- intercepted entry points ---- */
int kv_open(const char *p, int flags, ...) {
    fs_load_real();
    if (!r_open) return -1;
    if (p && (strstr(p, "cpuinfo") || strstr(p, "cpu/") || strstr(p, "cpu_") || strstr(p, "/sys/devices/system/cpu") || strstr(p, "/proc/stat") || strstr(p, "thermal") || strstr(p, "cpufreq") || strstr(p, "topology"))) {
        printf("[fs] open %s\n", p);
    }
    if (p) {
        if (strstr(p, "cmdline")) { int fd = kv_cmdline_fd(); if (fd >= 0) return fd; }
        if (strstr(p, "cpuinfo")) { int fd = kv_cpuinfo_fd(); if (fd >= 0) return fd; }
        if (strstr(p, "/sys/devices/system/cpu/")) { int fd = kv_sys_cpu_fd(); if (fd >= 0) return fd; }
    }
    va_list ap; va_start(ap, flags);
    mode_t mode = va_arg(ap, mode_t);
    va_end(ap);
    char buf[512];
    const char *r = fs_redirect(p, buf, sizeof buf);
    if (r) return r_open(r, flags, mode);
    return r_open(p, flags, mode);
}
int kv_open64(const char *p, int flags, ...) {
    fs_load_real();
    if (!r_open64) return kv_open(p, flags);
    va_list ap; va_start(ap, flags);
    mode_t mode = va_arg(ap, mode_t);
    va_end(ap);
    char buf[512];
    const char *r = fs_redirect(p, buf, sizeof buf);
    if (r) return r_open64(r, flags, mode);
    return r_open64(p, flags, mode);
}
FILE *kv_fopen(const char *p, const char *mode) {
    fs_load_real();
    if (!r_fopen) return NULL;
    if (p) {
        if (strstr(p, "cmdline")) { int fd = kv_cmdline_fd(); if (fd >= 0) return fdopen(fd, "r"); }
        if (strstr(p, "cpuinfo")) { int fd = kv_cpuinfo_fd(); if (fd >= 0) return fdopen(fd, "r"); }
        if (strstr(p, "/sys/devices/system/cpu/")) { int fd = kv_sys_cpu_fd(); if (fd >= 0) return fdopen(fd, "r"); }
    }
    char buf[512];
    const char *r = fs_redirect(p, buf, sizeof buf);
    if (r) return r_fopen(r, mode);
    return r_fopen(p, mode);
}
int kv_stat(const char *p, struct stat *st) {
    fs_load_real();
    if (!r_stat) return -1;
    char buf[512];
    const char *r = fs_redirect(p, buf, sizeof buf);
    if (r) return r_stat(r, st);
    return r_stat(p, st);
}
int kv_lstat(const char *p, struct stat *st) {
    fs_load_real();
    if (!r_lstat) return -1;
    char buf[512];
    const char *r = fs_redirect(p, buf, sizeof buf);
    if (r) return r_lstat(r, st);
    return r_lstat(p, st);
}
int kv_access(const char *p, int m) {
    fs_load_real();
    if (!r_access) return -1;
    char buf[512];
    const char *r = fs_redirect(p, buf, sizeof buf);
    if (r) return r_access(r, m);
    return r_access(p, m);
}

/* ---- route table ---- */
void *kv_fs_route(const char *name) {
    static const struct { const char *n; void *f; } m[] = {
        {"open", kv_open}, {"open64", kv_open64},
        {"fopen", kv_fopen}, {"fopen64", kv_fopen},
        {"stat", kv_stat}, {"stat64", kv_stat},
        {"lstat", kv_lstat}, {"lstat64", kv_lstat},
        {"access", kv_access},
        {"opendir", kv_opendir}, {"opendir64", kv_opendir},
        {"readdir", kv_readdir}, {"readdir64", kv_readdir},
        {"closedir", kv_closedir},
        {0, 0}
    };
    for (int i = 0; m[i].n; i++) if (strcmp(m[i].n, name) == 0) return m[i].f;
    return NULL;
}
