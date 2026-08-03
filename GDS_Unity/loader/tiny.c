/* tiny.c -- freestanding aarch64 test program.
 * No libc: _start calls the write/exit syscalls directly, so it has no
 * dynamic dependencies.  This is the shape our native R36S loader will have,
 * and it lets run_aarch64.py (Unicorn) exercise the loader's ELF mapping,
 * RELATIVE relocs and init_array without needing a C runtime.
 */
static long sys_write(int fd, const char *buf, unsigned long n) {
    register long x8 __asm__("x8") = 64;
    register long x0 __asm__("x0") = fd;
    register long x1 __asm__("x1") = (long)buf;
    register long x2 __asm__("x2") = (long)n;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2) : "memory");
    return x0;
}
static void sys_exit(int code) {
    register long x8 __asm__("x8") = 93;
    register long x0 __asm__("x0") = code;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8) : "memory");
    for (;;) {}
}

/* A global that RELATIVE relocation will have fixed up by the loader. */
static const char msg[] = "[tiny] freestanding aarch64 runs\n";

/* An init_array entry, to prove the loader runs constructors. */
static void _ctor(void) {
    sys_write(1, "[tiny] init_array ran\n", 22);
}
__attribute__((used, section(".init_array")))
void (*__init_ptr)(void) = &_ctor;

void _start(void) {
    sys_write(1, msg, sizeof(msg) - 1);
    sys_exit(0);
}
