/* gds_fs.c -- Android data-path redirection for GDS.
 * GDS's extracted data is a FLAT tree at <gamedir>/data/ (the contents of the
 * APK's assets/bin/Data, including 1143 hash-named asset files, plus
 * globalgamemanagers, boot.config, Managed/, global-metadata.dat, etc).
 * Unity/il2cpp opens Android-style paths ("assets/bin/Data/...", "bin/Data/...",
 * "assets/...") plus bare "global-metadata.dat".  We rewrite those to the flat
 * data/ dir so the game can find its files. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/stat.h>

#include "gds.h"

static char g_asset_dir[1024] = "data";

void gds_fs_set_data_dir(const char *dir) {
    if (dir) { snprintf(g_asset_dir, sizeof g_asset_dir, "%s", dir); }
}

extern char gds_gamedir[];

static const char *gds_redirect(const char *p, char *buf, size_t bufsz) {
    if (!p || !*p) return NULL;
    const char *rel = NULL;
    if (!strncmp(p, "assets/bin/Data/", 16)) rel = p + 16;
    else if (!strncmp(p, "bin/Data/", 9)) rel = p + 9;
    else if (!strncmp(p, "assets/", 7)) rel = p + 7;
    else if (!strncmp(p, "data/", 5)) rel = p + 5;
    if (!rel) {
        const char *base = strrchr(p, '/'); base = base ? base + 1 : p;
        if (!strcmp(base, "unity_app_guid") || !strcmp(base, "global-metadata.dat") ||
            !strcmp(base, "globalgamemanagers") || !strcmp(base, "boot.config") ||
            !strcmp(base, "unity default resources") || !strcmp(base, "unity_builtin_extra") ||
            !strncmp(base, "level", 5) || !strncmp(base, "sharedassets", 12) ||
            strstr(base, ".assets") || strstr(base, ".resS") || strstr(base, ".resource") ||
            strstr(base, "-resources.dat")) {
            if (!strcmp(base, "global-metadata.dat")) rel = "Managed/Metadata/global-metadata.dat";
            else rel = base;
        }
    }
    if (!rel) return NULL;
    unsigned i = 0, j = 0;
    for (; g_asset_dir[j] && i < bufsz - 2; i++, j++) buf[i] = g_asset_dir[j];
    if (i && buf[i-1] != '/') buf[i++] = '/';
    for (; *rel && i < bufsz - 2; i++, rel++) buf[i] = *rel;
    buf[i] = 0;
    return buf;
}

int gds_open(const char *p, int flags, ...) {
    va_list ap; va_start(ap, flags); mode_t mode = va_arg(ap, mode_t); va_end(ap);
    char buf[1024];
    const char *r = gds_redirect(p, buf, sizeof buf);
    return open(r ? r : p, flags, mode);
}
int gds_open64(const char *p, int flags, ...) {
    va_list ap; va_start(ap, flags); mode_t mode = va_arg(ap, mode_t); va_end(ap);
    char buf[1024];
    const char *r = gds_redirect(p, buf, sizeof buf);
    return open64(r ? r : p, flags, mode);
}
FILE *gds_fopen(const char *p, const char *mode) {
    char buf[1024];
    const char *r = gds_redirect(p, buf, sizeof buf);
    return fopen(r ? r : p, mode);
}
int gds_stat(const char *p, struct stat *st) {
    char buf[1024];
    const char *r = gds_redirect(p, buf, sizeof buf);
    return stat(r ? r : p, st);
}
int gds_lstat(const char *p, struct stat *st) {
    char buf[1024];
    const char *r = gds_redirect(p, buf, sizeof buf);
    return lstat(r ? r : p, st);
}
int gds_stat64(const char *p, struct stat *st) {
    char buf[1024];
    const char *r = gds_redirect(p, buf, sizeof buf);
    return stat64(r ? r : p, (struct stat64 *)st);
}
int gds_lstat64(const char *p, struct stat *st) {
    char buf[1024];
    const char *r = gds_redirect(p, buf, sizeof buf);
    return lstat64(r ? r : p, (struct stat64 *)st);
}
int gds_access(const char *p, int m) {
    char buf[1024];
    const char *r = gds_redirect(p, buf, sizeof buf);
    return access(r ? r : p, m);
}
int gds_faccessat(int dirfd, const char *p, int m, int f) {
    char buf[1024];
    const char *r = gds_redirect(p, buf, sizeof buf);
    return faccessat(dirfd, r ? r : p, m, f);
}
