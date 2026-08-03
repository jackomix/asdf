/* rawtest.c - bypass printf; call the write syscall directly. */
static long sys_write(int fd, const char *buf, unsigned long n) {
    register long x8 __asm__("x8") = 64;
    register long x0 __asm__("x0") = fd;
    register long x1 __asm__("x1") = (long)buf;
    register long x2 __asm__("x2") = (long)n;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2) : "memory");
    return x0;
}
static const char msg[] = "RAWWRITE OK\n";
void _start(void) {
    for (;;) {
        sys_write(1, msg, sizeof(msg) - 1);
    }
}
