/* elf.h - minimal AArch64 ELF loader types for the R36S native port.
 *
 * We load the APK's Android .so files the way Android's linker does, so the
 * shipped libil2cpp.so / libunity.so run on a glibc Linux box without the
 * app's own linker.  Only what the loader needs is declared here.
 */
#ifndef KV_ELF_H
#define KV_ELF_H

#include <stdint.h>

typedef long ssize_t;
typedef unsigned long size_t;

#define PROT_READ 1
#define PROT_WRITE 2
#define PROT_EXEC 4
#define MAP_PRIVATE 0x2
#define MAP_ANONYMOUS 0x20
#define MAP_FIXED_NOREPLACE 0x100000
#define O_RDONLY 0
#define MAP_FAILED ((void *)-1)

#define KV_PAGE 4096u

typedef uint64_t Elf64_Addr;
typedef uint64_t Elf64_Off;
typedef uint16_t Elf64_Half;
typedef uint32_t Elf64_Word;
typedef int32_t  Elf64_Sword;
typedef uint64_t Elf64_Xword;
typedef int64_t  Elf64_Sxword;

typedef struct {
    unsigned char e_ident[16];
    Elf64_Half    e_type;
    Elf64_Half    e_machine;
    Elf64_Word    e_version;
    Elf64_Addr    e_entry;
    Elf64_Off     e_phoff;
    Elf64_Off     e_shoff;
    Elf64_Word    e_flags;
    Elf64_Half    e_ehsize;
    Elf64_Half    e_phentsize;
    Elf64_Half    e_phnum;
    Elf64_Half    e_shentsize;
    Elf64_Half    e_shnum;
    Elf64_Half    e_shstrndx;
} Elf64_Ehdr;

typedef struct {
    Elf64_Word p_type;
    Elf64_Word p_flags;
    Elf64_Off  p_offset;
    Elf64_Addr p_vaddr;
    Elf64_Addr p_paddr;
    Elf64_Xword p_filesz;
    Elf64_Xword p_memsz;
    Elf64_Xword p_align;
} Elf64_Phdr;

/* Dynamic section entries. */
#define DT_NULL     0
#define DT_NEEDED   1
#define DT_PLTRELSZ 2
#define DT_PLTGOT   3
#define DT_HASH     4
#define DT_STRTAB   5
#define DT_SYMTAB   6
#define DT_RELA     7
#define DT_RELASZ   8
#define DT_RELAENT  9
#define DT_STRSZ    10
#define DT_SYMENT   11
#define DT_INIT     12
#define DT_FINI     13
#define DT_PLTREL   20
#define DT_JMPREL   23
#define DT_INIT_ARRAY 25
#define DT_FINI_ARRAY  26
#define DT_INIT_ARRAYSZ 27
#define DT_FINI_ARRAYSZ 28
#define DT_RELACOUNT 0x6ffffff9
#define DT_FLAGS     30
#define DT_GNU_HASH  0x6ffffef5
#define DT_VERSYM   0x6ffffff0
#define DT_VERNEED  0x6ffffffe
#define DT_VERNEEDNUM 0x6fffffff

/* symbol table special index: symbol is undefined (imported) */
#define SHN_UNDEF   0

typedef struct {
    Elf64_Sxword d_tag;
    union { Elf64_Addr d_val; Elf64_Addr d_ptr; } d_un;
} Elf64_Dyn;

/* Symbol table entry. */
typedef struct {
    Elf64_Word    st_name;
    unsigned char st_info;
    unsigned char st_other;
    Elf64_Half    st_shndx;
    Elf64_Addr    st_value;
    Elf64_Xword   st_size;
} Elf64_Sym;

/* Relocation (RELATIVE only; we resolve the rest against our own symbols). */
typedef struct {
    Elf64_Addr   r_offset;
    Elf64_Xword  r_info;
    Elf64_Sxword r_addend;
} Elf64_Rela;

#define R_AARCH64_NONE         0
#define R_AARCH64_ABS64        257
#define R_AARCH64_COPY         1024
#define R_AARCH64_GLOB_DAT     1025
#define R_AARCH64_JUMP_SLOT    1026
#define R_AARCH64_RELATIVE     1027
#define R_AARCH64_TLS_DTPMOD   1028
#define R_AARCH64_TLS_DTPREL   1029
#define R_AARCH64_TLS_TPREL    1030
#define R_AARCH64_ABS32        258
#define R_AARCH64_ADR_GOT_PAGE 1138 /* may appear; treated as GLOB_DAT-ish */

/* ---- libc-style prototypes the loader uses (provided by freestdlib.c) ---- */
int    printf(const char *fmt, ...);
int    fprintf(int fd, const char *fmt, ...);
void   perror(const char *s);
void  *malloc(unsigned long sz);
void  *calloc(unsigned long n, unsigned long sz);
void   free(void *p);
void  *memset(void *d, int c, unsigned long n);
void  *memcpy(void *d, const void *s, unsigned long n);
unsigned long strlen(const char *s);
int    strncmp(const char *a, const char *b, unsigned long n);
int    open(const char *path, int flags);
ssize_t read(int fd, void *buf, unsigned long n);
int    close(int fd);
void  *mmap(void *addr, unsigned long len, int prot, int flags, int fd, long off);
int    mprotect(void *addr, unsigned long len, int prot);
void  *dlsym(void *handle, const char *name);
void  *host_dlsym(const char *name);
void   exit(int code);
int    fstat(int fd, void *st);
char  *strncpy(char *d, const char *s, unsigned long n);
int    memcmp(const void *a, const void *b, unsigned long n);
long   lseek(int fd, long off, int whence);
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

#endif /* KV_ELF_H */
