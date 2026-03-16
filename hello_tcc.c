/* CPUTwo TCC smoke-test — no stdlib, direct UART MMIO */
// eg build via:
//
// ../tinycc_CPUTwo/cputwo-tcc -nostdlib hello_tcc.c -o hello_tcc.elf -B../tinycc_CPUTwo

#define UART_STATUS  (*(volatile unsigned int *)0x03F00000u)
#define UART_TX      (*(volatile unsigned int *)0x03F00004u)

static void uart_putc(unsigned char c)
{
    while (!(UART_STATUS & 1))
        ;
    UART_TX = c;
}

static void uart_puts(const char *s)
{
    while (*s)
        uart_putc((unsigned char)*s++);
}

static void uart_puthex(unsigned int v)
{
    int i;
    uart_puts("0x");
    for (i = 28; i >= 0; i -= 4) {
        unsigned int nybble = (v >> i) & 0xF;
        uart_putc(nybble < 10 ? '0' + nybble : 'A' + nybble - 10);
    }
}

int main(void)
{
    unsigned int a = 6, b = 7;

    uart_puts("Hello from TCC on CPUTwo!\n");

    uart_puts("6 * 7 = ");
    uart_puthex(a * b);
    uart_puts("\n");

    uart_puts("0xDEAD + 0xBEEF = ");
    uart_puthex(0xDEADu + 0xBEEFu);
    uart_puts("\n");

    uart_puts("Done.\n");
    return 0;
}
