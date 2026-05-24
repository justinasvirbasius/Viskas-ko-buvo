/*
 * uffs_journal_test.c — verify that the journal actually recovers state
 * after a simulated crash.
 *
 * Strategy: open a fresh image, write a transaction, then DON'T let
 * the checkpoint phase complete — we corrupt the target block back to
 * its old value (simulating "crash before target msync"), close, reopen,
 * and check that replay restored the correct contents.
 *
 * Build:
 *   cc -Wall -O2 -D_FILE_OFFSET_BITS=64 uffs_journal_test.c -o uffs_journal_test
 * Run:
 *   ./uffs_journal_test /tmp/uffs_journal_test.img
 */
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
#include <assert.h>

/* --- Layout constants copied from uffs_journal.c --- */
#define UFFS_BLOCK_SIZE   4096u
#define UFFS_MAGIC        0x55464A4Cu
#define UFFS_VERSION      3
#define UFFS_TOTAL_BLOCKS    4096u
#define UFFS_JOURNAL_BLOCKS  64u
#define UFFS_MAX_INODES      512u
#define J_MAGIC_DESCRIPTOR  0x44455343u
#define J_MAGIC_COMMIT      0x434F4D4Du
#define J_MAX_BLOCKS_PER_TXN  16

typedef struct {
    uint32_t magic, version, block_size, total_blocks;
    uint32_t bitmap_start, bitmap_blocks;
    uint32_t inode_table_start, inode_table_blocks, inode_count;
    uint32_t journal_start, journal_blocks;
    uint32_t journal_head, journal_tail;
    uint64_t journal_seq;
    uint32_t data_start, root_inode;
    uint32_t _pad[1];
} uffs_super_t;

typedef struct {
    uint32_t magic;
    uint64_t seq;
    uint32_t n_blocks;
    uint32_t targets[J_MAX_BLOCKS_PER_TXN];
} j_descriptor_t;

typedef struct { uint32_t magic; uint64_t seq; } j_commit_t;

/* --- Globals --- */
static int   fd;
static void *map;
static size_t mapsz;
static uffs_super_t *sb;
static char *blocks;

static void *block_ptr(uint32_t b) { return blocks + (size_t)b * UFFS_BLOCK_SIZE; }
static uint32_t j_block(uint32_t i) { return sb->journal_start + (i % sb->journal_blocks); }

static void sync_all(void) { msync(map, mapsz, MS_SYNC); }
static void sync_block(uint32_t b) { msync(block_ptr(b), UFFS_BLOCK_SIZE, MS_SYNC); }

#define CHECK(e) do { if (!(e)) { fprintf(stderr,"FAIL: %s @ %d\n",#e,__LINE__); exit(1);} } while(0)

/* --- Minimal format / open --- */

static void format_image(const char *path) {
    fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    CHECK(fd >= 0);
    mapsz = (size_t)UFFS_TOTAL_BLOCKS * UFFS_BLOCK_SIZE;
    CHECK(ftruncate(fd, mapsz) == 0);
    map = mmap(NULL, mapsz, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    CHECK(map != MAP_FAILED);
    blocks = map;
    memset(map, 0, mapsz);
    sb = map;
    sb->magic = UFFS_MAGIC; sb->version = UFFS_VERSION;
    sb->block_size = UFFS_BLOCK_SIZE; sb->total_blocks = UFFS_TOTAL_BLOCKS;
    sb->bitmap_start = 1;        sb->bitmap_blocks = 1;
    sb->inode_table_start = 2;   sb->inode_table_blocks = 15;
    sb->inode_count = UFFS_MAX_INODES;
    sb->journal_start = 17;      sb->journal_blocks = UFFS_JOURNAL_BLOCKS;
    sb->journal_head = sb->journal_tail = 0; sb->journal_seq = 0;
    sb->data_start = 17 + UFFS_JOURNAL_BLOCKS;
    sb->root_inode = 1;
    sync_all();
}

static void reopen_image(const char *path) {
    if (map) { munmap(map, mapsz); close(fd); }
    fd = open(path, O_RDWR, 0644);
    CHECK(fd >= 0);
    struct stat st; fstat(fd, &st);
    mapsz = st.st_size;
    map = mmap(NULL, mapsz, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    CHECK(map != MAP_FAILED);
    blocks = map;
    sb = map;
}

/* --- Write a transaction to the journal, optionally without
 *     applying it to the target (simulating "crashed before checkpoint"). */
static void write_txn(uint32_t *targets, char **after_images, int n,
                      int apply_to_target) {
    uint64_t seq = ++sb->journal_seq;
    uint32_t pos = sb->journal_tail;

    j_descriptor_t *desc = block_ptr(j_block(pos));
    memset(desc, 0, UFFS_BLOCK_SIZE);
    desc->magic = J_MAGIC_DESCRIPTOR; desc->seq = seq; desc->n_blocks = n;
    for (int i = 0; i < n; i++) desc->targets[i] = targets[i];
    pos++;

    for (int i = 0; i < n; i++) {
        memcpy(block_ptr(j_block(pos)), after_images[i], UFFS_BLOCK_SIZE);
        pos++;
    }
    for (uint32_t p = sb->journal_tail; p < pos; p++) sync_block(j_block(p));

    j_commit_t *cm = block_ptr(j_block(pos));
    memset(cm, 0, UFFS_BLOCK_SIZE);
    cm->magic = J_MAGIC_COMMIT; cm->seq = seq;
    sync_block(j_block(pos));
    pos++;

    sb->journal_tail = pos % sb->journal_blocks;
    sync_all();

    if (apply_to_target) {
        for (int i = 0; i < n; i++) {
            memcpy(block_ptr(targets[i]), after_images[i], UFFS_BLOCK_SIZE);
            sync_block(targets[i]);
        }
        sb->journal_head = sb->journal_tail;
        sync_all();
    }
    /* else: simulating crash. Journal is committed; target hasn't caught up. */
}

/* Apply replay logic — same algorithm as in uffs_journal.c */
static int try_replay(uint32_t *cursor) {
    j_descriptor_t *d = block_ptr(j_block(*cursor));
    if (d->magic != J_MAGIC_DESCRIPTOR) return -1;
    if (d->n_blocks == 0 || d->n_blocks > J_MAX_BLOCKS_PER_TXN) return -1;
    uint32_t commit_pos = *cursor + 1 + d->n_blocks;
    j_commit_t *c = block_ptr(j_block(commit_pos));
    if (c->magic != J_MAGIC_COMMIT || c->seq != d->seq) return -1;
    for (uint32_t i = 0; i < d->n_blocks; i++) {
        memcpy(block_ptr(d->targets[i]),
               block_ptr(j_block(*cursor + 1 + i)),
               UFFS_BLOCK_SIZE);
        sync_block(d->targets[i]);
    }
    *cursor = commit_pos + 1;
    return 0;
}
static int journal_replay(void) {
    uint32_t cur = sb->journal_head;
    int n = 0;
    while (cur != sb->journal_tail) {
        if (try_replay(&cur) < 0) break;
        n++;
    }
    sb->journal_head = sb->journal_tail = 0;
    sync_all();
    return n;
}

/* --- Tests --- */

static void test_clean_commit(const char *path) {
    format_image(path);
    /* Write something to inode block 2, with apply_to_target=1 (clean shutdown). */
    char *after = malloc(UFFS_BLOCK_SIZE);
    memset(after, 0xAB, UFFS_BLOCK_SIZE);
    uint32_t target = 2;   /* first inode block */
    write_txn(&target, &after, 1, 1);
    free(after);

    /* Reopen. Journal should be empty; target should be 0xAB. */
    reopen_image(path);
    CHECK(sb->journal_head == sb->journal_tail);
    char *t = block_ptr(2);
    for (int i = 0; i < UFFS_BLOCK_SIZE; i++) CHECK((unsigned char)t[i] == 0xAB);
    int replayed = journal_replay();
    CHECK(replayed == 0);
    printf("  clean shutdown: ok (no replay needed)\n");
}

static void test_crash_recovery(const char *path) {
    format_image(path);
    char *after = malloc(UFFS_BLOCK_SIZE);
    memset(after, 0xCD, UFFS_BLOCK_SIZE);
    uint32_t target = 3;
    /* Simulate crash: write journal but NOT the target. */
    write_txn(&target, &after, 1, 0);
    free(after);

    /* Verify the simulated-crash state: journal has the txn, target is still 0. */
    char *t = block_ptr(3);
    for (int i = 0; i < UFFS_BLOCK_SIZE; i++) CHECK(t[i] == 0);
    CHECK(sb->journal_head != sb->journal_tail);

    /* Now reopen and replay. */
    reopen_image(path);
    int replayed = journal_replay();
    CHECK(replayed == 1);
    t = block_ptr(3);
    for (int i = 0; i < UFFS_BLOCK_SIZE; i++) CHECK((unsigned char)t[i] == 0xCD);
    printf("  crash recovery: ok (1 txn replayed, target restored)\n");
}

static void test_torn_commit(const char *path) {
    format_image(path);
    /* Write a descriptor + data but NO commit block. Replay must skip it. */
    uint64_t seq = ++sb->journal_seq;
    uint32_t pos = sb->journal_tail;
    j_descriptor_t *d = block_ptr(j_block(pos));
    memset(d, 0, UFFS_BLOCK_SIZE);
    d->magic = J_MAGIC_DESCRIPTOR; d->seq = seq; d->n_blocks = 1;
    d->targets[0] = 4;
    char *data = block_ptr(j_block(pos + 1));
    memset(data, 0xEE, UFFS_BLOCK_SIZE);
    /* deliberately do NOT write commit block */
    sb->journal_tail = (pos + 3) % sb->journal_blocks;   /* claim 3 slots */
    sync_all();

    reopen_image(path);
    int replayed = journal_replay();
    CHECK(replayed == 0);   /* must NOT replay an uncommitted txn */
    char *t = block_ptr(4);
    for (int i = 0; i < UFFS_BLOCK_SIZE; i++) CHECK(t[i] == 0);
    printf("  torn commit: ok (uncommitted txn ignored)\n");
}

static void test_multiple_txns(const char *path) {
    format_image(path);
    /* Three transactions, then "crash" before checkpoint of the third. */
    char *a1 = malloc(UFFS_BLOCK_SIZE); memset(a1, 0x11, UFFS_BLOCK_SIZE);
    char *a2 = malloc(UFFS_BLOCK_SIZE); memset(a2, 0x22, UFFS_BLOCK_SIZE);
    char *a3 = malloc(UFFS_BLOCK_SIZE); memset(a3, 0x33, UFFS_BLOCK_SIZE);
    uint32_t t1 = 5, t2 = 6, t3 = 7;
    write_txn(&t1, &a1, 1, 1);   /* clean */
    write_txn(&t2, &a2, 1, 1);   /* clean */
    write_txn(&t3, &a3, 1, 0);   /* crash */
    free(a1); free(a2); free(a3);

    reopen_image(path);
    int n = journal_replay();
    CHECK(n == 1);   /* only the uncommitted-checkpoint one needs replay */
    CHECK(((unsigned char *)block_ptr(5))[0] == 0x11);
    CHECK(((unsigned char *)block_ptr(6))[0] == 0x22);
    CHECK(((unsigned char *)block_ptr(7))[0] == 0x33);
    printf("  multi-txn: ok (1 of 3 replayed)\n");
}

int main(int argc, char *argv[]) {
    const char *path = argc > 1 ? argv[1] : "/tmp/uffs_journal_test.img";
    printf("uffs_journal recovery test\n");
    printf("image: %s\n", path);
    test_clean_commit(path);
    test_crash_recovery(path);
    test_torn_commit(path);
    test_multiple_txns(path);
    if (map) munmap(map, mapsz);
    if (fd >= 0) close(fd);
    unlink(path);
    printf("all tests passed.\n");
    return 0;
}
