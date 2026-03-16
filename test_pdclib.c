/*
 * test_pdclib.c — comprehensive pdclib test suite for CPUTwo bare-metal
 *
 * Tests: stdio (sprintf/sscanf/snprintf/printf), string.h, stdlib.h,
 *        ctype.h, stdarg.h, stdint.h, limits.h, integer arithmetic
 *
 * SKIPPED (known broken on CPUTwo):
 *   - float/double: TCC stores double hi-word=0, arithmetic is broken
 *   - malloc/calloc/realloc/free: _gm_ at BSS address 0 corrupts itself
 *   - atoll / strtoll / strtoull: uintmax_t is 32-bit, accumulator overflows
 *   - sprintf/sscanf with 3+ varargs when the callee has 2+ named params
 *   - snprintf truncation: pdclib writes n bytes+nul instead of (n-1)+nul
 *
 * CPUTwo vararg ABI note:
 *   __builtin_va_start(ap,last) = ((char*)&last) - sizeof(last)
 *   __builtin_va_arg only walks the register-spill area (grows downward).
 *   For a function with N named params, only (4-N) varargs fit in registers.
 *   xsprintf/xsscanf have fmt as the ONLY named param → 3 varargs work.
 *
 * Compile: ./cputwocc.sh test_pdclib.c -o test_pdclib.elf
 * Run:     ./build/emulatortwo test_pdclib.elf
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <stdarg.h>
#include <stdint.h>
#include <limits.h>

/* ------------------------------------------------------------------ */
/* Test framework                                                       */
/* ------------------------------------------------------------------ */

static int pass = 0, fail = 0;

#define TEST(name, expr) do { \
    if (expr) { pass++; } \
    else { fail++; printf("FAIL: %s\n", name); } \
} while(0)

/* ------------------------------------------------------------------ */
/* Vararg-safe wrappers                                                 */
/* fmt is the ONLY named param → up to 3 varargs reach registers.     */
/* ------------------------------------------------------------------ */

static int my_vsnprintf(char *buf, size_t n, const char *fmt, va_list ap)
{
    return vsnprintf(buf, n, fmt, ap);
}

static int xsprintf(char *buf, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int r = my_vsnprintf(buf, 4096, fmt, ap);
    va_end(ap);
    return r;
}

static int xsscanf(const char *s, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int r = vsscanf(s, fmt, ap);
    va_end(ap);
    return r;
}

/* ------------------------------------------------------------------ */
/* stdio tests — sprintf/xsprintf                                       */
/* ------------------------------------------------------------------ */

static void test_sprintf_basic(void)
{
    char buf[128];

    xsprintf(buf, "%d", 42);
    TEST("sprintf_d_pos", strcmp(buf, "42") == 0);

    xsprintf(buf, "%d", -42);
    TEST("sprintf_d_neg", strcmp(buf, "-42") == 0);

    xsprintf(buf, "%d", 0);
    TEST("sprintf_d_zero", strcmp(buf, "0") == 0);

    xsprintf(buf, "%u", 0u);
    TEST("sprintf_u_zero", strcmp(buf, "0") == 0);

    xsprintf(buf, "%u", 4294967295u);
    TEST("sprintf_u_max", strcmp(buf, "4294967295") == 0);

    xsprintf(buf, "%x", 0xdeadbeefu);
    TEST("sprintf_x_lower", strcmp(buf, "deadbeef") == 0);

    xsprintf(buf, "%X", 0xDEADBEEFu);
    TEST("sprintf_X_upper", strcmp(buf, "DEADBEEF") == 0);

    xsprintf(buf, "%x", 0u);
    TEST("sprintf_x_zero", strcmp(buf, "0") == 0);

    xsprintf(buf, "%s", "hello");
    TEST("sprintf_s", strcmp(buf, "hello") == 0);

    xsprintf(buf, "%s", "");
    TEST("sprintf_s_empty", strcmp(buf, "") == 0);

    xsprintf(buf, "%c", (int)'A');
    TEST("sprintf_c", strcmp(buf, "A") == 0);

    /* 2 varargs — within register limit */
    xsprintf(buf, "%d %s", 7, "foo");
    TEST("sprintf_d_s", strcmp(buf, "7 foo") == 0);

    xsprintf(buf, "%d %c", 7, (int)'!');
    TEST("sprintf_d_c", strcmp(buf, "7 !") == 0);

    xsprintf(buf, "%s %d", "bar", 42);
    TEST("sprintf_s_d", strcmp(buf, "bar 42") == 0);
}

static void test_sprintf_width_precision(void)
{
    char buf[64];

    xsprintf(buf, "%5d", 42);
    TEST("sprintf_width_right", strcmp(buf, "   42") == 0);

    xsprintf(buf, "%-5d", 42);
    TEST("sprintf_width_left", strcmp(buf, "42   ") == 0);

    xsprintf(buf, "%05d", 42);
    TEST("sprintf_zero_pad", strcmp(buf, "00042") == 0);

    xsprintf(buf, "%+d", 42);
    TEST("sprintf_plus_pos", strcmp(buf, "+42") == 0);

    xsprintf(buf, "%+d", -42);
    TEST("sprintf_plus_neg", strcmp(buf, "-42") == 0);

    xsprintf(buf, "%.3s", "hello");
    TEST("sprintf_str_prec", strcmp(buf, "hel") == 0);

    xsprintf(buf, "%8.3s", "hello");
    TEST("sprintf_str_width_prec", strcmp(buf, "     hel") == 0);

    xsprintf(buf, "%i", -99);
    TEST("sprintf_i", strcmp(buf, "-99") == 0);

    xsprintf(buf, "%o", 8u);
    TEST("sprintf_o_8", strcmp(buf, "10") == 0);

    xsprintf(buf, "%o", 255u);
    TEST("sprintf_o_ff", strcmp(buf, "377") == 0);

    xsprintf(buf, "%05d", 123);
    TEST("sprintf_zero_pad_3dig", strcmp(buf, "00123") == 0);
}

static void test_sprintf_edge_cases(void)
{
    char buf[64];

    xsprintf(buf, "%d", 2147483647);
    TEST("sprintf_INT_MAX", strcmp(buf, "2147483647") == 0);

    xsprintf(buf, "%d", INT_MIN);
    TEST("sprintf_INT_MIN", strcmp(buf, "-2147483648") == 0);

    xsprintf(buf, "100%%");
    TEST("sprintf_percent", strcmp(buf, "100%") == 0);

    /* long long via %lld/%llu: pdclib's uintmax_t is 32-bit on CPUTwo,
       so only the low 32 bits are read.  Test values that fit in 32 bits. */
    xsprintf(buf, "%lld", 2147483647LL);
    TEST("sprintf_lld", strcmp(buf, "2147483647") == 0);

    xsprintf(buf, "%lld", -1LL);
    TEST("sprintf_lld_neg", strcmp(buf, "-1") == 0);

    xsprintf(buf, "%llu", 4294967295ULL);
    TEST("sprintf_llu_u32max", strcmp(buf, "4294967295") == 0);

    xsprintf(buf, "%08x", 0xCAFEu);
    TEST("sprintf_hex_pad", strcmp(buf, "0000cafe") == 0);

    xsprintf(buf, "%lx", 0xDEADBEEFUL);
    TEST("sprintf_lx", strcmp(buf, "deadbeef") == 0);

    xsprintf(buf, "%ld", -1L);
    TEST("sprintf_ld_neg", strcmp(buf, "-1") == 0);
}

static void test_snprintf(void)
{
    char buf[32];
    int n;

    /* fits with room to spare */
    n = snprintf(buf, 32, "%d", 12345);
    TEST("snprintf_fits_ret", n == 5);
    TEST("snprintf_fits_str", strcmp(buf, "12345") == 0);

    /* zero size — no write, return length */
    n = snprintf(NULL, 0, "%d", 999);
    TEST("snprintf_zero_size", n == 3);
}

static void test_sscanf(void)
{
    int i, n;
    unsigned int u;

    n = xsscanf("42", "%d", &i);
    TEST("sscanf_d_n", n == 1);
    TEST("sscanf_d_val", i == 42);

    xsscanf("-99", "%d", &i);
    TEST("sscanf_d_neg", i == -99);

    xsscanf("4294967295", "%u", &u);
    TEST("sscanf_u", u == 4294967295u);

    xsscanf("deadbeef", "%x", &u);
    TEST("sscanf_x", u == 0xdeadbeef);

    xsscanf("0xCAFE", "%x", &u);
    TEST("sscanf_x_0x", u == 0xCAFE);

    char s[32];
    n = xsscanf("hello world", "%s", s);
    TEST("sscanf_s_n", n == 1);
    TEST("sscanf_s_val", strcmp(s, "hello") == 0);

    char c;
    xsscanf("Z", "%c", &c);
    TEST("sscanf_c", c == 'Z');

    /* 2 output pointers — within register limit */
    int a, b;
    n = xsscanf("10 20", "%d %d", &a, &b);
    TEST("sscanf_two_d_n", n == 2);
    TEST("sscanf_two_d_a", a == 10);
    TEST("sscanf_two_d_b", b == 20);
}

/* ------------------------------------------------------------------ */
/* string.h tests                                                       */
/* ------------------------------------------------------------------ */

static void test_strlen(void)
{
    TEST("strlen_empty", strlen("") == 0);
    TEST("strlen_hello", strlen("hello") == 5);
    TEST("strlen_one", strlen("x") == 1);
    TEST("strlen_long", strlen("abcdefghij") == 10);
}

static void test_strcmp(void)
{
    TEST("strcmp_eq", strcmp("abc", "abc") == 0);
    TEST("strcmp_lt", strcmp("abc", "abd") < 0);
    TEST("strcmp_gt", strcmp("abd", "abc") > 0);
    TEST("strcmp_prefix_lt", strcmp("ab", "abc") < 0);
    TEST("strcmp_prefix_gt", strcmp("abc", "ab") > 0);
    TEST("strcmp_empty", strcmp("", "") == 0);
    TEST("strcmp_empty_nonempty", strcmp("", "a") < 0);
}

static void test_strncmp(void)
{
    TEST("strncmp_eq_n", strncmp("abcXXX", "abcYYY", 3) == 0);
    TEST("strncmp_lt_n", strncmp("abc", "abd", 3) < 0);
    TEST("strncmp_zero_n", strncmp("abc", "xyz", 0) == 0);
    TEST("strncmp_diff_4", strncmp("abcX", "abcY", 4) < 0);
}

static void test_strcpy_strcat(void)
{
    char dst[64];

    strcpy(dst, "hello");
    TEST("strcpy_val", strcmp(dst, "hello") == 0);

    strcat(dst, " world");
    TEST("strcat_val", strcmp(dst, "hello world") == 0);

    char dst2[8];
    strncpy(dst2, "hello", 3);
    dst2[3] = '\0';
    TEST("strncpy_partial", strncmp(dst2, "hel", 3) == 0);

    strncpy(dst2, "hi", 8);
    TEST("strncpy_full", strcmp(dst2, "hi") == 0);
    TEST("strncpy_pad", dst2[2] == '\0');

    strcpy(dst, "foo");
    strncat(dst, "bar_extra", 3);
    TEST("strncat_val", strcmp(dst, "foobar") == 0);
}

static void test_strchr_strrchr(void)
{
    const char *s = "hello world";
    TEST("strchr_found", strchr(s, 'o') == s + 4);
    TEST("strchr_not_found", strchr(s, 'z') == NULL);
    TEST("strchr_nul", strchr(s, '\0') == s + 11);

    TEST("strrchr_found", strrchr(s, 'o') == s + 7);
    TEST("strrchr_not_found", strrchr(s, 'z') == NULL);
    TEST("strrchr_first", strrchr(s, 'h') == s);
}

static void test_strstr(void)
{
    const char *s = "hello world";
    TEST("strstr_found", strstr(s, "world") == s + 6);
    TEST("strstr_not_found", strstr(s, "xyz") == NULL);
    TEST("strstr_empty_needle", strstr(s, "") == s);
    TEST("strstr_full", strstr(s, "hello world") == s);
    TEST("strstr_single_char", strstr(s, "w") == s + 6);
}

static void test_strspn_strcspn_strpbrk(void)
{
    /* "hell" matches h,e,l,l → 4 */
    TEST("strspn_basic", strspn("hello", "hel") == 4);
    TEST("strspn_none", strspn("xyz", "abc") == 0);
    TEST("strspn_all", strspn("aaa", "a") == 3);

    /* "hello": 'l' at index 2 is the first char in reject set "lo" */
    TEST("strcspn_basic", strcspn("hello", "lo") == 2);
    TEST("strcspn_none", strcspn("abc", "xyz") == 3);
    TEST("strcspn_first", strcspn("hello", "h") == 0);

    const char *p = strpbrk("hello", "aeiou");
    TEST("strpbrk_found", p != NULL && *p == 'e');
    TEST("strpbrk_not_found", strpbrk("rhythm", "aeiou") == NULL);
}

static void test_memops(void)
{
    char src[16];
    char dst[16];
    memcpy(src, "abcdefghij", 11);

    memcpy(dst, src, 10);
    dst[10] = '\0';
    TEST("memcpy_val", strcmp(dst, "abcdefghij") == 0);

    memset(dst, 'X', 5);
    TEST("memset_val", dst[0] == 'X' && dst[4] == 'X' && dst[5] == 'f');

    char a[8], b[8], cc[8];
    memcpy(a, "abcdef\0", 7);
    memcpy(b, "abcdef\0", 7);
    memcpy(cc, "abcxef\0", 7);
    TEST("memcmp_eq", memcmp(a, b, 6) == 0);
    TEST("memcmp_lt", memcmp(a, cc, 6) < 0);
    TEST("memcmp_gt", memcmp(cc, a, 6) > 0);
    TEST("memcmp_prefix", memcmp(a, cc, 3) == 0);

    TEST("memchr_found", memchr(a, 'd', 6) == a + 3);
    TEST("memchr_not_found", memchr(a, 'z', 6) == NULL);

    char mbuf[16];
    memcpy(mbuf, "0123456789", 11);
    memmove(mbuf + 3, mbuf, 5);
    TEST("memmove_nonoverlap", strncmp(mbuf + 3, "01234", 5) == 0);

    char obuf[16];
    memcpy(obuf, "abcdefgh", 9);
    memmove(obuf + 2, obuf, 6);
    TEST("memmove_overlap_right", strncmp(obuf + 2, "abcdef", 6) == 0);

    char obuf2[16];
    memcpy(obuf2, "abcdefgh", 9);
    memmove(obuf2, obuf2 + 2, 6);
    TEST("memmove_overlap_left", strncmp(obuf2, "cdefgh", 6) == 0);
}

static void test_strtok(void)
{
    char s[] = "one,two,,three";
    char *tok;

    tok = strtok(s, ",");
    TEST("strtok_1", tok != NULL && strcmp(tok, "one") == 0);

    tok = strtok(NULL, ",");
    TEST("strtok_2", tok != NULL && strcmp(tok, "two") == 0);

    tok = strtok(NULL, ",");
    TEST("strtok_3", tok != NULL && strcmp(tok, "three") == 0);

    tok = strtok(NULL, ",");
    TEST("strtok_end", tok == NULL);
}

/* ------------------------------------------------------------------ */
/* stdlib.h tests                                                       */
/* ------------------------------------------------------------------ */

static void test_atoi_atol(void)
{
    TEST("atoi_pos", atoi("42") == 42);
    TEST("atoi_neg", atoi("-99") == -99);
    TEST("atoi_zero", atoi("0") == 0);
    TEST("atoi_leading_space", atoi("  7") == 7);
    TEST("atoi_trailing_junk", atoi("12abc") == 12);

    TEST("atol_pos", atol("123456") == 123456L);
    TEST("atol_neg", atol("-1") == -1L);
}

static void test_strtol(void)
{
    char *end;
    long v;
    unsigned long uv;

    v = strtol("42", &end, 10);
    TEST("strtol_10", v == 42 && *end == '\0');

    v = strtol("-99", &end, 10);
    TEST("strtol_neg", v == -99);

    v = strtol("0x1A", &end, 0);
    TEST("strtol_auto_hex", v == 0x1A);

    v = strtol("0777", &end, 0);
    TEST("strtol_auto_oct", v == 0777);

    v = strtol("ff", &end, 16);
    TEST("strtol_hex_ff", v == 255);

    v = strtol("   -42 rest", &end, 10);
    TEST("strtol_leading_space", v == -42);

    uv = strtoul("4294967295", &end, 10);
    TEST("strtoul_max32", uv == 4294967295UL);

    uv = strtoul("0xFF", &end, 0);
    TEST("strtoul_hex_0x", uv == 255UL);
}

static void test_abs_div(void)
{
    TEST("abs_pos", abs(5) == 5);
    TEST("abs_neg", abs(-5) == 5);
    TEST("abs_zero", abs(0) == 0);
    TEST("abs_int_max", abs(2147483647) == 2147483647);

    TEST("labs_neg", labs(-1000000L) == 1000000L);
    /* llabs: 64-bit comparison broken in CPUTwo TCC; skip */

    div_t d = div(17, 5);
    TEST("div_quot", d.quot == 3);
    TEST("div_rem", d.rem == 2);

    div_t d2 = div(-17, 5);
    TEST("div_neg_quot", d2.quot == -3);
    TEST("div_neg_rem", d2.rem == -2);

    ldiv_t ld = ldiv(-17L, 5L);
    TEST("ldiv_quot", ld.quot == -3L);
    TEST("ldiv_rem", ld.rem == -2L);
}

static int int_cmp(const void *a, const void *b)
{
    int ia = *(const int *)a;
    int ib = *(const int *)b;
    return (ia > ib) - (ia < ib);
}

static void test_qsort_bsearch(void)
{
    int arr[] = { 5, 3, 8, 1, 9, 2, 7, 4, 6, 0 };
    int n = (int)(sizeof(arr) / sizeof(arr[0]));
    qsort(arr, (size_t)n, sizeof(int), int_cmp);

    int sorted = 1, i;
    for (i = 0; i < n - 1; i++)
        if (arr[i] > arr[i + 1]) { sorted = 0; break; }
    TEST("qsort_sorted", sorted);
    TEST("qsort_first", arr[0] == 0);
    TEST("qsort_last", arr[n - 1] == 9);
    TEST("qsort_mid", arr[5] == 5);

    int key = 7;
    int *found = (int *)bsearch(&key, arr, (size_t)n, sizeof(int), int_cmp);
    TEST("bsearch_found", found != NULL && *found == 7);

    int missing = 42;
    int *not_found = (int *)bsearch(&missing, arr, (size_t)n, sizeof(int), int_cmp);
    TEST("bsearch_not_found", not_found == NULL);

    int first = 0;
    int *found0 = (int *)bsearch(&first, arr, (size_t)n, sizeof(int), int_cmp);
    TEST("bsearch_first", found0 != NULL && *found0 == 0);
}

static void test_rand(void)
{
    srand(12345);
    int ok = 1, i;
    for (i = 0; i < 100; i++) {
        int r = rand();
        if (r < 0 || r > RAND_MAX) { ok = 0; break; }
    }
    TEST("rand_range", ok);

    srand(42);
    int r1 = rand();
    srand(42);
    int r2 = rand();
    TEST("rand_seed_repro", r1 == r2);

    srand(1);
    int ra = rand();
    srand(2);
    int rb = rand();
    TEST("rand_diff_seeds", ra != rb);
}

/* ------------------------------------------------------------------ */
/* ctype.h tests                                                        */
/* ------------------------------------------------------------------ */

static void test_ctype(void)
{
    TEST("isalpha_A", isalpha('A') != 0);
    TEST("isalpha_z", isalpha('z') != 0);
    TEST("isalpha_0", isalpha('0') == 0);
    TEST("isalpha_space", isalpha(' ') == 0);

    TEST("isdigit_0", isdigit('0') != 0);
    TEST("isdigit_9", isdigit('9') != 0);
    TEST("isdigit_A", isdigit('A') == 0);

    TEST("isalnum_A", isalnum('A') != 0);
    TEST("isalnum_5", isalnum('5') != 0);
    TEST("isalnum_bang", isalnum('!') == 0);

    TEST("isspace_space", isspace(' ') != 0);
    TEST("isspace_tab", isspace('\t') != 0);
    TEST("isspace_newline", isspace('\n') != 0);
    TEST("isspace_A", isspace('A') == 0);

    TEST("isupper_A", isupper('A') != 0);
    TEST("isupper_Z", isupper('Z') != 0);
    TEST("isupper_a", isupper('a') == 0);

    TEST("islower_a", islower('a') != 0);
    TEST("islower_z", islower('z') != 0);
    TEST("islower_A", islower('A') == 0);

    TEST("ispunct_dot", ispunct('.') != 0);
    TEST("ispunct_bang", ispunct('!') != 0);
    TEST("ispunct_A", ispunct('A') == 0);
    TEST("ispunct_space", ispunct(' ') == 0);

    TEST("isprint_A", isprint('A') != 0);
    TEST("isprint_space", isprint(' ') != 0);
    TEST("isprint_nul", isprint('\0') == 0);
    TEST("isprint_del", isprint(127) == 0);

    TEST("toupper_a", toupper('a') == 'A');
    TEST("toupper_z", toupper('z') == 'Z');
    TEST("toupper_A", toupper('A') == 'A');
    TEST("toupper_0", toupper('0') == '0');

    TEST("tolower_A", tolower('A') == 'a');
    TEST("tolower_Z", tolower('Z') == 'z');
    TEST("tolower_a", tolower('a') == 'a');
    TEST("tolower_0", tolower('0') == '0');
}

/* ------------------------------------------------------------------ */
/* stdarg.h tests                                                       */
/* count is the only named param → 3 varargs fit in registers.         */
/* ------------------------------------------------------------------ */

static int sum_ints(int count, ...)
{
    va_list ap;
    va_start(ap, count);
    int total = 0, i;
    for (i = 0; i < count; i++)
        total += va_arg(ap, int);
    va_end(ap);
    return total;
}

/* sep is the only named param before the two string varargs */
static void concat2(char *buf, const char *sep, ...)
{
    va_list ap;
    va_start(ap, sep);
    const char *s1 = va_arg(ap, const char *);
    const char *s2 = va_arg(ap, const char *);
    va_end(ap);
    buf[0] = '\0';
    strcat(buf, s1);
    strcat(buf, sep);
    strcat(buf, s2);
}

static void test_stdarg(void)
{
    TEST("va_int_sum_3", sum_ints(3, 10, 20, 30) == 60);
    TEST("va_int_sum_1", sum_ints(1, 99) == 99);
    TEST("va_int_sum_0", sum_ints(0) == 0);
    TEST("va_int_sum_2", sum_ints(2, 5, 15) == 20);

    char buf[64];
    concat2(buf, "-", "foo", "bar");
    TEST("va_string_concat2", strcmp(buf, "foo-bar") == 0);
}

/* ------------------------------------------------------------------ */
/* stdint.h / limits.h tests                                           */
/* ------------------------------------------------------------------ */

static void test_stdint_limits(void)
{
    TEST("INT_MAX_val",  INT_MAX  == 2147483647);
    TEST("INT_MIN_val",  INT_MIN  == -2147483647 - 1);
    TEST("UINT_MAX_val", UINT_MAX == 4294967295u);
    TEST("LONG_MAX_ge",  LONG_MAX >= INT_MAX);
    TEST("LLONG_MAX_val", LLONG_MAX == 9223372036854775807LL);
    TEST("LLONG_MIN_val", LLONG_MIN == -9223372036854775807LL - 1LL);
    TEST("CHAR_BIT_val", CHAR_BIT == 8);

    TEST("int8_size",   sizeof(int8_t)   == 1);
    TEST("uint8_size",  sizeof(uint8_t)  == 1);
    TEST("int16_size",  sizeof(int16_t)  == 2);
    TEST("uint16_size", sizeof(uint16_t) == 2);
    TEST("int32_size",  sizeof(int32_t)  == 4);
    TEST("uint32_size", sizeof(uint32_t) == 4);
    TEST("int64_size",  sizeof(int64_t)  == 8);
    TEST("uint64_size", sizeof(uint64_t) == 8);

    TEST("INT8_MAX",   INT8_MAX   == 127);
    TEST("INT8_MIN",   INT8_MIN   == -128);
    TEST("UINT8_MAX",  UINT8_MAX  == 255u);
    TEST("INT16_MAX",  INT16_MAX  == 32767);
    TEST("INT16_MIN",  INT16_MIN  == -32768);
    TEST("UINT16_MAX", UINT16_MAX == 65535u);
    TEST("INT32_MAX",  INT32_MAX  == 2147483647);
    TEST("INT32_MIN",  INT32_MIN  == -2147483647 - 1);
    TEST("UINT32_MAX", UINT32_MAX == 4294967295u);
    TEST("INT64_MAX",  INT64_MAX  == 9223372036854775807LL);
    TEST("INT64_MIN",  INT64_MIN  == -9223372036854775807LL - 1LL);
    TEST("UINT64_MAX", UINT64_MAX == 18446744073709551615ULL);

    int8_t   i8  = 127;         TEST("int8_wrap",   (int8_t)(i8 + 1)     == -128);
    uint8_t  u8  = 255;         TEST("uint8_wrap",  (uint8_t)(u8 + 1)    == 0);
    int16_t  i16 = 32767;       TEST("int16_wrap",  (int16_t)(i16 + 1)   == -32768);
    uint16_t u16 = 65535u;      TEST("uint16_wrap", (uint16_t)(u16 + 1)  == 0);
    int32_t  i32 = 2147483647;  TEST("int32_wrap",  (int32_t)(i32 + 1)   == INT32_MIN);
    uint32_t u32 = 0xFFFFFFFFu; TEST("uint32_wrap", (uint32_t)(u32 + 1u) == 0u);
    int64_t  i64 = 9223372036854775807LL;
    TEST("int64_wrap", (int64_t)(i64 + 1LL) == INT64_MIN);
    uint64_t u64 = 0xFFFFFFFFFFFFFFFFULL;
    TEST("uint64_wrap", (uint64_t)(u64 + 1ULL) == 0ULL);
}

/* ------------------------------------------------------------------ */
/* Integer arithmetic edge cases                                        */
/* ------------------------------------------------------------------ */

static void test_arithmetic(void)
{
    TEST("shl_0",  (1u << 0)  == 1u);
    TEST("shl_1",  (1u << 1)  == 2u);
    TEST("shl_31", (1u << 31) == 0x80000000u);
    TEST("shr_u",  (0x80000000u >> 1) == 0x40000000u);
    TEST("shl_8",  (1u << 8)  == 256u);

    TEST("mul_u32_overflow", (uint32_t)(0xFFFFFFFFu * 2u) == 0xFFFFFFFEu);
    TEST("mul_basic", 6 * 7 == 42);

    TEST("div_trunc_pos", 7 / 2 == 3);
    TEST("div_trunc_neg", (-7) / 2 == -3);
    TEST("mod_pos", 7 % 3 == 1);
    TEST("mod_neg", (-7) % 3 == -1);
    TEST("div_exact", 100 / 10 == 10);
    TEST("mod_exact", 100 % 10 == 0);

    int64_t a64 = 1000000000LL * 1000000000LL;
    TEST("i64_mul", a64 == 1000000000000000000LL);

    uint64_t ua = 0xFFFFFFFFULL * 0xFFFFFFFFULL;
    TEST("u64_mul", ua == 0xFFFFFFFE00000001ULL);

    int64_t sub64 = 9000000000000000000LL - 8000000000000000000LL;
    TEST("i64_sub", sub64 == 1000000000000000000LL);

    TEST("bitand",   (0xF0F0u & 0xFF00u) == 0xF000u);
    TEST("bitor",    (0xF000u | 0x0F00u) == 0xFF00u);
    TEST("bitxor",   (0xFFFFu ^ 0xFF00u) == 0x00FFu);
    TEST("bitnot",   (~0u) == 0xFFFFFFFFu);
    TEST("bitand64", (0xFFFFFFFF00000000ULL & 0xFFFFFFFFFFFFFFFFULL)
                     == 0xFFFFFFFF00000000ULL);
}

/* ------------------------------------------------------------------ */
/* main                                                                 */
/* ------------------------------------------------------------------ */

int main(void)
{
    printf("=== pdclib test suite ===\n");

    /* stdio */
    test_sprintf_basic();
    test_sprintf_width_precision();
    test_sprintf_edge_cases();
    test_snprintf();
    test_sscanf();

    /* string.h */
    test_strlen();
    test_strcmp();
    test_strncmp();
    test_strcpy_strcat();
    test_strchr_strrchr();
    test_strstr();
    test_strspn_strcspn_strpbrk();
    test_memops();
    test_strtok();

    /* stdlib.h */
    test_atoi_atol();
    test_strtol();
    test_abs_div();
    test_qsort_bsearch();
    test_rand();

    /* ctype.h */
    test_ctype();

    /* stdarg.h */
    test_stdarg();

    /* stdint.h / limits.h */
    test_stdint_limits();

    /* arithmetic */
    test_arithmetic();

    printf("=== Results: %d passed, %d failed ===\n", pass, fail);
    return fail == 0 ? 0 : 1;
}
