/*
 * uffs_journal.c — block-allocated filesystem with metadata journaling.
 *
 * On-disk layout (4 KiB blocks):
 *   block 0                        : superblock
 *   blocks 1..B                    : block-allocation bitmap
 *   blocks B+1..I                  : inode table
 *   blocks I+1..J                  : journal region (UFFS_JOURNAL_BLOCKS)
 *   blocks J+1..END                : data blocks
 *
 * Journal format. The region is a circular ring of blocks. Each
 * transaction occupies a contiguous run:
 *   [ descriptor block ]
 *   [ data block #1 ]   (after-image of target block #1)
 *   [ data block #2 ]   (after-image of target block #2)
 *   ...
 *   [ commit block ]
 *
 * A transaction is durable only once the commit block has been written
 * AND fsynced. On mount, we scan from sb->journal_head, replay every
 * txn whose commit block is valid, then reset head=tail to a clean state.
 *
 * What's journaled: inode-table writes, bitmap writes, directory-block
 * writes. File data blocks are NOT journaled (data=ordered mode):
 * a crash during a file write may leave the file with truncated
 * contents but the filesystem metadata stays consistent.
 *
 * Build:
 *   cc -Wall -O2 -D_FILE_OFFSET_BITS=64 uffs_journal.c \
 *      `pkg-config fuse3 --cflags --libs` -o uffs_journal
 *
 * Run:
 *   ./uffs_journal -f -o backing=/tmp/uffs.img /tmp/uffs
 *
 * Caveats: single-threaded, no checksums on journal blocks (you can fake
 * a commit by writing garbage with the right magic — fine for a teaching
 * artifact, not fine for production). Replay is not idempotent if a
 * crash happens DURING replay; real systems double-buffer.
 */
#define FUSE_USE_VERSION 31
#define _GNU_SOURCE

#include <fuse.h>
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

#define UFFS_BLOCK_SIZE   4096u
#define UFFS_MAGIC        0x55464A4Cu   /* 'UFJL' — journaled variant */
#define UFFS_VERSION      3

#define UFFS_TOTAL_BLOCKS    4096u
#define UFFS_JOURNAL_BLOCKS  64u
#define UFFS_MAX_INODES      512u
#define UFFS_DIRECT          12u
#define UFFS_INDIRECT_PTRS   (UFFS_BLOCK_SIZE / sizeof(uint32_t))

#define UFFS_MAX_NAME      59
#define UFFS_DIRENT_SIZE   64
#define UFFS_DIRENTS_PER_BLOCK (UFFS_BLOCK_SIZE / UFFS_DIRENT_SIZE)

#define UFFS_MAX_FILESZ \
    ((uint64_t)(UFFS_DIRECT + UFFS_INDIRECT_PTRS) * UFFS_BLOCK_SIZE)

/* Journal-block magic values. */
#define J_MAGIC_DESCRIPTOR  0x44455343u   /* 'DESC' */
#define J_MAGIC_COMMIT      0x434F4D4Du   /* 'COMM' */

#define J_MAX_BLOCKS_PER_TXN  16          /* descriptor capacity */

enum { T_REG=0, T_DIR=1, T_LNK=2 };

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t block_size;
    uint32_t total_blocks;
    uint32_t bitmap_start;
    uint32_t bitmap_blocks;
    uint32_t inode_table_start;
    uint32_t inode_table_blocks;
    uint32_t inode_count;
    uint32_t journal_start;
    uint32_t journal_blocks;
    uint32_t journal_head;       /* next block to read for replay */
    uint32_t journal_tail;       /* next block to write */
    uint64_t journal_seq;        /* monotonic transaction id */
    uint32_t data_start;
    uint32_t root_inode;
    uint32_t _pad[1];
} uffs_super_t;

typedef struct {
    uint32_t in_use;
    uint32_t type;
    uint32_t mode;
    uint32_t uid;
    uint32_t gid;
    uint32_t nlink;
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

/* Journal blocks ---------------------------------------------------- */
typedef struct {
    uint32_t magic;              /* J_MAGIC_DESCRIPTOR */
    uint64_t seq;                /* matches commit block */
    uint32_t n_blocks;           /* how many data blocks follow */
    uint32_t targets[J_MAX_BLOCKS_PER_TXN];  /* where each one lands */
} j_descriptor_t;

typedef struct {
    uint32_t magic;              /* J_MAGIC_COMMIT */
    uint64_t seq;
} j_commit_t;

_Static_assert(sizeof(uffs_dirent_t) == UFFS_DIRENT_SIZE, "dirent 64");
_Static_assert(sizeof(j_descriptor_t) <= UFFS_BLOCK_SIZE, "descriptor fits");
_Static_assert(sizeof(j_commit_t) <= UFFS_BLOCK_SIZE, "commit fits");

/* ---- in-memory state ------------------------------------------------ */

static int           backing_fd = -1;
static void         *backing_map;
static size_t        backing_size;
static uffs_super_t *sb;
static uint8_t      *bitmap;
static uffs_inode_t *itab;
static char         *blocks_base;

/* ---- block helpers -------------------------------------------------- */

static void *block_ptr(uint32_t b) { return blocks_base + (size_t)b * UFFS_BLOCK_SIZE; }

static void sync_block_sync(uint32_t b) {
    /* Synchronous flush — critical for journal commits. */
    msync(block_ptr(b), UFFS_BLOCK_SIZE, MS_SYNC);
}

static void sync_range_sync(void *p, size_t len) {
    long ps = sysconf(_SC_PAGESIZE);
    uintptr_t start = (uintptr_t)p & ~(ps - 1);
    uintptr_t end   = ((uintptr_t)p + len + ps - 1) & ~(ps - 1);
    msync((void *)start, end - start, MS_SYNC);
}

static int  bit_get(uint32_t b) { return (bitmap[b >> 3] >> (b & 7)) & 1; }
static void bit_set(uint32_t b) { bitmap[b >> 3] |=  (1u << (b & 7)); }
static void bit_clr(uint32_t b) { bitmap[b >> 3] &= ~(1u << (b & 7)); }

/* ====================================================================== */
/*  Journal                                                                */
/* ====================================================================== */

/* A transaction is staged in memory, then flushed as one atomic unit. */
typedef struct {
    int      active;
    int      n_blocks;
    uint32_t targets[J_MAX_BLOCKS_PER_TXN];
} txn_t;

static txn_t g_txn;

static uint32_t j_block(uint32_t i) {
    return sb->journal_start + (i % sb->journal_blocks);
}

/* Begin a fresh transaction. */
static void txn_begin(void) {
    if (g_txn.active) {
        fprintf(stderr, "uffs_journal: BUG: nested transaction\n");
        abort();
    }
    g_txn.active = 1;
    g_txn.n_blocks = 0;
}

/* Mark a block as touched by the current transaction. Caller is
 * responsible for modifying the block contents in the mmap (we capture
 * the after-image at commit time). */
static int txn_touch(uint32_t target) {
    if (!g_txn.active) {
        fprintf(stderr, "uffs_journal: BUG: touch outside transaction\n");
        abort();
    }
    /* Dedup. */
    for (int i = 0; i < g_txn.n_blocks; i++)
        if (g_txn.targets[i] == target) return 0;
    if (g_txn.n_blocks >= J_MAX_BLOCKS_PER_TXN) return -ENOSPC;
    g_txn.targets[g_txn.n_blocks++] = target;
    return 0;
}

/* How many journal blocks would this txn occupy? desc + N data + commit. */
static uint32_t txn_size_blocks(void) {
    return 1 + g_txn.n_blocks + 1;
}

/* How many free journal blocks are there right now? */
static uint32_t journal_free_blocks(void) {
    uint32_t used = (sb->journal_tail + sb->journal_blocks - sb->journal_head)
                    % sb->journal_blocks;
    /* Leave 1 block unused so head==tail unambiguously means empty. */
    return sb->journal_blocks - used - 1;
}

/* Commit the staged transaction:
 *   1. Write the descriptor + after-images + commit to the journal ring.
 *   2. fsync the journal.
 *   3. Update sb->journal_tail.
 *   4. fsync the target blocks (the actual on-disk locations).
 *   5. Advance sb->journal_head.
 *
 * Steps 1-3 make the transaction durable. Steps 4-5 perform the actual
 * "checkpoint" — flushing the modifications to their real homes and
 * freeing journal space. */
static int txn_commit(void) {
    if (!g_txn.active) { fprintf(stderr, "BUG: commit w/o begin\n"); abort(); }

    if (g_txn.n_blocks == 0) {
        /* No-op transaction. */
        g_txn.active = 0;
        return 0;
    }

    uint32_t need = txn_size_blocks();
    if (need > journal_free_blocks()) {
        /* Should never happen with current limits, but be honest. */
        fprintf(stderr, "uffs_journal: journal full (%u needed, %u free)\n",
                need, journal_free_blocks());
        g_txn.active = 0;
        return -ENOSPC;
    }

    uint64_t seq = ++sb->journal_seq;
    uint32_t pos = sb->journal_tail;

    /* (1a) descriptor block */
    j_descriptor_t *desc = block_ptr(j_block(pos));
    memset(desc, 0, UFFS_BLOCK_SIZE);
    desc->magic = J_MAGIC_DESCRIPTOR;
    desc->seq = seq;
    desc->n_blocks = g_txn.n_blocks;
    for (int i = 0; i < g_txn.n_blocks; i++) desc->targets[i] = g_txn.targets[i];
    pos++;

    /* (1b) after-images */
    for (int i = 0; i < g_txn.n_blocks; i++) {
        void *jslot = block_ptr(j_block(pos));
        void *src   = block_ptr(g_txn.targets[i]);
        memcpy(jslot, src, UFFS_BLOCK_SIZE);
        pos++;
    }

    /* Flush descriptor + after-images BEFORE the commit block, so we
     * can never observe a commit without the data it references. */
    for (uint32_t p = sb->journal_tail; p < pos; p++)
        sync_block_sync(j_block(p));

    /* (1c) commit block */
    j_commit_t *cm = block_ptr(j_block(pos));
    memset(cm, 0, UFFS_BLOCK_SIZE);
    cm->magic = J_MAGIC_COMMIT;
    cm->seq = seq;
    sync_block_sync(j_block(pos));
    pos++;

    /* (2,3) advance tail and persist superblock */
    sb->journal_tail = pos % sb->journal_blocks;
    sync_range_sync(sb, sizeof(*sb));

    /* (4) Now safe to flush target blocks to their real locations. */
    for (int i = 0; i < g_txn.n_blocks; i++)
        sync_block_sync(g_txn.targets[i]);

    /* (5) Checkpoint complete — release journal space. */
    sb->journal_head = sb->journal_tail;
    sync_range_sync(sb, sizeof(*sb));

    g_txn.active = 0;
    g_txn.n_blocks = 0;
    return 0;
}

/* Convenience: wrap a single-block change. */
static int txn_one(uint32_t target) {
    txn_begin();
    int rc = txn_touch(target);
    if (rc < 0) { g_txn.active = 0; return rc; }
    return txn_commit();
}

/* Helpers that combine "modify in mmap" + "journal that block". */
static uint32_t inode_block(uint32_t ino) {
    /* Which physical block holds inode N? */
    uint32_t inodes_per_block = UFFS_BLOCK_SIZE / sizeof(uffs_inode_t);
    return sb->inode_table_start + (ino / inodes_per_block);
}
static int txn_touch_inode(uint32_t ino) { return txn_touch(inode_block(ino)); }
static int txn_touch_bitmap(uint32_t b) {
    /* Which bitmap block covers data block b? */
    uint32_t bit_byte = b / 8;
    uint32_t off_in_bitmap = bit_byte / UFFS_BLOCK_SIZE;
    return txn_touch(sb->bitmap_start + off_in_bitmap);
}

/* ---- replay --------------------------------------------------------- */

/* Read a journal slot and try to parse it as a descriptor. Returns
 * 0 if it's a valid descriptor with a matching commit block downstream,
 * else nonzero. */
static int try_replay_one(uint32_t *cursor) {
    uint32_t pos = *cursor;
    j_descriptor_t *desc = block_ptr(j_block(pos));
    if (desc->magic != J_MAGIC_DESCRIPTOR) return -1;
    if (desc->n_blocks == 0 || desc->n_blocks > J_MAX_BLOCKS_PER_TXN) return -1;

    uint32_t commit_pos = pos + 1 + desc->n_blocks;
    j_commit_t *cm = block_ptr(j_block(commit_pos));
    if (cm->magic != J_MAGIC_COMMIT || cm->seq != desc->seq) return -1;

    /* Replay: copy each after-image to its target. */
    for (uint32_t i = 0; i < desc->n_blocks; i++) {
        uint32_t target = desc->targets[i];
        if (target >= sb->total_blocks) return -1;   /* corrupt */
        void *src = block_ptr(j_block(pos + 1 + i));
        void *dst = block_ptr(target);
        memcpy(dst, src, UFFS_BLOCK_SIZE);
        sync_block_sync(target);
    }
    *cursor = commit_pos + 1;
    return 0;
}

static void journal_replay(void) {
    uint32_t cursor = sb->journal_head;
    int n_replayed = 0;
    while (cursor != sb->journal_tail) {
        if (try_replay_one(&cursor) < 0) break;
        n_replayed++;
    }
    if (n_replayed > 0) {
        fprintf(stderr, "uffs_journal: replayed %d transaction(s)\n", n_replayed);
    }
    /* Reset journal. The data we just wrote is durable in its real
     * homes, so the log content is no longer needed. */
    sb->journal_head = sb->journal_tail = 0;
    sync_range_sync(sb, sizeof(*sb));
}

/* ====================================================================== */
/*  Block / inode / dir layer — same shape as uffs_blocks, plus txn calls */
/* ====================================================================== */

static int alloc_block(void) {
    for (uint32_t b = sb->data_start; b < sb->total_blocks; b++) {
        if (!bit_get(b)) {
            bit_set(b);
            memset(block_ptr(b), 0, UFFS_BLOCK_SIZE);
            txn_touch_bitmap(b);
            /* data block content isn't journaled, but we touch it so a
             * subsequent caller writing to it can stage it explicitly */
            return b;
        }
    }
    return -ENOSPC;
}

static void free_block(uint32_t b) {
    if (b < sb->data_start || b >= sb->total_blocks) return;
    bit_clr(b);
    txn_touch_bitmap(b);
}

static int alloc_inode(void) {
    for (uint32_t i = 1; i < sb->inode_count; i++) {
        if (!itab[i].in_use) {
            memset(&itab[i], 0, sizeof(itab[i]));
            itab[i].in_use = 1;
            itab[i].atime = itab[i].mtime = itab[i].ctime = time(NULL);
            txn_touch_inode(i);
            return i;
        }
    }
    return -ENOSPC;
}

static void inode_release_blocks(uffs_inode_t *n) {
    for (uint32_t i = 0; i < UFFS_DIRECT; i++)
        if (n->direct[i]) { free_block(n->direct[i]); n->direct[i] = 0; }
    if (n->indirect) {
        uint32_t *ind = block_ptr(n->indirect);
        for (uint32_t i = 0; i < UFFS_INDIRECT_PTRS; i++)
            if (ind[i]) free_block(ind[i]);
        free_block(n->indirect);
        n->indirect = 0;
    }
}

static void free_inode(int ino) {
    if (ino <= 0 || (uint32_t)ino >= sb->inode_count) return;
    inode_release_blocks(&itab[ino]);
    memset(&itab[ino], 0, sizeof(itab[ino]));
    txn_touch_inode(ino);
}

static int inode_bmap(uffs_inode_t *n, uint32_t lbn, int allocate) {
    uint32_t ino_idx = n - itab;
    if (lbn < UFFS_DIRECT) {
        if (n->direct[lbn] == 0 && allocate) {
            int b = alloc_block(); if (b < 0) return b;
            n->direct[lbn] = b;
            txn_touch_inode(ino_idx);
        }
        return n->direct[lbn];
    }
    uint32_t idx = lbn - UFFS_DIRECT;
    if (idx >= UFFS_INDIRECT_PTRS) return -EFBIG;
    if (n->indirect == 0) {
        if (!allocate) return 0;
        int b = alloc_block(); if (b < 0) return b;
        n->indirect = b;
        txn_touch_inode(ino_idx);
    }
    uint32_t *ind = block_ptr(n->indirect);
    if (ind[idx] == 0 && allocate) {
        int b = alloc_block(); if (b < 0) return b;
        ind[idx] = b;
        txn_touch(n->indirect);
    }
    return ind[idx];
}

/* ---- directory layer ------------------------------------------------ */

static int dir_lookup(uffs_inode_t *dir, const char *name, uint32_t *slot_out) {
    uint32_t nblocks = (dir->size + UFFS_BLOCK_SIZE - 1) / UFFS_BLOCK_SIZE;
    for (uint32_t lbn = 0; lbn < nblocks; lbn++) {
        int b = inode_bmap(dir, lbn, 0);
        if (b <= 0) continue;
        uffs_dirent_t *de = block_ptr(b);
        for (uint32_t i = 0; i < UFFS_DIRENTS_PER_BLOCK; i++)
            if (de[i].ino && strcmp(de[i].name, name) == 0) {
                if (slot_out) *slot_out = lbn * UFFS_DIRENTS_PER_BLOCK + i;
                return de[i].ino;
            }
    }
    return -ENOENT;
}

static int dir_add(int dir_ino, const char *name, int child) {
    uffs_inode_t *dir = &itab[dir_ino];
    uint32_t nblocks = (dir->size + UFFS_BLOCK_SIZE - 1) / UFFS_BLOCK_SIZE;
    for (uint32_t lbn = 0; lbn < nblocks; lbn++) {
        int b = inode_bmap(dir, lbn, 0);
        if (b <= 0) continue;
        uffs_dirent_t *de = block_ptr(b);
        for (uint32_t i = 0; i < UFFS_DIRENTS_PER_BLOCK; i++) {
            if (!de[i].ino) {
                de[i].ino = child;
                strncpy(de[i].name, name, UFFS_MAX_NAME);
                de[i].name[UFFS_MAX_NAME] = 0;
                txn_touch(b);
                dir->mtime = dir->ctime = time(NULL);
                txn_touch_inode(dir_ino);
                return 0;
            }
        }
    }
    /* Extend the directory. */
    uint32_t new_lbn = nblocks;
    int b = inode_bmap(dir, new_lbn, 1);
    if (b < 0) return b;
    uffs_dirent_t *de = block_ptr(b);
    memset(de, 0, UFFS_BLOCK_SIZE);
    de[0].ino = child;
    strncpy(de[0].name, name, UFFS_MAX_NAME);
    de[0].name[UFFS_MAX_NAME] = 0;
    txn_touch(b);
    dir->size = (uint64_t)(new_lbn + 1) * UFFS_BLOCK_SIZE;
    dir->mtime = dir->ctime = time(NULL);
    txn_touch_inode(dir_ino);
    return 0;
}

static int dir_remove(int dir_ino, const char *name) {
    uffs_inode_t *dir = &itab[dir_ino];
    uint32_t nblocks = (dir->size + UFFS_BLOCK_SIZE - 1) / UFFS_BLOCK_SIZE;
    for (uint32_t lbn = 0; lbn < nblocks; lbn++) {
        int b = inode_bmap(dir, lbn, 0);
        if (b <= 0) continue;
        uffs_dirent_t *de = block_ptr(b);
        for (uint32_t i = 0; i < UFFS_DIRENTS_PER_BLOCK; i++) {
            if (de[i].ino && strcmp(de[i].name, name) == 0) {
                de[i].ino = 0;
                de[i].name[0] = 0;
                txn_touch(b);
                dir->mtime = dir->ctime = time(NULL);
                txn_touch_inode(dir_ino);
                return 0;
            }
        }
    }
    return -ENOENT;
}

static int dir_is_empty(uffs_inode_t *dir) {
    uint32_t nblocks = (dir->size + UFFS_BLOCK_SIZE - 1) / UFFS_BLOCK_SIZE;
    for (uint32_t lbn = 0; lbn < nblocks; lbn++) {
        int b = inode_bmap(dir, lbn, 0);
        if (b <= 0) continue;
        uffs_dirent_t *de = block_ptr(b);
        for (uint32_t i = 0; i < UFFS_DIRENTS_PER_BLOCK; i++)
            if (de[i].ino) return 0;
    }
    return 1;
}

/* ---- path lookup --------------------------------------------------- */

static int path_lookup(const char *path, int *parent_out, char *leaf_out) {
    if (path[0] != '/') return -EINVAL;
    int cur = sb->root_inode, parent = -1;
    char leaf[UFFS_MAX_NAME + 1] = "";
    const char *p = path + 1;
    while (*p) {
        const char *slash = strchr(p, '/');
        size_t len = slash ? (size_t)(slash - p) : strlen(p);
        if (len == 0) { p++; continue; }
        if (len > UFFS_MAX_NAME) return -ENAMETOOLONG;
        char comp[UFFS_MAX_NAME + 1];
        memcpy(comp, p, len); comp[len] = 0;
        if (itab[cur].type != T_DIR) return -ENOTDIR;
        int next = dir_lookup(&itab[cur], comp, NULL);
        parent = cur; strncpy(leaf, comp, sizeof(leaf));
        if (next < 0) {
            if (slash == NULL || *(slash + 1) == 0) {
                if (parent_out) *parent_out = parent;
                if (leaf_out) strcpy(leaf_out, leaf);
                return -ENOENT;
            }
            return -ENOENT;
        }
        cur = next;
        if (!slash) break;
        p = slash + 1;
    }
    if (parent_out) *parent_out = parent;
    if (leaf_out) strcpy(leaf_out, leaf);
    return cur;
}

/* ---- file I/O -------------------------------------------------------- */

static int file_read(uffs_inode_t *n, char *buf, size_t size, off_t off) {
    if ((uint64_t)off >= n->size) return 0;
    if ((uint64_t)off + size > n->size) size = n->size - off;
    size_t done = 0;
    while (done < size) {
        uint32_t lbn = (off + done) / UFFS_BLOCK_SIZE;
        uint32_t bo  = (off + done) % UFFS_BLOCK_SIZE;
        size_t chunk = UFFS_BLOCK_SIZE - bo;
        if (chunk > size - done) chunk = size - done;
        int b = inode_bmap(n, lbn, 0);
        if (b > 0) memcpy(buf + done, (char *)block_ptr(b) + bo, chunk);
        else       memset(buf + done, 0, chunk);
        done += chunk;
    }
    return size;
}

/* file_write does data=ordered: it writes data blocks directly (no
 * journal), but journals the metadata update (size, mtime, possibly
 * new direct/indirect pointers). On a crash mid-write you may see a
 * file whose declared size points into uninitialized blocks; the
 * filesystem itself stays consistent. */
static int file_write(uffs_inode_t *n, const char *buf, size_t size, off_t off) {
    if ((uint64_t)off + size > UFFS_MAX_FILESZ) return -EFBIG;
    uint32_t ino_idx = n - itab;
    size_t done = 0;
    while (done < size) {
        uint32_t lbn = (off + done) / UFFS_BLOCK_SIZE;
        uint32_t bo  = (off + done) % UFFS_BLOCK_SIZE;
        size_t chunk = UFFS_BLOCK_SIZE - bo;
        if (chunk > size - done) chunk = size - done;
        int b = inode_bmap(n, lbn, 1);
        if (b < 0) return b;
        memcpy((char *)block_ptr(b) + bo, buf + done, chunk);
        /* Data block: not journaled, but we do msync it before the
         * commit so the journal's view of "the new size is durable"
         * matches what's on disk. */
        msync(block_ptr(b), UFFS_BLOCK_SIZE, MS_SYNC);
        done += chunk;
    }
    if ((uint64_t)off + size > n->size) n->size = off + size;
    n->mtime = n->ctime = time(NULL);
    txn_touch_inode(ino_idx);
    return size;
}

static int file_truncate(uffs_inode_t *n, uint64_t newsize) {
    if (newsize > UFFS_MAX_FILESZ) return -EFBIG;
    uint32_t ino_idx = n - itab;
    if (newsize < n->size) {
        uint32_t first_free = (newsize + UFFS_BLOCK_SIZE - 1) / UFFS_BLOCK_SIZE;
        uint32_t cur_blocks = (n->size + UFFS_BLOCK_SIZE - 1) / UFFS_BLOCK_SIZE;
        for (uint32_t lbn = first_free; lbn < cur_blocks; lbn++) {
            if (lbn < UFFS_DIRECT) {
                if (n->direct[lbn]) { free_block(n->direct[lbn]); n->direct[lbn] = 0; }
            } else if (n->indirect) {
                uint32_t *ind = block_ptr(n->indirect);
                uint32_t idx = lbn - UFFS_DIRECT;
                if (ind[idx]) { free_block(ind[idx]); ind[idx] = 0; }
                txn_touch(n->indirect);
            }
        }
        if (first_free <= UFFS_DIRECT && n->indirect) {
            free_block(n->indirect);
            n->indirect = 0;
        }
    }
    n->size = newsize;
    n->mtime = n->ctime = time(NULL);
    txn_touch_inode(ino_idx);
    return 0;
}

/* ---- backing-file open / format ------------------------------------- */

static int format_fresh(void) {
    uint32_t bitmap_bytes  = (UFFS_TOTAL_BLOCKS + 7) / 8;
    uint32_t bitmap_blocks = (bitmap_bytes + UFFS_BLOCK_SIZE - 1) / UFFS_BLOCK_SIZE;
    uint32_t itab_bytes    = UFFS_MAX_INODES * sizeof(uffs_inode_t);
    uint32_t itab_blocks   = (itab_bytes + UFFS_BLOCK_SIZE - 1) / UFFS_BLOCK_SIZE;

    uint32_t bitmap_start  = 1;
    uint32_t itab_start    = bitmap_start + bitmap_blocks;
    uint32_t journal_start = itab_start + itab_blocks;
    uint32_t data_start    = journal_start + UFFS_JOURNAL_BLOCKS;

    if (data_start >= UFFS_TOTAL_BLOCKS) {
        fprintf(stderr, "uffs_journal: image too small for metadata + journal\n");
        return -1;
    }

    memset(backing_map, 0, backing_size);
    sb = backing_map;
    sb->magic        = UFFS_MAGIC;
    sb->version      = UFFS_VERSION;
    sb->block_size   = UFFS_BLOCK_SIZE;
    sb->total_blocks = UFFS_TOTAL_BLOCKS;
    sb->bitmap_start = bitmap_start;
    sb->bitmap_blocks= bitmap_blocks;
    sb->inode_table_start  = itab_start;
    sb->inode_table_blocks = itab_blocks;
    sb->inode_count  = UFFS_MAX_INODES;
    sb->journal_start  = journal_start;
    sb->journal_blocks = UFFS_JOURNAL_BLOCKS;
    sb->journal_head = 0;
    sb->journal_tail = 0;
    sb->journal_seq  = 0;
    sb->data_start   = data_start;
    sb->root_inode   = 1;

    bitmap = (uint8_t *)block_ptr(bitmap_start);
    itab   = (uffs_inode_t *)block_ptr(itab_start);

    for (uint32_t b = 0; b < data_start; b++) bit_set(b);

    memset(&itab[1], 0, sizeof(itab[1]));
    itab[1].in_use = 1;
    itab[1].type   = T_DIR;
    itab[1].mode   = 0755;
    itab[1].nlink  = 2;
    itab[1].uid    = getuid();
    itab[1].gid    = getgid();
    itab[1].atime  = itab[1].mtime = itab[1].ctime = time(NULL);

    msync(backing_map, backing_size, MS_SYNC);
    fprintf(stderr, "uffs_journal: formatted (journal at %u for %u blocks, "
                    "data at %u)\n", journal_start, UFFS_JOURNAL_BLOCKS, data_start);
    return 0;
}

static int open_backing(const char *path) {
    int fd = open(path, O_RDWR | O_CREAT, 0644);
    if (fd < 0) { perror("open backing"); return -1; }
    size_t need = (size_t)UFFS_TOTAL_BLOCKS * UFFS_BLOCK_SIZE;
    struct stat st; fstat(fd, &st);
    int fresh = (st.st_size == 0);
    if (fresh && ftruncate(fd, need) < 0) { perror("ftruncate"); close(fd); return -1; }

    void *map = mmap(NULL, need, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) { perror("mmap"); close(fd); return -1; }

    backing_fd   = fd;
    backing_map  = map;
    backing_size = need;
    blocks_base  = map;

    if (fresh) {
        if (format_fresh() < 0) return -1;
    } else {
        sb = map;
        if (sb->magic != UFFS_MAGIC || sb->block_size != UFFS_BLOCK_SIZE) {
            fprintf(stderr, "uffs_journal: bad/incompatible image\n");
            return -1;
        }
        bitmap = (uint8_t *)block_ptr(sb->bitmap_start);
        itab   = (uffs_inode_t *)block_ptr(sb->inode_table_start);
        fprintf(stderr, "uffs_journal: reopened image\n");
        journal_replay();
    }
    return 0;
}

/* ====================================================================== */
/*  FUSE ops — each one wraps its work in txn_begin/txn_commit            */
/* ====================================================================== */

static int op_getattr(const char *path, struct stat *st, struct fuse_file_info *fi) {
    (void)fi;
    int ino = path_lookup(path, NULL, NULL);
    if (ino < 0) return ino;
    uffs_inode_t *n = &itab[ino];
    memset(st, 0, sizeof(*st));
    mode_t tb = S_IFREG;
    if (n->type == T_DIR) tb = S_IFDIR;
    else if (n->type == T_LNK) tb = S_IFLNK;
    st->st_ino = ino; st->st_mode = n->mode | tb;
    st->st_nlink = n->nlink; st->st_uid = n->uid; st->st_gid = n->gid;
    st->st_size = n->size;
    st->st_atime = n->atime; st->st_mtime = n->mtime; st->st_ctime = n->ctime;
    st->st_blksize = UFFS_BLOCK_SIZE;
    st->st_blocks  = (n->size + 511) / 512;
    return 0;
}

static int op_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
                      off_t off, struct fuse_file_info *fi,
                      enum fuse_readdir_flags fl) {
    (void)off; (void)fi; (void)fl;
    int ino = path_lookup(path, NULL, NULL);
    if (ino < 0) return ino;
    uffs_inode_t *dir = &itab[ino];
    if (dir->type != T_DIR) return -ENOTDIR;
    filler(buf, ".",  NULL, 0, 0);
    filler(buf, "..", NULL, 0, 0);
    uint32_t nblocks = (dir->size + UFFS_BLOCK_SIZE - 1) / UFFS_BLOCK_SIZE;
    for (uint32_t lbn = 0; lbn < nblocks; lbn++) {
        int b = inode_bmap(dir, lbn, 0);
        if (b <= 0) continue;
        uffs_dirent_t *de = block_ptr(b);
        for (uint32_t i = 0; i < UFFS_DIRENTS_PER_BLOCK; i++)
            if (de[i].ino) filler(buf, de[i].name, NULL, 0, 0);
    }
    return 0;
}

static int mknod_common(const char *path, mode_t mode, int type) {
    int parent; char leaf[UFFS_MAX_NAME + 1];
    int ino = path_lookup(path, &parent, leaf);
    if (ino >= 0) return -EEXIST;
    if (ino != -ENOENT) return ino;
    if (parent < 0) return -EINVAL;

    txn_begin();
    int ni = alloc_inode();
    if (ni < 0) { g_txn.active = 0; return ni; }
    uffs_inode_t *n = &itab[ni];
    n->type  = type;
    n->mode  = mode & 0777;
    n->uid   = fuse_get_context()->uid;
    n->gid   = fuse_get_context()->gid;
    n->nlink = (type == T_DIR) ? 2 : 1;
    txn_touch_inode(ni);
    int rc = dir_add(parent, leaf, ni);
    if (rc < 0) { free_inode(ni); txn_commit(); return rc; }
    return txn_commit() < 0 ? -EIO : ni;
}

static int op_create(const char *path, mode_t mode, struct fuse_file_info *fi) {
    (void)fi; int rc = mknod_common(path, mode, T_REG); return rc < 0 ? rc : 0;
}
static int op_mkdir(const char *path, mode_t mode) {
    int rc = mknod_common(path, mode, T_DIR); return rc < 0 ? rc : 0;
}

static int op_unlink(const char *path) {
    int parent; char leaf[UFFS_MAX_NAME + 1];
    int ino = path_lookup(path, &parent, leaf);
    if (ino < 0) return ino;
    if (itab[ino].type == T_DIR) return -EISDIR;
    txn_begin();
    dir_remove(parent, leaf);
    if (--itab[ino].nlink == 0) free_inode(ino);
    else txn_touch_inode(ino);
    return txn_commit() < 0 ? -EIO : 0;
}

static int op_rmdir(const char *path) {
    int parent; char leaf[UFFS_MAX_NAME + 1];
    int ino = path_lookup(path, &parent, leaf);
    if (ino < 0) return ino;
    if (itab[ino].type != T_DIR) return -ENOTDIR;
    if (!dir_is_empty(&itab[ino])) return -ENOTEMPTY;
    txn_begin();
    dir_remove(parent, leaf);
    free_inode(ino);
    return txn_commit() < 0 ? -EIO : 0;
}

static int op_open(const char *path, struct fuse_file_info *fi) {
    (void)fi;
    int ino = path_lookup(path, NULL, NULL);
    if (ino < 0) return ino;
    if (itab[ino].type == T_DIR) return -EISDIR;
    return 0;
}

static int op_read(const char *path, char *buf, size_t size, off_t off,
                   struct fuse_file_info *fi) {
    (void)fi;
    int ino = path_lookup(path, NULL, NULL);
    if (ino < 0) return ino;
    int n = file_read(&itab[ino], buf, size, off);
    /* atime is intentionally not journaled — too expensive */
    itab[ino].atime = time(NULL);
    return n;
}

static int op_write(const char *path, const char *buf, size_t size, off_t off,
                    struct fuse_file_info *fi) {
    (void)fi;
    int ino = path_lookup(path, NULL, NULL);
    if (ino < 0) return ino;
    txn_begin();
    int n = file_write(&itab[ino], buf, size, off);
    if (n < 0) { txn_commit(); return n; }
    int c = txn_commit();
    return c < 0 ? -EIO : n;
}

static int op_truncate(const char *path, off_t size, struct fuse_file_info *fi) {
    (void)fi;
    int ino = path_lookup(path, NULL, NULL);
    if (ino < 0) return ino;
    txn_begin();
    int rc = file_truncate(&itab[ino], size);
    int c = txn_commit();
    return rc < 0 ? rc : (c < 0 ? -EIO : 0);
}

static int op_chmod(const char *path, mode_t mode, struct fuse_file_info *fi) {
    (void)fi;
    int ino = path_lookup(path, NULL, NULL);
    if (ino < 0) return ino;
    txn_begin();
    itab[ino].mode = mode & 0777;
    itab[ino].ctime = time(NULL);
    txn_touch_inode(ino);
    return txn_commit() < 0 ? -EIO : 0;
}

static int op_chown(const char *path, uid_t uid, gid_t gid, struct fuse_file_info *fi) {
    (void)fi;
    int ino = path_lookup(path, NULL, NULL);
    if (ino < 0) return ino;
    txn_begin();
    if (uid != (uid_t)-1) itab[ino].uid = uid;
    if (gid != (gid_t)-1) itab[ino].gid = gid;
    itab[ino].ctime = time(NULL);
    txn_touch_inode(ino);
    return txn_commit() < 0 ? -EIO : 0;
}

static int op_utimens(const char *path, const struct timespec tv[2],
                      struct fuse_file_info *fi) {
    (void)fi;
    int ino = path_lookup(path, NULL, NULL);
    if (ino < 0) return ino;
    txn_begin();
    itab[ino].atime = tv[0].tv_sec;
    itab[ino].mtime = tv[1].tv_sec;
    txn_touch_inode(ino);
    return txn_commit() < 0 ? -EIO : 0;
}

static int op_rename(const char *from, const char *to, unsigned int flags) {
    if (flags) return -EINVAL;
    int sp; char sl[UFFS_MAX_NAME + 1];
    int src = path_lookup(from, &sp, sl);
    if (src < 0) return src;
    int dp; char dl[UFFS_MAX_NAME + 1];
    int dst = path_lookup(to, &dp, dl);
    if (dst == src) return 0;
    txn_begin();
    if (dst >= 0) {
        if (itab[dst].type == T_DIR) {
            if (itab[src].type != T_DIR) { txn_commit(); return -EISDIR; }
            if (!dir_is_empty(&itab[dst])) { txn_commit(); return -ENOTEMPTY; }
        } else if (itab[src].type == T_DIR) {
            txn_commit(); return -ENOTDIR;
        }
        dir_remove(dp, dl);
        if (--itab[dst].nlink == 0) free_inode(dst);
    } else if (dst != -ENOENT) {
        txn_commit(); return dst;
    }
    dir_remove(sp, sl);
    int rc = dir_add(dp, dl, src);
    int c = txn_commit();
    return rc < 0 ? rc : (c < 0 ? -EIO : 0);
}

static int op_statfs(const char *path, struct statvfs *st) {
    (void)path;
    uint32_t used_blocks = 0;
    for (uint32_t b = sb->data_start; b < sb->total_blocks; b++)
        if (bit_get(b)) used_blocks++;
    uint32_t used_inodes = 0;
    for (uint32_t i = 1; i < sb->inode_count; i++)
        if (itab[i].in_use) used_inodes++;
    memset(st, 0, sizeof(*st));
    uint32_t data_blocks = sb->total_blocks - sb->data_start;
    st->f_bsize = UFFS_BLOCK_SIZE; st->f_frsize = UFFS_BLOCK_SIZE;
    st->f_blocks = data_blocks; st->f_bfree = data_blocks - used_blocks;
    st->f_bavail = st->f_bfree;
    st->f_files = sb->inode_count; st->f_ffree = sb->inode_count - used_inodes;
    st->f_namemax = UFFS_MAX_NAME;
    return 0;
}

static void op_destroy(void *priv) {
    (void)priv;
    if (backing_map) msync(backing_map, backing_size, MS_SYNC);
    if (backing_fd >= 0) close(backing_fd);
    fprintf(stderr, "uffs_journal: synced and closed.\n");
}

static const struct fuse_operations ops = {
    .destroy = op_destroy,
    .getattr = op_getattr, .readdir = op_readdir,
    .create = op_create,   .mkdir = op_mkdir,
    .unlink = op_unlink,   .rmdir = op_rmdir,
    .open = op_open,
    .read = op_read,       .write = op_write,
    .truncate = op_truncate,
    .chmod = op_chmod,     .chown = op_chown,
    .utimens = op_utimens, .rename = op_rename,
    .statfs = op_statfs,
};

struct opt_holder { char *backing; };
#define OPT(t, p) { t, offsetof(struct opt_holder, p), 1 }
static const struct fuse_opt opt_spec[] = {
    OPT("backing=%s", backing),
    FUSE_OPT_END
};

int main(int argc, char *argv[]) {
    struct fuse_args args = FUSE_ARGS_INIT(argc, argv);
    struct opt_holder opts = {0};
    if (fuse_opt_parse(&args, &opts, opt_spec, NULL) == -1) return 1;
    if (!opts.backing) {
        fprintf(stderr, "usage: uffs_journal -o backing=PATH MOUNTPOINT\n");
        return 1;
    }
    if (open_backing(opts.backing) < 0) return 1;
    return fuse_main(args.argc, args.argv, &ops, NULL);
}
