/*
 * uffs_core_full.c — the merged core. Combines:
 *   - the RPC server shape from uffs_core_persist (multi-node, threaded)
 *   - the block-allocated on-disk layout from uffs_blocks
 *   - the metadata write-ahead log from uffs_journal
 *
 * Speaks the same wire protocol as uffs_core / uffs_core_persist, so
 * the existing uffs_node binary connects to it unchanged.
 *
 * Build:
 *   cc -Wall -O2 -pthread uffs_core_full.c -o uffs_core_full
 *
 * Run:
 *   ./uffs_core_full /tmp/uffs.img /tmp/uffs_core.sock
 *
 * Then mount one or more nodes:
 *   ./uffs_node -f -o core=/tmp/uffs_core.sock,entry_timeout=0,attr_timeout=0,ac_attr_timeout=0 /tmp/mnt
 *
 * IMPORTANT: when multiple nodes share this core, you MUST mount with
 * entry_timeout=0,attr_timeout=0,ac_attr_timeout=0 — otherwise the
 * kernel's FUSE attribute cache on each node serves stale data.
 * Fixing this requires moving the node to FUSE low-level API and
 * pushing inval callbacks from the core; out of scope here.
 *
 * Caveats: one global lock; one in-flight transaction at a time; no
 * journal checksums; replay isn't itself idempotent under crash-during-
 * replay. See the README for the full list.
 */
#define _GNU_SOURCE
#include "uffs_proto.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <stdint.h>
#include <pthread.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/mman.h>
#include <sys/stat.h>

/* ---- layout constants (must match uffs_journal.c) ------------------- */

#define UFFS_BLOCK_SIZE      4096u
#define UFFS_MAGIC           0x55464A4Cu   /* 'UFJL' */
#define UFFS_VERSION         3

#define UFFS_TOTAL_BLOCKS    4096u
#define UFFS_JOURNAL_BLOCKS  64u
#define UFFS_MAX_INODES      512u
#define UFFS_DIRECT          12u
#define UFFS_INDIRECT_PTRS   (UFFS_BLOCK_SIZE / sizeof(uint32_t))

#define UFFS_MAX_NAME        59
#define UFFS_DIRENT_SIZE     64
#define UFFS_DIRENTS_PER_BLOCK (UFFS_BLOCK_SIZE / UFFS_DIRENT_SIZE)

#define UFFS_MAX_FILESZ \
    ((uint64_t)(UFFS_DIRECT + UFFS_INDIRECT_PTRS) * UFFS_BLOCK_SIZE)

#define J_MAGIC_DESCRIPTOR   0x44455343u
#define J_MAGIC_COMMIT       0x434F4D4Du
#define J_MAX_BLOCKS_PER_TXN 16

enum { T_REG=0, T_DIR=1, T_LNK=2 };

/* ---- on-disk structures --------------------------------------------- */

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

typedef struct {
    uint32_t magic;
    uint64_t seq;
    uint32_t n_blocks;
    uint32_t targets[J_MAX_BLOCKS_PER_TXN];
} j_descriptor_t;

typedef struct { uint32_t magic; uint64_t seq; } j_commit_t;

_Static_assert(sizeof(uffs_dirent_t) == UFFS_DIRENT_SIZE, "dirent 64");

/* ---- globals -------------------------------------------------------- */

static int            backing_fd = -1;
static void          *backing_map;
static size_t         backing_size;
static uffs_super_t  *sb;
static uint8_t       *bitmap;
static uffs_inode_t  *itab;
static char          *blocks_base;

/* One global lock across the whole filesystem. Held for every op. */
static pthread_mutex_t fs_lock = PTHREAD_MUTEX_INITIALIZER;

/* Push-channel registry: nodes that opened a second connection and sent
 * the hello marker land here. broadcast_inval() walks this list and
 * sends INVAL_INODE to each. Protected by its own lock so we don't
 * hold fs_lock during network I/O. */
#define MAX_PUSH 32
static int             push_fds[MAX_PUSH];
static int             push_n;
static pthread_mutex_t push_mu = PTHREAD_MUTEX_INITIALIZER;

static void push_register(int fd) {
    pthread_mutex_lock(&push_mu);
    if (push_n < MAX_PUSH) push_fds[push_n++] = fd;
    pthread_mutex_unlock(&push_mu);
    fprintf(stderr, "core_full: push channel registered (fd %d, total %d)\n",
            fd, push_n);
}
static void push_unregister(int fd) {
    pthread_mutex_lock(&push_mu);
    for (int i = 0; i < push_n; i++) {
        if (push_fds[i] == fd) {
            push_fds[i] = push_fds[--push_n];
            break;
        }
    }
    pthread_mutex_unlock(&push_mu);
}
static void broadcast_inval(uint64_t ino) {
    uffs_push_t msg = { .opcode = PUSH_INVAL_INODE, .ino = ino };
    pthread_mutex_lock(&push_mu);
    for (int i = 0; i < push_n; i++) {
        /* Best-effort. If the write fails we just keep the fd around;
         * the conn_thread for the push side will eventually notice the
         * disconnect and unregister. */
        ssize_t w = write(push_fds[i], &msg, sizeof(msg));
        (void)w;
    }
    pthread_mutex_unlock(&push_mu);
}

/* Per-transaction staging (single-txn-at-a-time, protected by fs_lock). */
typedef struct {
    int      active;
    int      n_blocks;
    uint32_t targets[J_MAX_BLOCKS_PER_TXN];
    /* Inodes whose data the caller modified — broadcast after commit. */
    int      n_inodes;
    uint32_t inodes[J_MAX_BLOCKS_PER_TXN];
} txn_t;
static txn_t g_txn;

/* When a transaction commits, the inodes it touched are copied here.
 * After releasing fs_lock, the handler calls do_pending_broadcasts()
 * to actually push invalidations on the wire — broadcasting under
 * fs_lock would let a slow push reader block all FS ops. */
static __thread int      pending_n;
static __thread uint32_t pending_inos[J_MAX_BLOCKS_PER_TXN];

static void do_pending_broadcasts(void) {
    for (int i = 0; i < pending_n; i++) broadcast_inval(pending_inos[i]);
    pending_n = 0;
}

/* Record that this transaction modified inode `ino` (so we'll broadcast
 * an invalidation after commit). Idempotent. */
static void txn_note_inode(uint32_t ino) {
    if (!g_txn.active) return;
    for (int i = 0; i < g_txn.n_inodes; i++)
        if (g_txn.inodes[i] == ino) return;
    if (g_txn.n_inodes < (int)(sizeof(g_txn.inodes)/sizeof(g_txn.inodes[0])))
        g_txn.inodes[g_txn.n_inodes++] = ino;
}

/* ---- block helpers --------------------------------------------------- */

static void *block_ptr(uint32_t b) { return blocks_base + (size_t)b * UFFS_BLOCK_SIZE; }
static void sync_block_sync(uint32_t b) { msync(block_ptr(b), UFFS_BLOCK_SIZE, MS_SYNC); }
static void sync_range_sync(void *p, size_t len) {
    long ps = sysconf(_SC_PAGESIZE);
    uintptr_t s = (uintptr_t)p & ~(ps - 1);
    uintptr_t e = ((uintptr_t)p + len + ps - 1) & ~(ps - 1);
    msync((void *)s, e - s, MS_SYNC);
}
static int  bit_get(uint32_t b) { return (bitmap[b >> 3] >> (b & 7)) & 1; }
static void bit_set(uint32_t b) { bitmap[b >> 3] |=  (1u << (b & 7)); }
static void bit_clr(uint32_t b) { bitmap[b >> 3] &= ~(1u << (b & 7)); }

/* ---- journal -------------------------------------------------------- */

static uint32_t j_block(uint32_t i) {
    return sb->journal_start + (i % sb->journal_blocks);
}

static void txn_begin(void) {
    if (g_txn.active) { fprintf(stderr, "BUG: nested txn\n"); abort(); }
    g_txn.active = 1;
    g_txn.n_blocks = 0;
    g_txn.n_inodes = 0;
}

static int txn_touch(uint32_t target) {
    if (!g_txn.active) { fprintf(stderr, "BUG: touch outside txn\n"); abort(); }
    for (int i = 0; i < g_txn.n_blocks; i++)
        if (g_txn.targets[i] == target) return 0;
    if (g_txn.n_blocks >= J_MAX_BLOCKS_PER_TXN) return -ENOSPC;
    g_txn.targets[g_txn.n_blocks++] = target;
    return 0;
}

static uint32_t journal_free_blocks(void) {
    uint32_t used = (sb->journal_tail + sb->journal_blocks - sb->journal_head)
                    % sb->journal_blocks;
    return sb->journal_blocks - used - 1;
}

static int txn_commit(void) {
    if (!g_txn.active) { fprintf(stderr, "BUG: commit w/o begin\n"); abort(); }
    pending_n = 0;
    if (g_txn.n_blocks == 0) { g_txn.active = 0; return 0; }

    uint32_t need = 1 + g_txn.n_blocks + 1;
    if (need > journal_free_blocks()) {
        g_txn.active = 0;
        return -ENOSPC;
    }

    uint64_t seq = ++sb->journal_seq;
    uint32_t pos = sb->journal_tail;

    j_descriptor_t *desc = block_ptr(j_block(pos));
    memset(desc, 0, UFFS_BLOCK_SIZE);
    desc->magic = J_MAGIC_DESCRIPTOR;
    desc->seq = seq;
    desc->n_blocks = g_txn.n_blocks;
    for (int i = 0; i < g_txn.n_blocks; i++) desc->targets[i] = g_txn.targets[i];
    pos++;

    for (int i = 0; i < g_txn.n_blocks; i++) {
        memcpy(block_ptr(j_block(pos)), block_ptr(g_txn.targets[i]), UFFS_BLOCK_SIZE);
        pos++;
    }
    for (uint32_t p = sb->journal_tail; p < pos; p++) sync_block_sync(j_block(p));

    j_commit_t *cm = block_ptr(j_block(pos));
    memset(cm, 0, UFFS_BLOCK_SIZE);
    cm->magic = J_MAGIC_COMMIT;
    cm->seq = seq;
    sync_block_sync(j_block(pos));
    pos++;

    sb->journal_tail = pos % sb->journal_blocks;
    sync_range_sync(sb, sizeof(*sb));

    for (int i = 0; i < g_txn.n_blocks; i++)
        sync_block_sync(g_txn.targets[i]);

    sb->journal_head = sb->journal_tail;
    sync_range_sync(sb, sizeof(*sb));

    /* Snapshot touched inodes for post-unlock broadcast. */
    pending_n = g_txn.n_inodes;
    memcpy(pending_inos, g_txn.inodes, g_txn.n_inodes * sizeof(uint32_t));

    g_txn.active = 0;
    g_txn.n_blocks = 0;
    return 0;
}

static uint32_t inode_block(uint32_t ino) {
    uint32_t per_block = UFFS_BLOCK_SIZE / sizeof(uffs_inode_t);
    return sb->inode_table_start + (ino / per_block);
}
static int txn_touch_inode(uint32_t ino) {
    txn_note_inode(ino);
    return txn_touch(inode_block(ino));
}
static int txn_touch_bitmap(uint32_t b) {
    uint32_t bit_byte = b / 8;
    return txn_touch(sb->bitmap_start + (bit_byte / UFFS_BLOCK_SIZE));
}

static int try_replay_one(uint32_t *cursor) {
    j_descriptor_t *d = block_ptr(j_block(*cursor));
    if (d->magic != J_MAGIC_DESCRIPTOR) return -1;
    if (d->n_blocks == 0 || d->n_blocks > J_MAX_BLOCKS_PER_TXN) return -1;
    uint32_t commit_pos = *cursor + 1 + d->n_blocks;
    j_commit_t *c = block_ptr(j_block(commit_pos));
    if (c->magic != J_MAGIC_COMMIT || c->seq != d->seq) return -1;
    for (uint32_t i = 0; i < d->n_blocks; i++) {
        uint32_t t = d->targets[i];
        if (t >= sb->total_blocks) return -1;
        memcpy(block_ptr(t), block_ptr(j_block(*cursor + 1 + i)), UFFS_BLOCK_SIZE);
        sync_block_sync(t);
    }
    *cursor = commit_pos + 1;
    return 0;
}

static void journal_replay(void) {
    uint32_t cur = sb->journal_head;
    int n = 0;
    while (cur != sb->journal_tail) {
        if (try_replay_one(&cur) < 0) break;
        n++;
    }
    if (n > 0) fprintf(stderr, "core_full: replayed %d transaction(s)\n", n);
    sb->journal_head = sb->journal_tail = 0;
    sync_range_sync(sb, sizeof(*sb));
}

/* ---- inode/dir/file layer (verbatim from uffs_journal) -------------- */

static int alloc_block(void) {
    for (uint32_t b = sb->data_start; b < sb->total_blocks; b++) {
        if (!bit_get(b)) {
            bit_set(b);
            memset(block_ptr(b), 0, UFFS_BLOCK_SIZE);
            txn_touch_bitmap(b);
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
    for (uint32_t i = 1; i < sb->inode_count; i++)
        if (!itab[i].in_use) {
            memset(&itab[i], 0, sizeof(itab[i]));
            itab[i].in_use = 1;
            itab[i].atime = itab[i].mtime = itab[i].ctime = time(NULL);
            txn_touch_inode(i);
            return i;
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

static int dir_lookup(uffs_inode_t *dir, const char *name) {
    uint32_t nb = (dir->size + UFFS_BLOCK_SIZE - 1) / UFFS_BLOCK_SIZE;
    for (uint32_t lbn = 0; lbn < nb; lbn++) {
        int b = inode_bmap(dir, lbn, 0);
        if (b <= 0) continue;
        uffs_dirent_t *de = block_ptr(b);
        for (uint32_t i = 0; i < UFFS_DIRENTS_PER_BLOCK; i++)
            if (de[i].ino && strcmp(de[i].name, name) == 0) return de[i].ino;
    }
    return -ENOENT;
}

static int dir_add(int dir_ino, const char *name, int child) {
    uffs_inode_t *dir = &itab[dir_ino];
    uint32_t nb = (dir->size + UFFS_BLOCK_SIZE - 1) / UFFS_BLOCK_SIZE;
    for (uint32_t lbn = 0; lbn < nb; lbn++) {
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
    uint32_t new_lbn = nb;
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
    uint32_t nb = (dir->size + UFFS_BLOCK_SIZE - 1) / UFFS_BLOCK_SIZE;
    for (uint32_t lbn = 0; lbn < nb; lbn++) {
        int b = inode_bmap(dir, lbn, 0);
        if (b <= 0) continue;
        uffs_dirent_t *de = block_ptr(b);
        for (uint32_t i = 0; i < UFFS_DIRENTS_PER_BLOCK; i++) {
            if (de[i].ino && strcmp(de[i].name, name) == 0) {
                de[i].ino = 0; de[i].name[0] = 0;
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
    uint32_t nb = (dir->size + UFFS_BLOCK_SIZE - 1) / UFFS_BLOCK_SIZE;
    for (uint32_t lbn = 0; lbn < nb; lbn++) {
        int b = inode_bmap(dir, lbn, 0);
        if (b <= 0) continue;
        uffs_dirent_t *de = block_ptr(b);
        for (uint32_t i = 0; i < UFFS_DIRENTS_PER_BLOCK; i++)
            if (de[i].ino) return 0;
    }
    return 1;
}

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
        int next = dir_lookup(&itab[cur], comp);
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

/* ---- format / open / replay ----------------------------------------- */

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
        fprintf(stderr, "core_full: image too small\n");
        return -1;
    }

    memset(backing_map, 0, backing_size);
    sb = backing_map;
    sb->magic = UFFS_MAGIC;     sb->version = UFFS_VERSION;
    sb->block_size = UFFS_BLOCK_SIZE; sb->total_blocks = UFFS_TOTAL_BLOCKS;
    sb->bitmap_start = bitmap_start; sb->bitmap_blocks = bitmap_blocks;
    sb->inode_table_start = itab_start; sb->inode_table_blocks = itab_blocks;
    sb->inode_count = UFFS_MAX_INODES;
    sb->journal_start = journal_start; sb->journal_blocks = UFFS_JOURNAL_BLOCKS;
    sb->journal_head = sb->journal_tail = 0; sb->journal_seq = 0;
    sb->data_start = data_start; sb->root_inode = 1;

    bitmap = (uint8_t *)block_ptr(bitmap_start);
    itab   = (uffs_inode_t *)block_ptr(itab_start);
    for (uint32_t b = 0; b < data_start; b++) bit_set(b);

    memset(&itab[1], 0, sizeof(itab[1]));
    itab[1].in_use = 1;  itab[1].type = T_DIR;
    itab[1].mode = 0755; itab[1].nlink = 2;
    itab[1].uid = getuid(); itab[1].gid = getgid();
    itab[1].atime = itab[1].mtime = itab[1].ctime = time(NULL);
    msync(backing_map, backing_size, MS_SYNC);
    fprintf(stderr, "core_full: formatted (data starts at block %u)\n", data_start);
    return 0;
}

static int open_backing(const char *path) {
    int fd = open(path, O_RDWR | O_CREAT, 0644);
    if (fd < 0) { perror("open"); return -1; }
    size_t need = (size_t)UFFS_TOTAL_BLOCKS * UFFS_BLOCK_SIZE;
    struct stat st; fstat(fd, &st);
    int fresh = (st.st_size == 0);
    if (fresh && ftruncate(fd, need) < 0) { perror("ftruncate"); close(fd); return -1; }
    void *m = mmap(NULL, need, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (m == MAP_FAILED) { perror("mmap"); close(fd); return -1; }
    backing_fd = fd; backing_map = m; backing_size = need; blocks_base = m;
    if (fresh) {
        if (format_fresh() < 0) return -1;
    } else {
        sb = m;
        if (sb->magic != UFFS_MAGIC || sb->block_size != UFFS_BLOCK_SIZE) {
            fprintf(stderr, "core_full: bad image\n"); return -1;
        }
        bitmap = (uint8_t *)block_ptr(sb->bitmap_start);
        itab   = (uffs_inode_t *)block_ptr(sb->inode_table_start);
        fprintf(stderr, "core_full: reopened image\n");
        journal_replay();
    }
    return 0;
}

/* ====================================================================== */
/*  Wire I/O — same as uffs_core_persist                                  */
/* ====================================================================== */

static int read_all(int fd, void *buf, size_t n) {
    size_t got = 0;
    while (got < n) {
        ssize_t r = read(fd, (char*)buf + got, n - got);
        if (r == 0) return -1;
        if (r < 0) { if (errno == EINTR) continue; return -1; }
        got += r;
    }
    return 0;
}
static int write_all(int fd, const void *buf, size_t n) {
    size_t sent = 0;
    while (sent < n) {
        ssize_t w = write(fd, (const char*)buf + sent, n - sent);
        if (w < 0) { if (errno == EINTR) continue; return -1; }
        sent += w;
    }
    return 0;
}
static int send_reply(int fd, int32_t status, const void *payload, uint32_t plen) {
    uffs_frame_hdr_t h = {
        .length = sizeof(h) - 4 + plen, .opcode = 0,
        .status = status, .payload_len = plen,
    };
    if (write_all(fd, &h, sizeof(h)) < 0) return -1;
    if (plen && write_all(fd, payload, plen) < 0) return -1;
    return 0;
}

/* ====================================================================== */
/*  Op handlers — same shape as uffs_core_persist, but storage calls now */
/*  go through the journaled path.                                       */
/* ====================================================================== */

static void fill_attr(int ino, uffs_attr_t *a) {
    uint32_t tb = S_IFREG;
    if (itab[ino].type == T_DIR) tb = S_IFDIR;
    else if (itab[ino].type == T_LNK) tb = S_IFLNK;
    a->mode = itab[ino].mode | tb;
    a->nlink = itab[ino].nlink;
    a->uid = itab[ino].uid; a->gid = itab[ino].gid;
    a->size = itab[ino].size;
    a->atime = itab[ino].atime; a->mtime = itab[ino].mtime; a->ctime = itab[ino].ctime;
    a->ino = ino;
}

#define LOCK()   pthread_mutex_lock(&fs_lock)
#define UNLOCK() pthread_mutex_unlock(&fs_lock)

static int h_getattr(int fd, const char *path) {
    LOCK();
    int ino = path_lookup(path, NULL, NULL);
    if (ino < 0) { UNLOCK(); return send_reply(fd, ino, NULL, 0); }
    uffs_attr_t a; fill_attr(ino, &a);
    UNLOCK();
    return send_reply(fd, 0, &a, sizeof(a));
}

static int h_readdir(int fd, const char *path) {
    LOCK();
    int ino = path_lookup(path, NULL, NULL);
    if (ino < 0) { UNLOCK(); return send_reply(fd, ino, NULL, 0); }
    if (itab[ino].type != T_DIR) { UNLOCK(); return send_reply(fd, -ENOTDIR, NULL, 0); }
    char buf[16384]; size_t pos = 0;
    uffs_inode_t *dir = &itab[ino];
    uint32_t nb = (dir->size + UFFS_BLOCK_SIZE - 1) / UFFS_BLOCK_SIZE;
    for (uint32_t lbn = 0; lbn < nb && pos < sizeof(buf) - 1; lbn++) {
        int b = inode_bmap(dir, lbn, 0);
        if (b <= 0) continue;
        uffs_dirent_t *de = block_ptr(b);
        for (uint32_t i = 0; i < UFFS_DIRENTS_PER_BLOCK; i++) {
            if (!de[i].ino) continue;
            size_t L = strlen(de[i].name) + 1;
            if (pos + L + 1 > sizeof(buf)) break;
            memcpy(buf + pos, de[i].name, L);
            pos += L;
        }
    }
    buf[pos++] = 0;
    UNLOCK();
    return send_reply(fd, 0, buf, pos);
}

static int h_create_or_mkdir(int fd, const char *path, uint32_t mode, int type) {
    LOCK();
    int parent; char leaf[UFFS_MAX_NAME + 1];
    int ino = path_lookup(path, &parent, leaf);
    if (ino >= 0)        { UNLOCK(); return send_reply(fd, -EEXIST, NULL, 0); }
    if (ino != -ENOENT)  { UNLOCK(); return send_reply(fd, ino,     NULL, 0); }
    if (parent < 0)      { UNLOCK(); return send_reply(fd, -EINVAL, NULL, 0); }

    txn_begin();
    int ni = alloc_inode();
    if (ni < 0) { g_txn.active = 0; UNLOCK(); return send_reply(fd, ni, NULL, 0); }
    itab[ni].type = type; itab[ni].mode = mode & 0777;
    itab[ni].nlink = (type == T_DIR) ? 2 : 1;
    txn_touch_inode(ni);
    int rc = dir_add(parent, leaf, ni);
    if (rc < 0) { free_inode(ni); txn_commit(); UNLOCK(); return send_reply(fd, rc, NULL, 0); }
    int c = txn_commit();
    UNLOCK();
    return send_reply(fd, c < 0 ? c : 0, NULL, 0);
}

static int h_unlink(int fd, const char *path) {
    LOCK();
    int parent; char leaf[UFFS_MAX_NAME + 1];
    int ino = path_lookup(path, &parent, leaf);
    if (ino < 0)               { UNLOCK(); return send_reply(fd, ino, NULL, 0); }
    if (itab[ino].type == T_DIR) { UNLOCK(); return send_reply(fd, -EISDIR, NULL, 0); }
    txn_begin();
    dir_remove(parent, leaf);
    if (--itab[ino].nlink == 0) free_inode(ino);
    else txn_touch_inode(ino);
    int c = txn_commit();
    UNLOCK();
    return send_reply(fd, c < 0 ? c : 0, NULL, 0);
}

static int h_rmdir(int fd, const char *path) {
    LOCK();
    int parent; char leaf[UFFS_MAX_NAME + 1];
    int ino = path_lookup(path, &parent, leaf);
    if (ino < 0)                  { UNLOCK(); return send_reply(fd, ino, NULL, 0); }
    if (itab[ino].type != T_DIR)  { UNLOCK(); return send_reply(fd, -ENOTDIR, NULL, 0); }
    if (!dir_is_empty(&itab[ino])){ UNLOCK(); return send_reply(fd, -ENOTEMPTY, NULL, 0); }
    txn_begin();
    dir_remove(parent, leaf);
    free_inode(ino);
    int c = txn_commit();
    UNLOCK();
    return send_reply(fd, c < 0 ? c : 0, NULL, 0);
}

static int h_read(int fd, const char *path, uint64_t off, uint32_t size) {
    LOCK();
    int ino = path_lookup(path, NULL, NULL);
    if (ino < 0) { UNLOCK(); return send_reply(fd, ino, NULL, 0); }
    uffs_inode_t *n = &itab[ino];
    if (off >= n->size) { UNLOCK(); return send_reply(fd, 0, NULL, 0); }
    uint32_t want = size;
    if (off + want > n->size) want = n->size - off;
    char *tmp = malloc(want);
    if (!tmp) { UNLOCK(); return send_reply(fd, -ENOMEM, NULL, 0); }
    file_read(n, tmp, want, off);
    n->atime = time(NULL);  /* deliberately not journaled */
    UNLOCK();
    int rc = send_reply(fd, want, tmp, want);
    free(tmp);
    return rc;
}

static int h_write(int fd, const char *path, uint64_t off,
                   const char *data, uint32_t size) {
    LOCK();
    int ino = path_lookup(path, NULL, NULL);
    if (ino < 0) { UNLOCK(); return send_reply(fd, ino, NULL, 0); }
    txn_begin();
    int n = file_write(&itab[ino], data, size, off);
    if (n < 0) { txn_commit(); UNLOCK(); return send_reply(fd, n, NULL, 0); }
    int c = txn_commit();
    UNLOCK();
    return send_reply(fd, c < 0 ? c : n, NULL, 0);
}

static int h_truncate(int fd, const char *path, uint64_t size) {
    LOCK();
    int ino = path_lookup(path, NULL, NULL);
    if (ino < 0) { UNLOCK(); return send_reply(fd, ino, NULL, 0); }
    txn_begin();
    int rc = file_truncate(&itab[ino], size);
    int c  = txn_commit();
    UNLOCK();
    return send_reply(fd, rc < 0 ? rc : c, NULL, 0);
}

static int h_rename(int fd, const char *from, const char *to) {
    LOCK();
    int sp; char sl[UFFS_MAX_NAME + 1];
    int src = path_lookup(from, &sp, sl);
    if (src < 0) { UNLOCK(); return send_reply(fd, src, NULL, 0); }
    int dp; char dl[UFFS_MAX_NAME + 1];
    int dst = path_lookup(to, &dp, dl);
    if (dst == src) { UNLOCK(); return send_reply(fd, 0, NULL, 0); }
    txn_begin();
    if (dst >= 0) {
        if (itab[dst].type == T_DIR) {
            if (itab[src].type != T_DIR) { txn_commit(); UNLOCK(); return send_reply(fd, -EISDIR, NULL, 0); }
            if (!dir_is_empty(&itab[dst])) { txn_commit(); UNLOCK(); return send_reply(fd, -ENOTEMPTY, NULL, 0); }
        } else if (itab[src].type == T_DIR) {
            txn_commit(); UNLOCK(); return send_reply(fd, -ENOTDIR, NULL, 0);
        }
        dir_remove(dp, dl);
        if (--itab[dst].nlink == 0) free_inode(dst);
    } else if (dst != -ENOENT) {
        txn_commit(); UNLOCK(); return send_reply(fd, dst, NULL, 0);
    }
    dir_remove(sp, sl);
    int rc = dir_add(dp, dl, src);
    int c  = txn_commit();
    UNLOCK();
    return send_reply(fd, rc < 0 ? rc : c, NULL, 0);
}

/* ---- dispatcher (identical to uffs_core_persist) ------------------- */

static int serve_one(int fd) {
    uffs_frame_hdr_t h;
    if (read_all(fd, &h, sizeof(h)) < 0) return -1;
    if (h.payload_len > UFFS_MAX_IO + UFFS_MAX_PATH + 64) return -1;

    char *payload = NULL;
    if (h.payload_len) {
        payload = malloc(h.payload_len + 1);
        if (!payload) return -1;
        if (read_all(fd, payload, h.payload_len) < 0) { free(payload); return -1; }
        payload[h.payload_len] = 0;
    }

    int rc = 0;
    switch (h.opcode) {
    case OP_GETATTR: rc = h_getattr(fd, payload); break;
    case OP_READDIR: rc = h_readdir(fd, payload); break;
    case OP_CREATE: {
        uffs_create_req_t *r = (uffs_create_req_t *)payload;
        rc = h_create_or_mkdir(fd, payload + sizeof(*r), r->mode, T_REG);
        break;
    }
    case OP_MKDIR: {
        uffs_create_req_t *r = (uffs_create_req_t *)payload;
        rc = h_create_or_mkdir(fd, payload + sizeof(*r), r->mode, T_DIR);
        break;
    }
    case OP_UNLINK: rc = h_unlink(fd, payload); break;
    case OP_RMDIR:  rc = h_rmdir(fd, payload); break;
    case OP_READ: {
        uffs_read_req_t *r = (uffs_read_req_t *)payload;
        rc = h_read(fd, payload + sizeof(*r), r->offset, r->size);
        break;
    }
    case OP_WRITE: {
        uffs_write_req_t *r = (uffs_write_req_t *)payload;
        const char *path = payload + sizeof(*r);
        size_t plen = strlen(path) + 1;
        rc = h_write(fd, path, r->offset, path + plen, r->size);
        break;
    }
    case OP_TRUNCATE: {
        uffs_truncate_req_t *r = (uffs_truncate_req_t *)payload;
        rc = h_truncate(fd, payload + sizeof(*r), r->size);
        break;
    }
    case OP_RENAME: {
        const char *from = payload;
        const char *to   = payload + strlen(from) + 1;
        rc = h_rename(fd, from, to);
        break;
    }
    default:
        rc = send_reply(fd, -ENOSYS, NULL, 0);
        break;
    }
    free(payload);
    /* Any inodes touched by the just-completed handler get their
     * invalidations pushed to all connected nodes now (outside fs_lock). */
    do_pending_broadcasts();
    return rc;
}

static void *conn_thread(void *arg) {
    int fd = (int)(intptr_t)arg;

    /* Peek at the first frame. If it's opcode=0 with no payload, this
     * connection is a push channel — we just register it and block on
     * a read so we notice when the peer disconnects. */
    uffs_frame_hdr_t first;
    if (read_all(fd, &first, sizeof(first)) < 0) {
        fprintf(stderr, "core_full: short read on hello (fd %d)\n", fd);
        close(fd);
        return NULL;
    }

    if (first.opcode == 0 && first.payload_len == 0) {
        /* push channel */
        push_register(fd);
        char dummy;
        /* read() returns 0 on graceful close; we just sit and wait. */
        while (read(fd, &dummy, 1) > 0) { /* should not receive anything */ }
        push_unregister(fd);
        fprintf(stderr, "core_full: push channel disconnected (fd %d)\n", fd);
        close(fd);
        return NULL;
    }

    /* Normal RPC connection. We've already consumed the header; replay
     * it by handling this one ourselves, then loop. */
    fprintf(stderr, "core_full: RPC node connected (fd %d)\n", fd);

    /* dispatch the frame we already read */
    char *payload = NULL;
    if (first.payload_len) {
        if (first.payload_len > UFFS_MAX_IO + UFFS_MAX_PATH + 64) {
            close(fd); return NULL;
        }
        payload = malloc(first.payload_len + 1);
        if (!payload) { close(fd); return NULL; }
        if (read_all(fd, payload, first.payload_len) < 0) {
            free(payload); close(fd); return NULL;
        }
        payload[first.payload_len] = 0;
    }
    /* Tiny duplicated dispatch — mirror of serve_one's switch. We could
     * factor this out, but the duplication is short and clearer. */
    int rc = 0;
    switch (first.opcode) {
    case OP_GETATTR: rc = h_getattr(fd, payload); break;
    case OP_READDIR: rc = h_readdir(fd, payload); break;
    case OP_CREATE: {
        uffs_create_req_t *r = (uffs_create_req_t *)payload;
        rc = h_create_or_mkdir(fd, payload + sizeof(*r), r->mode, T_REG);
        break;
    }
    case OP_MKDIR: {
        uffs_create_req_t *r = (uffs_create_req_t *)payload;
        rc = h_create_or_mkdir(fd, payload + sizeof(*r), r->mode, T_DIR);
        break;
    }
    case OP_UNLINK: rc = h_unlink(fd, payload); break;
    case OP_RMDIR:  rc = h_rmdir(fd, payload); break;
    case OP_READ: {
        uffs_read_req_t *r = (uffs_read_req_t *)payload;
        rc = h_read(fd, payload + sizeof(*r), r->offset, r->size);
        break;
    }
    case OP_WRITE: {
        uffs_write_req_t *r = (uffs_write_req_t *)payload;
        const char *path = payload + sizeof(*r);
        size_t plen = strlen(path) + 1;
        rc = h_write(fd, path, r->offset, path + plen, r->size);
        break;
    }
    case OP_TRUNCATE: {
        uffs_truncate_req_t *r = (uffs_truncate_req_t *)payload;
        rc = h_truncate(fd, payload + sizeof(*r), r->size);
        break;
    }
    case OP_RENAME: {
        const char *from = payload;
        const char *to = payload + strlen(from) + 1;
        rc = h_rename(fd, from, to);
        break;
    }
    default: rc = send_reply(fd, -ENOSYS, NULL, 0); break;
    }
    free(payload);
    do_pending_broadcasts();

    if (rc == 0) while (serve_one(fd) == 0) {}
    fprintf(stderr, "core_full: RPC node disconnected (fd %d)\n", fd);
    close(fd);
    return NULL;
}

static volatile int g_running = 1;
static int g_srv = -1;
static void on_signal(int s) { (void)s; g_running = 0; if (g_srv >= 0) close(g_srv); }

int main(int argc, char *argv[]) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s BACKING_IMAGE SOCKET_PATH\n", argv[0]);
        return 1;
    }
    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT,  on_signal);
    signal(SIGTERM, on_signal);

    if (open_backing(argv[1]) < 0) return 1;

    unlink(argv[2]);
    g_srv = socket(AF_UNIX, SOCK_STREAM, 0);
    if (g_srv < 0) { perror("socket"); return 1; }
    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    strncpy(addr.sun_path, argv[2], sizeof(addr.sun_path) - 1);
    if (bind(g_srv, (struct sockaddr *)&addr, sizeof(addr)) < 0) { perror("bind"); return 1; }
    if (listen(g_srv, 8) < 0) { perror("listen"); return 1; }
    fprintf(stderr, "core_full: listening on %s\n", argv[2]);

    while (g_running) {
        int cli = accept(g_srv, NULL, NULL);
        if (cli < 0) {
            if (errno == EINTR) continue;
            if (!g_running) break;
            perror("accept"); break;
        }
        pthread_t th;
        if (pthread_create(&th, NULL, conn_thread, (void *)(intptr_t)cli) != 0) {
            perror("pthread_create"); close(cli); continue;
        }
        pthread_detach(th);
    }
    if (backing_map) msync(backing_map, backing_size, MS_SYNC);
    if (backing_fd >= 0) close(backing_fd);
    unlink(argv[2]);
    fprintf(stderr, "core_full: shutdown, image synced\n");
    return 0;
}
