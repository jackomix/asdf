/* musl_compat.h -- glibc-ism shims for the MUSL (sandbox/qemu) build only.
 * The R36S device build links glibc and never defines MUSL_BUILD; this header
 * is included by source files that use glibc-only identifiers so the loader
 * can also be built as a fully static musl binary for headless qemu-aarch64
 * reproduction runs.  Semantics are unchanged: on 64-bit, pread/lseek/mmap
 * are already 64-bit; the *_l locale variants degrade to the base call under
 * the POSIX/C locale the loader forces, and extra trailing args are ignored
 * by the aarch64 calling convention.
 *
 * These are OBJECT-LIKE defines on purpose: the import tables reference the
 * identifiers by name (not through a call), so function-like macros would not
 * expand.  musl headers never declare these names, so the defines cannot
 * collide with prototypes. */
#ifndef MUSL_COMPAT_H
#define MUSL_COMPAT_H

#ifdef MUSL_BUILD

#include <stdlib.h>
#include <locale.h>
#include <wchar.h>
#include <wctype.h>
#include <string.h>

/* 64-bit is native on aarch64: the largefile aliases are plain calls. */
#define pread64   pread
#define pwrite64  pwrite
#define lseek64   lseek
#define mmap64    mmap
#define fopen64   fopen
#define open64    open
#define fseeko64  fseeko
#define ftello64  ftello
#define stat64    stat
#define lstat64   lstat
#define fstat64   fstat
#define statfs64  statfs
#define statvfs64 statvfs
#define readdir64 readdir
#define truncate64 truncate
#define fstatat64 fstatat

/* locale-sensitive variants degrade to the base call (loader sets LC_ALL=C) */
#define strtoll_l  strtoll
#define strtoull_l strtoull
#define strtold_l  strtold
#define strtod_l   strtod
#define strtof_l   strtof
#define strtol_l   strtol
#define strtoul_l  strtoul
#define strcoll_l  strcoll
#define strxfrm_l  strxfrm
#define strcasecmp_l strcasecmp
#define strncasecmp_l strncasecmp
#define wcscoll_l  wcscoll
#define wcsxfrm_l  wcsxfrm
#define wcscasecmp_l wcscasecmp
#define wcsncasecmp_l wcsncasecmp

/* misc glibc-isms musl lacks (simple semantics) */

#endif /* MUSL_BUILD */
#endif /* MUSL_COMPAT_H */
