/*
 * uffs_blocks_test.c — standalone smoke test for the uffs_blocks storage
 * layer. No FUSE needed. Compiles the same logic by #defining a stub for
 * the one FUSE call we use (fuse_get_context).
 *
 * Build:
 *   cc -Wall -O2 -D_FILE_OFFSET_BITS=64 -DUFFS_BLOCKS_NO_FUSE uffs_blocks_test.c -o uffs_blocks_test
 * Run:
 *   ./uffs_blocks_test /tmp/uffs_test.img
 */

/* Stub out FUSE so we can compile just the storage half. */
#define FUSE_USE_VERSION 31

/* Minimal stand-in for the few FUSE symbols uffs_blocks.c touches. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <stdint.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/statvfs.h>
#include <assert.h>

/* Constants and types lifted verbatim from uffs_blocks.c so this file
 * is self-contained. If you change the layout there, update here too —
 * or better, factor the storage layer into its own .h. */
#define UFFS_BLOCK_SIZE   4096u
#define UFFS_MAGIC        0x55464253u
#define UFFS_VERSION      2
#define UFFS_TOTAL_BLOCKS  4096u
#define UFFS_MAX_INODES    512u
#define UFFS_DIRECT        12u
#define UFFS_INDIRECT_PTRS (UFFS_BLOCK_SIZE / sizeof(uint32_t))
#define UFFS_MAX_NAME      59
#define UFFS_DIRENT_SIZE   64
#define UFFS_DIRENTS_PER_BLOCK (UFFS_BLOCK_SIZE / UFFS_DIRENT_SIZE)
#define UFFS_MAX_FILESZ \
    ((uint64_t)(UFFS_DIRECT + UFFS_INDIRECT_PTRS) * UFFS_BLOCK_SIZE)
enum { T_REG=0, T_DIR=1, T_LNK=2 };

typedef struct {
    uint32_t magic, version, block_size, total_blocks;
    uint32_t bitmap_start, bitmap_blocks;
    uint32_t inode_table_start, inode_table_blocks, inode_count;
    uint32_t data_start, root_inode;
    uint32_t _pad[5];
} uffs_super_t;

typedef struct {
    uint32_t in_use, type, mode, uid, gid, nlink;
    uint64_t size;
    int64_t  atime, mtime, ctime;
    uint32_t direct[UFFS_DIRECT];
    uint32_t indirect;
    uint32_t _pad[3];
} uffs_inode_t;

typedef struct {
    uint32_t ino;
    char     name[UFFS_MAX_NAME + 1];
} uffs_dirent_t;

_Static_assert(sizeof(uffs_dirent_t) == UFFS_DIRENT_SIZE, "dirent 64");

static int           backing_fd = -1;
static void         *backing_map;
static size_t        backing_size;
static uffs_super_t *sb;
static uint8_t      *bitmap;
static uffs_inode_t *itab;
static char         *blocks_base;

static void *block_ptr(uint32_t b) { return blocks_base + (size_t)b * UFFS_BLOCK_SIZE; }
static int  bit_get(uint32_t b) { return (bitmap[b >> 3] >> (b & 7)) & 1; }
static void bit_set(uint32_t b) { bitmap[b >> 3] |=  (1u << (b & 7)); }
static void bit_clr(uint32_t b) { bitmap[b >> 3] &= ~(1u << (b & 7)); }

static int alloc_block(void) {
    for (uint32_t b = sb->data_start; b < sb->total_blocks; b++)
        if (!bit_get(b)) { bit_set(b); memset(block_ptr(b), 0, UFFS_BLOCK_SIZE); return b; }
    return -ENOSPC;
}
static void free_block(uint32_t b) {
    if (b < sb->data_start || b >= sb->total_blocks) return;
    bit_clr(b);
}

static int alloc_inode(void) {
    for (uint32_t i = 1; i < sb->inode_count; i++)
        if (!itab[i].in_use) {
            memset(&itab[i], 0, sizeof(itab[i]));
            itab[i].in_use = 1;
            itab[i].atime = itab[i].mtime = itab[i].ctime = time(NULL);
            return i;
        }
    return -ENOSPC;
}

static int inode_bmap(uffs_inode_t *n, uint32_t lbn, int allocate) {
    if (lbn < UFFS_DIRECT) {
        if (n->direct[lbn] == 0 && allocate) {
            int b = alloc_block(); if (b < 0) return b;
            n->direct[lbn] = b;
        }
        return n->direct[lbn];
    }
    uint32_t idx = lbn - UFFS_DIRECT;
    if (idx >= UFFS_INDIRECT_PTRS) return -EFBIG;
    if (n->indirect == 0) {
        if (!allocate) return 0;
        int b = alloc_block(); if (b < 0) return b;
        n->indirect = b;
    }
    uint32_t *ind = block_ptr(n->indirect);
    if (ind[idx] == 0 && allocate) {
        int b = alloc_block(); if (b < 0) return b;
        ind[idx] = b;
    }
    return ind[idx];
}

static int file_write(uffs_inode_t *n, const char *buf, size_t size, off_t off) {
    if ((uint64_t)off + size > UFFS_MAX_FILESZ) return -EFBIG;
    size_t done = 0;
    while (done < size) {
        uint32_t lbn = (off + done) / UFFS_BLOCK_SIZE;
        uint32_t bo  = (off + done) % UFFS_BLOCK_SIZE;
        size_t   chunk = UFFS_BLOCK_SIZE - bo;
        if (chunk > size - done) chunk = size - done;
        int b = inode_bmap(n, lbn, 1);
        if (b < 0) return b;
        memcpy((char *)block_ptr(b) + bo, buf + done, chunk);
        done += chunk;
    }
    if ((uint64_t)off + size > n->size) n->size = off + size;
    return size;
}

static int file_read(uffs_inode_t *n, char *buf, size_t size, off_t off) {
    if ((uint64_t)off >= n->size) return 0;
    if ((uint64_t)off + size > n->size) size = n->size - off;
    size_t done = 0;
    while (done < size) {
        uint32_t lbn = (off + done) / UFFS_BLOCK_SIZE;
        uint32_t bo  = (off + done) % UFFS_BLOCK_SIZE;
        size_t   chunk = UFFS_BLOCK_SIZE - bo;
        if (chunk > size - done) chunk = size - done;
        int b = inode_bmap(n, lbn, 0);
        if (b > 0) memcpy(buf + done, (char *)block_ptr(b) + bo, chunk);
        else       memset(buf + done, 0, chunk);
        done += chunk;
    }
    return size;
}

static int format_fresh(void) {
    uint32_t bitmap_bytes  = (UFFS_TOTAL_BLOCKS + 7) / 8;
    uint32_t bitmap_blocks = (bitmap_bytes + UFFS_BLOCK_SIZE - 1) / UFFS_BLOCK_SIZE;
    uint32_t itab_bytes    = UFFS_MAX_INODES * sizeof(uffs_inode_t);
    uint32_t itab_blocks   = (itab_bytes + UFFS_BLOCK_SIZE - 1) / UFFS_BLOCK_SIZE;
    uint32_t data_start    = 1 + bitmap_blocks + itab_blocks;

    memset(backing_map, 0, backing_size);
    sb = backing_map;
    sb->magic = UFFS_MAGIC; sb->version = UFFS_VERSION;
    sb->block_size = UFFS_BLOCK_SIZE; sb->total_blocks = UFFS_TOTAL_BLOCKS;
    sb->bitmap_start = 1; sb->bitmap_blocks = bitmap_blocks;
    sb->inode_table_start = 1 + bitmap_blocks;
    sb->inode_table_blocks = itab_blocks;
    sb->inode_count = UFFS_MAX_INODES;
    sb->data_start = data_start; sb->root_inode = 1;

    bitmap = (uint8_t *)block_ptr(sb->bitmap_start);
    itab   = (uffs_inode_t *)block_ptr(sb->inode_table_start);
    for (uint32_t b = 0; b < data_start; b++) bit_set(b);

    itab[1].in_use = 1; itab[1].type = T_DIR; itab[1].mode = 0755; itab[1].nlink = 2;
    return 0;
}

static int open_backing(const char *path) {
    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return -1;
    size_t need = (size_t)UFFS_TOTAL_BLOCKS * UFFS_BLOCK_SIZE;
    if (ftruncate(fd, need) < 0) return -1;
    void *m = mmap(NULL, need, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (m == MAP_FAILED) return -1;
    backing_fd = fd; backing_map = m; backing_size = need; blocks_base = m;
    return format_fresh();
}

/* ---- tests ---------------------------------------------------------- */

#define CHECK(expr) do { if (!(expr)) { \
    fprintf(stderr, "FAIL: %s (%s:%d)\n", #expr, __FILE__, __LINE__); \
    exit(1); } } while (0)

static void test_layout(void) {
    CHECK(sb->magic == UFFS_MAGIC);
    CHECK(sb->data_start > sb->inode_table_start);
    CHECK(sb->total_blocks == UFFS_TOTAL_BLOCKS);
    /* every metadata block must be marked used */
    for (uint32_t b = 0; b < sb->data_start; b++) CHECK(bit_get(b));
    /* the first data block must be free */
    CHECK(!bit_get(sb->data_start));
    printf("  layout: data_start=%u, %u data blocks free\n",
           sb->data_start, sb->total_blocks - sb->data_start);
}

static void test_alloc_free(void) {
    int b1 = alloc_block(); CHECK(b1 > 0);
    int b2 = alloc_block(); CHECK(b2 > 0 && b2 != b1);
    free_block(b1);
    int b3 = alloc_block(); CHECK(b3 == b1);   /* should reuse first free */
    free_block(b2); free_block(b3);
    printf("  alloc/free: ok\n");
}

static void test_small_file(void) {
    int ino = alloc_inode(); CHECK(ino > 0);
    itab[ino].type = T_REG; itab[ino].nlink = 1;
    const char *msg = "hello world";
    CHECK(file_write(&itab[ino], msg, strlen(msg), 0) == (int)strlen(msg));
    char buf[64] = {0};
    CHECK(file_read(&itab[ino], buf, sizeof(buf), 0) == (int)strlen(msg));
    CHECK(strcmp(buf, msg) == 0);
    CHECK(itab[ino].direct[0] != 0);
    CHECK(itab[ino].indirect == 0);
    printf("  small file: ok (uses direct block %u)\n", itab[ino].direct[0]);
}

static void test_large_file(void) {
    int ino = alloc_inode(); CHECK(ino > 0);
    itab[ino].type = T_REG; itab[ino].nlink = 1;

    /* Write 13 blocks worth — forces 12 direct + 1 indirect. */
    size_t n = 13 * UFFS_BLOCK_SIZE;
    char *src = malloc(n);
    for (size_t i = 0; i < n; i++) src[i] = (char)(i * 31 + 7);
    CHECK(file_write(&itab[ino], src, n, 0) == (int)n);
    CHECK(itab[ino].indirect != 0);

    char *dst = malloc(n);
    CHECK(file_read(&itab[ino], dst, n, 0) == (int)n);
    CHECK(memcmp(src, dst, n) == 0);
    free(src); free(dst);
    printf("  large file (13 blocks): ok (indirect=%u)\n", itab[ino].indirect);
}

static void test_sparse_hole(void) {
    int ino = alloc_inode(); CHECK(ino > 0);
    itab[ino].type = T_REG; itab[ino].nlink = 1;
    /* Write at offset 8 KiB — leaves blocks 0 and 1 unallocated. */
    const char *msg = "after the hole";
    CHECK(file_write(&itab[ino], msg, strlen(msg), 8192) == (int)strlen(msg));
    CHECK(itab[ino].direct[0] == 0);
    CHECK(itab[ino].direct[1] == 0);
    CHECK(itab[ino].direct[2] != 0);
    /* Read across the hole; first 8 KiB should be zeros. */
    char buf[16];
    CHECK(file_read(&itab[ino], buf, sizeof(buf), 0) == 16);
    for (int i = 0; i < 16; i++) CHECK(buf[i] == 0);
    CHECK(file_read(&itab[ino], buf, strlen(msg), 8192) == (int)strlen(msg));
    CHECK(memcmp(buf, msg, strlen(msg)) == 0);
    printf("  sparse hole: ok (block 0,1 unallocated, reads as zero)\n");
}

static void test_max_file(void) {
    int ino = alloc_inode(); CHECK(ino > 0);
    itab[ino].type = T_REG; itab[ino].nlink = 1;
    /* Try to write past max — should refuse. */
    char one = 'x';
    int rc = file_write(&itab[ino], &one, 1, UFFS_MAX_FILESZ);
    CHECK(rc == -EFBIG);
    /* Write right at the boundary should succeed. */
    rc = file_write(&itab[ino], &one, 1, UFFS_MAX_FILESZ - 1);
    CHECK(rc == 1);
    printf("  max filesize boundary: ok (max=%llu bytes)\n",
           (unsigned long long)UFFS_MAX_FILESZ);
}

int main(int argc, char *argv[]) {
    const char *path = argc > 1 ? argv[1] : "/tmp/uffs_test.img";
    printf("uffs_blocks storage-layer smoke test\n");
    printf("image: %s\n", path);
    CHECK(open_backing(path) == 0);
    test_layout();
    test_alloc_free();
    test_small_file();
    test_large_file();
    test_sparse_hole();
    test_max_file();
    msync(backing_map, backing_size, MS_SYNC);
    close(backing_fd);
    unlink(path);
    printf("all tests passed.\n");
    return 0;
}
