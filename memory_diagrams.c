/* ============================================================================
 *  memory_diagrams.c   —   A complete field guide to memory in C
 *  ----------------------------------------------------------------------------
 *  Sixteen demonstrations, each pairing real C code with an ASCII picture of
 *  what actually sits in RAM.  Compile and run on any POSIX-ish x86-64 system:
 *
 *      cc -std=c11 -Wall -Wextra -O0 -g memory_diagrams.c -o memory_diagrams
 *      ./memory_diagrams
 *
 *  Assumes a 64-bit machine:  int = 4 B,  long = 8 B,  pointer = 8 B,
 *  little-endian byte order.  Real addresses vary per run (ASLR).
 *
 *  Sections:
 *      I    static array of int
 *      II   pointer arithmetic
 *      III  array name vs pointer variable
 *      IV   strings and '\0'
 *      V    string literals in .rodata
 *      VI   2-D arrays (row-major layout)
 *      VII  array of pointers (jagged strings)
 *      VIII structs and padding
 *      IX   unions and type punning
 *      X    bitfields
 *      XI   malloc / calloc / free
 *      XII  realloc lifecycle
 *      XIII function pointers
 *      XIV  void* and generic pointers
 *      XV   stack frames during a call chain
 *      XVI  the process address space
 * ==========================================================================*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <stdint.h>
#include <inttypes.h>

/* ---- globals used by section XVI ---------------------------------------- */
int        g_init   = 42;          /* .data    — initialised global         */
int        g_zero;                 /* .bss     — uninitialised global       */
static int s_count  = 0;           /* .bss     — explicit zero folds in     */
const char *g_name  = "alice";     /* pointer in .data, "alice" in .rodata  */

/* ---- pretty banners ----------------------------------------------------- */
static void banner(const char *roman, const char *title) {
    printf("\n");
    printf("============================================================\n");
    printf(" PLATE %s   %s\n", roman, title);
    printf("============================================================\n");
}

static void rule(void) {
    printf("------------------------------------------------------------\n");
}

/* ========================================================================
 *  PLATE I   —   A static array of int
 * ====================================================================== */
static void plate_I(void) {
    banner("I", "A static array of int");

    int arr[5] = { 10, 20, 30, 40, 50 };

    printf("declaration : int arr[5] = { 10, 20, 30, 40, 50 };\n");
    printf("sizeof(arr) : %zu bytes  (5 ints * 4 B each)\n", sizeof arr);
    printf("length      : %zu\n", sizeof arr / sizeof arr[0]);
    rule();

    printf("Memory layout — five contiguous 4-byte slots:\n\n");
    printf("  addr           +0      +4      +8     +12     +16\n");
    printf("  %-12p +-------+-------+-------+-------+-------+\n", (void*)&arr[0]);
    printf("               |  %3d  |  %3d  |  %3d  |  %3d  |  %3d  |\n",
           arr[0], arr[1], arr[2], arr[3], arr[4]);
    printf("               +-------+-------+-------+-------+-------+\n");
    printf("                arr[0]  arr[1]  arr[2]  arr[3]  arr[4]\n\n");

    for (int i = 0; i < 5; i++)
        printf("  &arr[%d] = %p   (offset +%d)\n",
               i, (void*)&arr[i], i * (int)sizeof(int));

    rule();
    printf("Note: 'arr' decays to &arr[0] in almost every expression.\n");
    printf("      The two exceptions are sizeof(arr) and &arr.\n");
}

/* ========================================================================
 *  PLATE II  —   Pointer arithmetic
 * ====================================================================== */
static void plate_II(void) {
    banner("II", "Pointer arithmetic — distances in elements, not bytes");

    int arr[5] = { 10, 20, 30, 40, 50 };
    int *p = arr;

    printf("  *(p + 2)         = %d\n", *(p + 2));
    printf("  p[3]             = %d   (same as *(p+3))\n", p[3]);
    printf("  3[p]             = %d   (yes, this is legal C)\n", 3[p]);

    ptrdiff_t d = &arr[4] - &arr[1];
    printf("  &arr[4] - &arr[1] = %td elements (NOT 12 bytes)\n", d);

    rule();
    printf("Step size: adding 1 to an int* advances by sizeof(int) = 4 bytes.\n\n");
    printf("  +-------+-------+-------+-------+-------+ . (one past end)\n");
    printf("  |  10   |  20   |  30   |  40   |  50   |\n");
    printf("  +-------+-------+-------+-------+-------+\n");
    printf("    ^p      ^p+1    ^p+2    ^p+3    ^p+4    ^p+5  (legal value,\n");
    printf("    arr     +4 B    +8 B    +12 B   +16 B           never deref!)\n");
}

/* ========================================================================
 *  PLATE III —   Array name vs pointer variable
 * ====================================================================== */
static void plate_III(void) {
    banner("III", "Array name vs pointer variable");

    int  arr[3] = { 7, 8, 9 };
    int *ptr    = arr;

    printf("  sizeof(arr) = %zu   (12 — three int values)\n", sizeof arr);
    printf("  sizeof(ptr) = %zu   (8  — one address)\n",     sizeof ptr);

    rule();
    printf("Two different objects in memory:\n\n");
    printf("  STACK %p +-------+-------+-------+\n", (void*)arr);
    printf("                  |   7   |   8   |   9   |   <-- arr (12 B)\n");
    printf("                  +-------+-------+-------+\n");
    printf("                   arr[0]  arr[1]  arr[2]\n\n");
    printf("  STACK %p +---------------------+\n", (void*)&ptr);
    printf("                  |  %-18p |   <-- ptr (8 B address)\n", (void*)ptr);
    printf("                  +---------------------+\n");
    printf("                            |\n");
    printf("                            +---> arr[0]\n");

    ptr++;          /* legal — ptr is a variable */
    /* arr++;  <-- ILLEGAL: array name is not an lvalue */
    printf("\n  After ptr++:  ptr = %p   (advanced by 4 bytes)\n", (void*)ptr);
}

/* ========================================================================
 *  PLATE IV  —   Strings: char arrays terminated by '\0'
 * ====================================================================== */
static void plate_IV(void) {
    banner("IV", "Strings — char arrays with a '\\0' terminator");

    char msg[8] = "HELLO";          /* 5 chars + '\0' + 2 unused */

    printf("  declaration : char msg[8] = \"HELLO\";\n");
    printf("  sizeof(msg) : %zu   (the whole array)\n", sizeof msg);
    printf("  strlen(msg) : %zu   (up to but not including '\\0')\n", strlen(msg));

    rule();
    printf("Byte layout (ASCII hex shown below each cell):\n\n");
    printf("  +-----+-----+-----+-----+-----+-----+-----+-----+\n");
    printf("  | 'H' | 'E' | 'L' | 'L' | 'O' |\\0   |  ?  |  ?  |\n");
    printf("  +-----+-----+-----+-----+-----+-----+-----+-----+\n");
    printf("   0x48  0x45  0x4C  0x4C  0x4F  0x00  unin  unin\n");
    printf("   [0]   [1]   [2]   [3]   [4]   [5]   [6]   [7]\n\n");

    printf("Actual bytes from your binary:\n   ");
    for (size_t i = 0; i < sizeof msg; i++)
        printf("%02x ", (unsigned char)msg[i]);
    printf("\n");

    rule();
    printf("Three declarations, three storage stories:\n");
    printf("  char s[]  = \"hi\";     stack copy, 3 B, writable\n");
    printf("  char s[8] = \"hi\";     stack copy, 8 B (rest zeroed), writable\n");
    printf("  char *s   = \"hi\";     ptr to .rodata — writing SEGFAULTS\n");
}

/* ========================================================================
 *  PLATE V   —   String literals in .rodata
 * ====================================================================== */
static void plate_V(void) {
    banner("V", "String literals live in read-only memory");

    char  a[] = "hello";    /* stack copy — writable             */
    char *b   = "hello";    /* pointer to literal in .rodata     */
    char *c   = "hello";    /* often the same address as b       */

    a[0] = 'J';             /* fine: modifies the stack copy     */
    /* b[0] = 'J';  <-- SIGSEGV at runtime */

    printf("  &a (stack)       = %p\n", (void*)a);
    printf("  b  (-> .rodata)  = %p\n", (void*)b);
    printf("  c  (-> .rodata)  = %p\n", (void*)c);

    if (b == c)
        printf("  b == c  : compiler folded the duplicate literal\n");
    else
        printf("  b != c  : separate copies in .rodata\n");

    rule();
    printf("Two different layouts for the same letters:\n\n");
    printf("  STACK  +-----+-----+-----+-----+-----+-----+\n");
    printf("         | 'J' | 'e' | 'l' | 'l' | 'o' |\\0   |   a[] — writable\n");
    printf("         +-----+-----+-----+-----+-----+-----+\n\n");
    printf("  .rodata +-----+-----+-----+-----+-----+-----+\n");
    printf("          | 'h' | 'e' | 'l' | 'l' | 'o' |\\0   |   shared literal\n");
    printf("          +-----+-----+-----+-----+-----+-----+\n");
    printf("              ^                                   read-only mapping\n");
    printf("              |\n");
    printf("              +--- b and c both point here\n");
}

/* ========================================================================
 *  PLATE VI  —   2D arrays in row-major layout
 * ====================================================================== */
static void plate_VI(void) {
    banner("VI", "2D arrays — row-major in one flat line");

    int m[3][4] = {
        { 1,  2,  3,  4 },
        { 5,  6,  7,  8 },
        { 9, 10, 11, 12 },
    };

    printf("  sizeof(m)    = %zu   (3*4*4)\n", sizeof m);
    printf("  sizeof(m[0]) = %zu   (one row)\n", sizeof m[0]);
    printf("  m[1][2]      = %d    (address formula: base + (r*4+c)*4)\n",
           m[1][2]);

    rule();
    printf("Layout — three rows packed end to end (one int array of 12):\n\n");
    printf("  row 0:  +----+----+----+----+\n");
    printf("          |  1 |  2 |  3 |  4 |\n");
    printf("          +----+----+----+----+\n");
    printf("  row 1:  +----+----+----+----+\n");
    printf("          |  5 |  6 | *7*|  8 |   <-- m[1][2] at byte offset 24\n");
    printf("          +----+----+----+----+\n");
    printf("  row 2:  +----+----+----+----+\n");
    printf("          |  9 | 10 | 11 | 12 |\n");
    printf("          +----+----+----+----+\n\n");

    printf("Verifying contiguous storage by walking as a flat 1-D array:\n   ");
    int *flat = &m[0][0];
    for (int i = 0; i < 12; i++) printf("%d ", flat[i]);
    printf("\n");

    rule();
    printf("Cache rule: for r { for c } walks linearly, friendly to the\n");
    printf("64-byte cache line.  Swapping the loops strides by 16 bytes\n");
    printf("each step and can be 5-10x slower on big matrices.\n");
}

/* ========================================================================
 *  PLATE VII —   Array of pointers (jagged strings)
 * ====================================================================== */
static void plate_VII(void) {
    banner("VII", "Array of pointers — the jagged alternative");

    const char *names[3] = { "Mon", "Tuesday", "Wed" };

    printf("Each element is an 8-byte pointer; strings sized to fit:\n\n");
    for (int i = 0; i < 3; i++)
        printf("  names[%d] = %p  --> \"%s\"  (%zu bytes incl. '\\0')\n",
               i, (const void*)names[i], names[i], strlen(names[i]) + 1);

    rule();
    printf("  names[] +-----------+-----------+-----------+\n");
    printf("          |   ptr ->  |   ptr ->  |   ptr ->  |   3 * 8 B = 24 B\n");
    printf("          +-----+-----+-----+-----+-----+-----+\n");
    printf("                |           |           |\n");
    printf("                v           v           v\n");
    printf("  .rodata:  'M''o''n'\\0  'T''u''e''s''d''a''y'\\0  'W''e''d'\\0\n");
    printf("            \\---4 B---/  \\---------8 B---------/  \\--4 B--/\n");
    printf("\n");
    printf("Total char data: 16 bytes; total pointer table: 24 bytes.\n");
    printf("This is exactly the shape of int main(int argc, char *argv[]).\n");
}

/* ========================================================================
 *  PLATE VIII —  Structs and padding
 * ====================================================================== */
struct Bad {
    char  c;        /* offset  0, then 3 bytes pad */
    int   n;        /* offset  4                   */
    char  d;        /* offset  8, then 7 bytes pad */
    long  big;      /* offset 16                   */
};                  /* sizeof = 24 */

struct Good {
    long  big;      /* offset  0  */
    int   n;        /* offset  8  */
    char  c;        /* offset 12  */
    char  d;        /* offset 13, then 2 bytes tail pad */
};                  /* sizeof = 16 */

static void plate_VIII(void) {
    banner("VIII", "Structs and padding — alignment costs bytes");

    printf("  sizeof(struct Bad)  = %zu\n", sizeof(struct Bad));
    printf("  sizeof(struct Good) = %zu\n", sizeof(struct Good));

    printf("\n  offsetof in Bad :  c=%zu  n=%zu  d=%zu  big=%zu\n",
        offsetof(struct Bad, c),  offsetof(struct Bad, n),
        offsetof(struct Bad, d),  offsetof(struct Bad, big));

    printf("  offsetof in Good:  big=%zu  n=%zu  c=%zu  d=%zu\n",
        offsetof(struct Good, big), offsetof(struct Good, n),
        offsetof(struct Good, c),   offsetof(struct Good, d));

    rule();
    printf("struct Bad layout — 24 bytes (11 of them wasted):\n\n");
    printf("  offset  0     1     2     3     4   5   6   7\n");
    printf("        +-----+-----+-----+-----+-------------------+\n");
    printf("        |  c  | pad | pad | pad |   n  (int, 4 B)   |\n");
    printf("        +-----+-----+-----+-----+-------------------+\n");
    printf("  offset  8     9    10    11    12   13   14   15\n");
    printf("        +-----+-----+-----+-----+-----+-----+-----+-----+\n");
    printf("        |  d  | pad | pad | pad | pad | pad | pad | pad |\n");
    printf("        +-----+-----+-----+-----+-----+-----+-----+-----+\n");
    printf("  offset  16 ............................... 23\n");
    printf("        +---------------------------------------+\n");
    printf("        |          big (long, 8 B)              |\n");
    printf("        +---------------------------------------+\n");

    rule();
    printf("struct Good — same fields, reordered, 16 bytes (only 2 wasted):\n\n");
    printf("  offset  0 ........................ 7\n");
    printf("        +-------------------------------+\n");
    printf("        |       big (long, 8 B)         |\n");
    printf("        +-------------------------------+\n");
    printf("  offset  8 .......... 11   12    13    14    15\n");
    printf("        +---------------+-----+-----+-----+-----+\n");
    printf("        |  n (int, 4 B) |  c  |  d  | pad | pad |\n");
    printf("        +---------------+-----+-----+-----+-----+\n");

    printf("\nRule of thumb: order members from largest alignment to smallest.\n");
}

/* ========================================================================
 *  PLATE IX —   Unions and type punning
 * ====================================================================== */
union Convert {
    uint32_t u;
    float    f;
    uint8_t  bytes[4];
};

static void plate_IX(void) {
    banner("IX", "Unions — all members share the same storage");

    union Convert v;
    v.f = 3.14f;

    printf("  Stored as float : %f\n", v.f);
    printf("  Same bytes as u : 0x%08x   (%u)\n", v.u, v.u);
    printf("  Byte by byte    : ");
    for (int i = 0; i < 4; i++) printf("0x%02x ", v.bytes[i]);
    printf("  (little-endian: low byte first)\n");

    rule();
    printf("All three views point at THE SAME 4 bytes:\n\n");
    printf("        +-----+-----+-----+-----+\n");
    printf("  bytes |0x%02x |0x%02x |0x%02x |0x%02x |\n",
           v.bytes[0], v.bytes[1], v.bytes[2], v.bytes[3]);
    printf("        +-----+-----+-----+-----+\n");
    printf("  u     | <----- uint32_t ----> |  = 0x%08x\n", v.u);
    printf("  f     | <-----  float  -----> |  = %f\n", v.f);
    printf("\n  sizeof(union Convert) = %zu  (size of the LARGEST member)\n",
           sizeof(union Convert));

    rule();
    printf("Use cases: tagged variants, hardware register layouts, and\n");
    printf("inspecting the bit pattern of floats (legal in C, not in C++).\n");
}

/* ========================================================================
 *  PLATE X   —   Bitfields
 * ====================================================================== */
struct Flags {
    unsigned ready    : 1;     /* 1 bit  */
    unsigned error    : 1;     /* 1 bit  */
    unsigned mode     : 3;     /* 3 bits, values 0..7 */
    unsigned priority : 4;     /* 4 bits, values 0..15 */
    unsigned reserved : 23;    /* round out to 32 bits */
};

static void plate_X(void) {
    banner("X", "Bitfields — packing multiple values into one word");

    struct Flags f = { 0 };
    f.ready    = 1;
    f.error    = 0;
    f.mode     = 5;        /* binary 101 */
    f.priority = 12;       /* binary 1100 */

    printf("  sizeof(struct Flags) = %zu   (one 4-byte word)\n", sizeof f);
    printf("  ready=%u  error=%u  mode=%u  priority=%u\n",
           f.ready, f.error, f.mode, f.priority);

    rule();
    printf("32 bits in one word, allocated low to high (typical x86 layout):\n\n");
    printf("  bit  31 ............................. 8  7 6 5 4  3 2 1  0\n");
    printf("      +---------------------------------+--------+------+-+-+\n");
    printf("      |         reserved (23 bits)      |priority| mode |E|R|\n");
    printf("      +---------------------------------+--------+------+-+-+\n");
    printf("                                          1100     101   0 1\n");
    printf("                                          = 12     = 5\n\n");
    printf("  Without bitfields you'd burn an int per flag — 16 bytes vs 4.\n");
    printf("  Caveat: bit ordering and padding are implementation-defined,\n");
    printf("  so bitfields are NOT portable across compilers for wire formats.\n");
}

/* ========================================================================
 *  PLATE XI  —   malloc / calloc / free
 * ====================================================================== */
static void plate_XI(void) {
    banner("XI", "Dynamic arrays — malloc, calloc, free");

    int *a = malloc(4 * sizeof *a);
    if (!a) { perror("malloc"); return; }
    for (int i = 0; i < 4; i++) a[i] = (i + 1) * 100;

    int *b = calloc(4, sizeof *b);     /* zero-initialised */
    if (!b) { free(a); perror("calloc"); return; }

    printf("  &a (stack)  = %p     pointer variable, 8 B\n", (void*)&a);
    printf("   a           = %p   --> heap block (malloc)\n", (void*)a);
    printf("  &b (stack)  = %p     pointer variable, 8 B\n", (void*)&b);
    printf("   b           = %p   --> heap block (calloc)\n", (void*)b);

    rule();
    printf("Stack pointer  ===>  heap block:\n\n");
    printf("  STACK  +------------------+\n");
    printf("         |  a = %p  |   <-- pointer variable\n", (void*)a);
    printf("         +------------------+\n");
    printf("                  |\n");
    printf("                  v\n");
    printf("  HEAP   +-------+-------+-------+-------+\n");
    printf("         |  %3d  |  %3d  |  %3d  |  %3d  |   malloc:  uninit\n",
           a[0], a[1], a[2], a[3]);
    printf("         +-------+-------+-------+-------+   then we wrote it\n\n");
    printf("  HEAP   +-------+-------+-------+-------+\n");
    printf("         |  %3d  |  %3d  |  %3d  |  %3d  |   calloc:  zeroed\n",
           b[0], b[1], b[2], b[3]);
    printf("         +-------+-------+-------+-------+\n");

    free(a);  a = NULL;     /* hygiene: avoid a dangling pointer */
    free(b);  b = NULL;

    rule();
    printf("Four classic bugs to remember (all caught by -fsanitize=address):\n");
    printf("  * memory leak       : forgot to free\n");
    printf("  * double free       : free called twice on same pointer\n");
    printf("  * use-after-free    : read/write through pointer after free\n");
    printf("  * heap overflow     : wrote past the allocated block\n");
}

/* ========================================================================
 *  PLATE XII —   realloc lifecycle
 * ====================================================================== */
static void plate_XII(void) {
    banner("XII", "realloc — growing a buffer step by step");

    size_t cap = 4, len = 0;
    int *buf = malloc(cap * sizeof *buf);
    if (!buf) { perror("malloc"); return; }

    void *first_addr = buf;
    printf("  Step 1: malloc(%zu)  buf @ %p   cap=%zu len=%zu\n",
           cap * sizeof *buf, (void*)buf, cap, len);

    int prev_cap = 0;
    for (int v = 1; v <= 10; v++) {
        if (len == cap) {
            size_t new_cap = cap * 2;
            uintptr_t old_addr = (uintptr_t)buf;  /* save as integer */
            int *tmp = realloc(buf, new_cap * sizeof *buf);
            if (!tmp) { free(buf); perror("realloc"); return; }
            buf = tmp;
            prev_cap = cap;
            cap = new_cap;
            printf("           realloc -> %zu B  buf @ %p   %s\n",
                   cap * sizeof *buf, (void*)buf,
                   (old_addr == (uintptr_t)buf)
                       ? "(same address: grown in place)"
                       : "(MOVED: copied, old freed)");
            (void)prev_cap;
        }
        buf[len++] = v;
    }

    rule();
    printf("Final buffer  (len = %zu, cap = %zu):\n  [ ", len, cap);
    for (size_t i = 0; i < len; i++) printf("%d ", buf[i]);
    printf("]\n");
    printf("  started at %p, ended at %p\n", first_addr, (void*)buf);

    free(buf);

    rule();
    printf("ANTI-PATTERN to memorise:  buf = realloc(buf, n);\n");
    printf("If realloc returns NULL you've just stomped the only pointer to\n");
    printf("the original block -> instant leak.  Always go through a temp.\n");
}

/* ========================================================================
 *  PLATE XIII —  Function pointers
 * ====================================================================== */
static int add_op(int a, int b) { return a + b; }
static int sub_op(int a, int b) { return a - b; }
static int mul_op(int a, int b) { return a * b; }

static void plate_XIII(void) {
    banner("XIII", "Function pointers — code addresses in a table");

    /* a table of function pointers */
    int (*ops[3])(int, int) = { add_op, sub_op, mul_op };
    const char *labels[3]   = { "add",  "sub",  "mul"  };

    printf("Dispatch table (each slot is an 8-byte code address):\n\n");
    printf("  ops[] +-------------+-------------+-------------+\n");
    printf("        | %-11p | %-11p | %-11p |\n",
           (void*)ops[0], (void*)ops[1], (void*)ops[2]);
    printf("        +-------------+-------------+-------------+\n");
    printf("           |              |              |\n");
    printf("           v              v              v\n");
    printf("        .text:        .text:        .text:\n");
    printf("        add_op()      sub_op()      mul_op()\n\n");

    for (int i = 0; i < 3; i++)
        printf("  ops[%d](6, 4) = %2d    (\"%s\")\n",
               i, ops[i](6, 4), labels[i]);

    rule();
    printf("Function pointers live in CODE memory (.text), which is mapped\n");
    printf("read+execute.  This is the mechanism behind C++ vtables, qsort's\n");
    printf("comparator, libc callbacks, and every plugin system.\n");
}

/* ========================================================================
 *  PLATE XIV —   void* and generic pointers
 * ====================================================================== */
static void plate_XIV(void) {
    banner("XIV", "void* — a generic pointer with no element size");

    int    i  = 0xDEADBEEF;
    double d  = 2.71828;
    char   s[] = "hi";

    void *vp;
    vp = &i; printf("  &i as void*: %p  (back as int*    -> %#x)\n",
                    vp, *(int*)vp);
    vp = &d; printf("  &d as void*: %p  (back as double* -> %f)\n",
                    vp, *(double*)vp);
    vp = s;  printf("   s as void*: %p  (back as char*   -> \"%s\")\n",
                    vp, (char*)vp);

    rule();
    printf("Rules for void*:\n");
    printf("  * It holds any object address (and may point at code on some\n");
    printf("    platforms, though that's technically implementation-defined).\n");
    printf("  * You CANNOT dereference it directly: *vp is illegal.\n");
    printf("  * You CANNOT do arithmetic on it: vp + 1 is illegal in ISO C\n");
    printf("    (GCC permits it as an extension, treating it like char*).\n");
    printf("  * Convert to a typed pointer before use:  (int*)vp\n\n");
    printf("This is the type used by malloc, free, memcpy, qsort comparators.\n");
    printf("It's how C achieves \"generic programming\" without templates.\n");
}

/* ========================================================================
 *  PLATE XV  —   Stack frames during a call chain
 * ====================================================================== */
static int   g_indent = 0;
static void  show_indent(void) {
    for (int i = 0; i < g_indent; i++) printf("    ");
}

static int leaf(int a, int b) {
    int sum = a + b;
    show_indent(); printf("|-> leaf(a=%d, b=%d)    &a=%p  &sum=%p\n",
                          a, b, (void*)&a, (void*)&sum);
    show_indent(); printf("    return %d\n", sum);
    return sum;
}

static int middle(int x) {
    int doubled;
    show_indent(); printf("|-> middle(x=%d)         &x=%p\n",
                          x, (void*)&x);
    g_indent++;
    doubled = leaf(x, x);
    g_indent--;
    show_indent(); printf("    return %d  (frame for middle goes away)\n",
                          doubled);
    return doubled;
}

static void plate_XV(void) {
    banner("XV", "Stack frames during a function call chain");

    int result;
    int  x_in_plate = 21;
    printf("  &x_in_plate (this frame) = %p\n", (void*)&x_in_plate);
    rule();
    printf("Trace — note how each callee's local addresses are LOWER\n");
    printf("(stack grows downward on x86-64):\n\n");

    g_indent = 0;
    result = middle(x_in_plate);

    rule();
    printf("Final result : %d\n", result);
    printf("\n");
    printf("ASCII snapshot at the deepest point (inside leaf):\n\n");
    printf("           +----------------------------+  HIGH addr\n");
    printf("           |  main / plate_XV frame     |\n");
    printf("           |    x_in_plate = 21         |\n");
    printf("           |    result     = ???        |\n");
    printf("           +----------------------------+\n");
    printf("                       v\n");
    printf("           +----------------------------+\n");
    printf("           |  middle frame              |\n");
    printf("           |    x       = 21            |\n");
    printf("           |    doubled = ???           |\n");
    printf("           +----------------------------+\n");
    printf("                       v\n");
    printf("           +----------------------------+  *** ACTIVE ***\n");
    printf("           |  leaf frame                |\n");
    printf("           |    a   = 21                |\n");
    printf("           |    b   = 21                |\n");
    printf("           |    sum = 42                |\n");
    printf("           +----------------------------+  LOW addr\n\n");

    printf("On return, each frame is popped.  Locals cease to exist.\n");
    printf("THIS is why  int *bad() { int x=42; return &x; }  is broken.\n");
}

/* ========================================================================
 *  PLATE XVI —   The process address space
 * ====================================================================== */
static void plate_XVI(void) {
    banner("XVI", "The process address space");

    int  x_local = 7;
    static int once = 0;            /* .bss */
    int *heap_ptr = malloc(16);

    printf("Real addresses from this run (values vary per execution due to ASLR):\n\n");
    printf("  .text     : main()         %p   (code, read+execute)\n", (void*)&plate_XVI);
    printf("  .rodata   : \"alice\"        %p   (read-only literal)\n", (void*)g_name);
    printf("  .data     : g_init = 42    %p   (init global)\n",       (void*)&g_init);
    printf("  .data     : g_name (ptr)   %p\n",                       (void*)&g_name);
    printf("  .bss      : g_zero         %p   (zero-init global)\n",  (void*)&g_zero);
    printf("  .bss      : s_count        %p   (static, zero)\n",      (void*)&s_count);
    printf("  .bss      : once (static)  %p\n",                       (void*)&once);
    printf("  HEAP      : malloc(16)     %p   (manual lifetime)\n",   (void*)heap_ptr);
    printf("  STACK     : x_local        %p   (auto lifetime)\n",     (void*)&x_local);
    printf("  STACK     : heap_ptr       %p   (the pointer itself)\n",(void*)&heap_ptr);

    rule();
    printf("Address-space map (typical x86-64 Linux process):\n\n");
    printf("  HIGH  0x7fff........  +-------------------------+\n");
    printf("                        |  STACK  (grows down)    |   locals, args, ret addrs\n");
    printf("                        +-------------------------+\n");
    printf("                        |                         |\n");
    printf("                        |       unmapped gap      |\n");
    printf("                        |                         |\n");
    printf("                        +-------------------------+\n");
    printf("                        |  HEAP   (grows up)      |   malloc / calloc / realloc\n");
    printf("                        +-------------------------+\n");
    printf("                        |  .bss                   |   uninit globals & statics\n");
    printf("                        +-------------------------+\n");
    printf("                        |  .data                  |   init globals & statics\n");
    printf("                        +-------------------------+\n");
    printf("                        |  .rodata                |   string literals, const\n");
    printf("                        +-------------------------+\n");
    printf("                        |  .text                  |   machine code\n");
    printf("  LOW   0x400000        +-------------------------+\n");

    free(heap_ptr);

    rule();
    printf("Where each declaration site lives:\n");
    printf("  global, initialised        -> .data        (writable, persists)\n");
    printf("  global, uninit or = 0      -> .bss         (writable, zeroed)\n");
    printf("  const global / literal     -> .rodata      (read-only)\n");
    printf("  local variable             -> stack        (frame lifetime)\n");
    printf("  static local               -> .data / .bss (program lifetime)\n");
    printf("  malloc / calloc / realloc  -> heap         (until free)\n");
}

/* ========================================================================
 *  main — drive every plate in order
 * ====================================================================== */
int main(void) {
    printf("============================================================\n");
    printf("  C MEMORY & ARRAYS  —  A complete field guide\n");
    printf("  sixteen plates, each pairing code with a memory picture\n");
    printf("  built for x86-64: int=4B  long=8B  ptr=8B  little-endian\n");
    printf("============================================================\n");

    plate_I();
    plate_II();
    plate_III();
    plate_IV();
    plate_V();
    plate_VI();
    plate_VII();
    plate_VIII();
    plate_IX();
    plate_X();
    plate_XI();
    plate_XII();
    plate_XIII();
    plate_XIV();
    plate_XV();
    plate_XVI();

    printf("\n============================================================\n");
    printf("  end of field guide   —   compile with:\n");
    printf("    cc -std=c11 -Wall -Wextra -O0 -g memory_diagrams.c\n");
    printf("============================================================\n");
    return 0;
}
