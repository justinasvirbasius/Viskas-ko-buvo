/*
 * uffs_blocks.c — block-allocator variant.
 *
 * On-disk layout, in fixed-size 4 KiB blocks:
 *
 *   block 0           : superblock
 *   blocks 1..B       : block-allocation bitmap (1 bit per block)
 *   blocks B+1..I     : inode table (inodes packed, sizeof on disk)
 *   blocks I+1..END   : data blocks (allocated on demand)
 *
 * Inode shrinks to a fixed small struct: 12 direct block pointers and
 * 1 indirect pointer. Indirect block holds 1024 pointers. Max file
 * size = (12 + 1024) * 4096 = ~4 MiB.
 *
 * Build:
 *   cc -Wall -O2 -D_FILE_OFFSET_BITS=64 uffs_blocks.c \
 *      `pkg-config fuse3 --cflags --libs` -o uffs_blocks
 *
 * Run:
 *   ./uffs_blocks -f -o backing=/tmp/uffs.img /tmp/uffs
 *   # first run creates a fresh image; later runs reopen it
 *
 * Caveats (still): no journaling, MS_ASYNC durability, coarse no-locking
 * (single-threaded FUSE). Block allocator is a linear scan of the bitmap,
 * which is fine up to a few thousand blocks and a project beyond that.
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

/* ---- layout constants ------------------------------------------------- */

#define UFFS_BLOCK_SIZE   4096u
#define UFFS_MAGIC        0x55464253u   /* 'UFBS' */
#define UFFS_VERSION      2

#define UFFS_TOTAL_BLOCKS  4096u        /* 4096 * 4 KiB = 16 MiB image */
#define UFFS_MAX_INODES    512u
#define UFFS_DIRECT        12u
#define UFFS_INDIRECT_PTRS (UFFS_BLOCK_SIZE / sizeof(uint32_t)) /* 1024 */

#define UFFS_MAX_NAME      59           /* keep dir entry exactly 64 bytes */
#define UFFS_DIRENT_SIZE   64
#define UFFS_DIRENTS_PER_BLOCK (UFFS_BLOCK_SIZE / UFFS_DIRENT_SIZE)  /* 64 */

#define UFFS_MAX_FILESZ \
    ((uint64_t)(UFFS_DIRECT + UFFS_INDIRECT_PTRS) * UFFS_BLOCK_SIZE)

enum { T_REG=0, T_DIR=1, T_LNK=2 };

/* ---- on-disk structures (all fixed-width) ---------------------------- */

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t block_size;
    uint32_t total_blocks;
    uint32_t bitmap_start;     /* block # where bitmap begins (= 1) */
    uint32_t bitmap_blocks;
    uint32_t inode_table_start;
    uint32_t inode_table_blocks;
    uint32_t inode_count;
    uint32_t data_start;
    uint32_t root_inode;       /* = 1 */
    uint32_t _pad[5];
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
    uint32_t indirect;         /* block # of indirect block, 0 if none */
    uint32_t _pad[3];
} uffs_inode_t;
/* sizeof = 4*6 + 8 + 8*3 + 4*12 + 4 + 4*3 = 24+8+24+48+4+12 = 120 bytes */

typedef struct {
    uint32_t ino;              /* 0 if entry is empty */
    char     name[UFFS_MAX_NAME + 1];   /* 60 bytes including NUL */
} uffs_dirent_t;
/* sizeof = 4 + 60 = 64 */

/* Belt-and-suspenders: if these break, the on-disk layout is wrong. */
_Static_assert(sizeof(uffs_dirent_t) == UFFS_DIRENT_SIZE, "dirent must be 64 bytes");
_Static_assert(UFFS_BLOCK_SIZE % UFFS_DIRENT_SIZE == 0, "dirents must tile a block");
_Static_assert(sizeof(uffs_super_t) <= UFFS_BLOCK_SIZE, "superblock must fit in one block");

/* ---- in-memory state ------------------------------------------------- */

static int           backing_fd = -1;
static void         *backing_map;
static size_t        backing_size;
static uffs_super_t *sb;
static uint8_t      *bitmap;            /* points into the mmap */
static uffs_inode_t *itab;              /* points into the mmap */
static char         *blocks_base;       /* points at byte 0 of block 0 in mmap */

/* ---- block helpers --------------------------------------------------- */

static void *block_ptr(uint32_t b) {
    return blocks_base + (size_t)b * UFFS_BLOCK_SIZE;
}

static void sync_range(void *p, size_t len) {
    long ps = sysconf(_SC_PAGESIZE);
    uintptr_t start = (uintptr_t)p & ~(ps - 1);
    uintptr_t end   = ((uintptr_t)p + len + ps - 1) & ~(ps - 1);
    msync((void *)start, end - start, MS_ASYNC);
}

static void sync_block(uint32_t b) { sync_range(block_ptr(b), UFFS_BLOCK_SIZE); }

static int  bit_get(uint32_t b) { return (bitmap[b >> 3] >> (b & 7)) & 1; }
static void bit_set(uint32_t b) { bitmap[b >> 3] |=  (1u << (b & 7)); }
static void bit_clr(uint32_t b) { bitmap[b >> 3] &= ~(1u << (b & 7)); }

static int alloc_block(void) {
    for (uint32_t b = sb->data_start; b < sb->total_blocks; b++) {
        if (!bit_get(b)) {
            bit_set(b);
            memset(block_ptr(b), 0, UFFS_BLOCK_SIZE);
            sync_block(b);
            sync_range(bitmap + (b >> 3), 1);
            return b;
        }
    }
    return -ENOSPC;
}

static void free_block(uint32_t b) {
    if (b < sb->data_start || b >= sb->total_blocks) return;
    bit_clr(b);
    sync_range(bitmap + (b >> 3), 1);
}

/* ---- inode helpers --------------------------------------------------- */

static int alloc_inode(void) {
    for (uint32_t i = 1; i < sb->inode_count; i++) {
        if (!itab[i].in_use) {
            memset(&itab[i], 0, sizeof(itab[i]));
            itab[i].in_use = 1;
            itab[i].atime = itab[i].mtime = itab[i].ctime = time(NULL);
            sync_range(&itab[i], sizeof(itab[i]));
            return i;
        }
    }
    return -ENOSPC;
}

/* Release every data block referenced by an inode. */
static void inode_release_blocks(uffs_inode_t *n) {
    for (uint32_t i = 0; i < UFFS_DIRECT; i++) {
        if (n->direct[i]) { free_block(n->direct[i]); n->direct[i] = 0; }
    }
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
    sync_range(&itab[ino], sizeof(itab[ino]));
}

/* Return the data block number for logical block `lbn` of an inode.
 * If `allocate` is set, allocate it (and the indirect block) when missing. */
static int inode_bmap(uffs_inode_t *n, uint32_t lbn, int allocate) {
    if (lbn < UFFS_DIRECT) {
        if (n->direct[lbn] == 0 && allocate) {
            int b = alloc_block();
            if (b < 0) return b;
            n->direct[lbn] = b;
            sync_range(n, sizeof(*n));
        }
        return n->direct[lbn];
    }
    uint32_t idx = lbn - UFFS_DIRECT;
    if (idx >= UFFS_INDIRECT_PTRS) return -EFBIG;
    if (n->indirect == 0) {
        if (!allocate) return 0;
        int b = alloc_block();
        if (b < 0) return b;
        n->indirect = b;
        sync_range(n, sizeof(*n));
    }
    uint32_t *ind = block_ptr(n->indirect);
    if (ind[idx] == 0 && allocate) {
        int b = alloc_block();
        if (b < 0) return b;
        ind[idx] = b;
        sync_block(n->indirect);
    }
    return ind[idx];
}

/* ---- directory entries ----------------------------------------------- */

/* Directories are sequences of fixed-size dirents stored in the data
 * blocks of the directory inode. An entry with ino==0 is free. */

static int dir_lookup(uffs_inode_t *dir, const char *name, uint32_t *slot_out) {
    uint32_t nblocks = (dir->size + UFFS_BLOCK_SIZE - 1) / UFFS_BLOCK_SIZE;
    for (uint32_t lbn = 0; lbn < nblocks; lbn++) {
        int b = inode_bmap(dir, lbn, 0);
        if (b <= 0) continue;
        uffs_dirent_t *de = block_ptr(b);
        for (uint32_t i = 0; i < UFFS_DIRENTS_PER_BLOCK; i++) {
            if (de[i].ino && strcmp(de[i].name, name) == 0) {
                if (slot_out) *slot_out = lbn * UFFS_DIRENTS_PER_BLOCK + i;
                return de[i].ino;
            }
        }
    }
    return -ENOENT;
}

static int dir_add(int dir_ino, const char *name, int child) {
    uffs_inode_t *dir = &itab[dir_ino];
    uint32_t nblocks = (dir->size + UFFS_BLOCK_SIZE - 1) / UFFS_BLOCK_SIZE;

    /* Look for a free slot in existing blocks. */
    for (uint32_t lbn = 0; lbn < nblocks; lbn++) {
        int b = inode_bmap(dir, lbn, 0);
        if (b <= 0) continue;
        uffs_dirent_t *de = block_ptr(b);
        for (uint32_t i = 0; i < UFFS_DIRENTS_PER_BLOCK; i++) {
            if (!de[i].ino) {
                de[i].ino = child;
                strncpy(de[i].name, name, UFFS_MAX_NAME);
                de[i].name[UFFS_MAX_NAME] = 0;
                sync_block(b);
                dir->mtime = dir->ctime = time(NULL);
                sync_range(dir, sizeof(*dir));
                return 0;
            }
        }
    }

    /* No free slot — extend the directory by one block. */
    uint32_t new_lbn = nblocks;
    int b = inode_bmap(dir, new_lbn, 1);
    if (b < 0) return b;
    uffs_dirent_t *de = block_ptr(b);
    memset(de, 0, UFFS_BLOCK_SIZE);
    de[0].ino = child;
    strncpy(de[0].name, name, UFFS_MAX_NAME);
    de[0].name[UFFS_MAX_NAME] = 0;
    sync_block(b);
    dir->size = (uint64_t)(new_lbn + 1) * UFFS_BLOCK_SIZE;
    dir->mtime = dir->ctime = time(NULL);
    sync_range(dir, sizeof(*dir));
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
                sync_block(b);
                dir->mtime = dir->ctime = time(NULL);
                sync_range(dir, sizeof(*dir));
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

/* ---- path lookup ----------------------------------------------------- */

static int path_lookup(const char *path, int *parent_out, char *leaf_out) {
    if (path[0] != '/') return -EINVAL;
    int cur = sb->root_inode;
    int parent = -1;
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
        parent = cur;
        strncpy(leaf, comp, sizeof(leaf));
        if (next < 0) {
            if (slash == NULL || *(slash + 1) == 0) {
                if (parent_out) *parent_out = parent;
                if (leaf_out)   strcpy(leaf_out, leaf);
                return -ENOENT;
            }
            return -ENOENT;
        }
        cur = next;
        if (!slash) break;
        p = slash + 1;
    }
    if (parent_out) *parent_out = parent;
    if (leaf_out)   strcpy(leaf_out, leaf);
    return cur;
}

/* ---- file I/O via block map ----------------------------------------- */

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
        else       memset(buf + done, 0, chunk);   /* sparse hole reads as 0 */
        done += chunk;
    }
    return size;
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
        sync_block(b);
        done += chunk;
    }
    if ((uint64_t)off + size > n->size) n->size = off + size;
    n->mtime = n->ctime = time(NULL);
    sync_range(n, sizeof(*n));
    return size;
}

static int file_truncate(uffs_inode_t *n, uint64_t newsize) {
    if (newsize > UFFS_MAX_FILESZ) return -EFBIG;
    if (newsize < n->size) {
        /* free blocks beyond the new end */
        uint32_t first_free = (newsize + UFFS_BLOCK_SIZE - 1) / UFFS_BLOCK_SIZE;
        uint32_t cur_blocks = (n->size + UFFS_BLOCK_SIZE - 1) / UFFS_BLOCK_SIZE;
        for (uint32_t lbn = first_free; lbn < cur_blocks; lbn++) {
            if (lbn < UFFS_DIRECT) {
                if (n->direct[lbn]) { free_block(n->direct[lbn]); n->direct[lbn] = 0; }
            } else if (n->indirect) {
                uint32_t *ind = block_ptr(n->indirect);
                uint32_t idx = lbn - UFFS_DIRECT;
                if (ind[idx]) { free_block(ind[idx]); ind[idx] = 0; }
            }
        }
        /* drop the indirect block itself if everything in it is gone */
        if (first_free <= UFFS_DIRECT && n->indirect) {
            free_block(n->indirect);
            n->indirect = 0;
        }
    }
    n->size = newsize;
    n->mtime = n->ctime = time(NULL);
    sync_range(n, sizeof(*n));
    return 0;
}

/* ---- backing-file open / format ------------------------------------- */

static int format_fresh(void) {
    /* Layout math */
    uint32_t bitmap_bytes = (UFFS_TOTAL_BLOCKS + 7) / 8;
    uint32_t bitmap_blocks = (bitmap_bytes + UFFS_BLOCK_SIZE - 1) / UFFS_BLOCK_SIZE;
    uint32_t itab_bytes = UFFS_MAX_INODES * sizeof(uffs_inode_t);
    uint32_t itab_blocks = (itab_bytes + UFFS_BLOCK_SIZE - 1) / UFFS_BLOCK_SIZE;

    uint32_t bitmap_start = 1;
    uint32_t itab_start   = bitmap_start + bitmap_blocks;
    uint32_t data_start   = itab_start + itab_blocks;

    if (data_start >= UFFS_TOTAL_BLOCKS) {
        fprintf(stderr, "uffs_blocks: image too small for metadata\n");
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
    sb->data_start   = data_start;
    sb->root_inode   = 1;

    bitmap = (uint8_t *)block_ptr(bitmap_start);
    itab   = (uffs_inode_t *)block_ptr(itab_start);

    /* Mark superblock, bitmap, and inode-table blocks as used. */
    for (uint32_t b = 0; b < data_start; b++) bit_set(b);

    /* Root inode. */
    memset(&itab[1], 0, sizeof(itab[1]));
    itab[1].in_use = 1;
    itab[1].type   = T_DIR;
    itab[1].mode   = 0755;
    itab[1].nlink  = 2;
    itab[1].uid    = getuid();
    itab[1].gid    = getgid();
    itab[1].atime  = itab[1].mtime = itab[1].ctime = time(NULL);

    msync(backing_map, backing_size, MS_SYNC);
    fprintf(stderr, "uffs_blocks: formatted (data starts at block %u, "
                    "%u blocks total, %u inodes)\n",
            data_start, UFFS_TOTAL_BLOCKS, UFFS_MAX_INODES);
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
            fprintf(stderr, "uffs_blocks: bad/incompatible image\n");
            return -1;
        }
        bitmap = (uint8_t *)block_ptr(sb->bitmap_start);
        itab   = (uffs_inode_t *)block_ptr(sb->inode_table_start);
        fprintf(stderr, "uffs_blocks: reopened image (%u blocks, %u inodes)\n",
                sb->total_blocks, sb->inode_count);
    }
    return 0;
}

/* ====================================================================== */
/*  FUSE ops                                                              */
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
    st->st_ino     = ino;
    st->st_mode    = n->mode | tb;
    st->st_nlink   = n->nlink;
    st->st_uid     = n->uid;
    st->st_gid     = n->gid;
    st->st_size    = n->size;
    st->st_atime   = n->atime;
    st->st_mtime   = n->mtime;
    st->st_ctime   = n->ctime;
    st->st_blksize = UFFS_BLOCK_SIZE;
    /* st_blocks counts 512-byte units; approximate from file size */
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

    int ni = alloc_inode();
    if (ni < 0) return ni;
    uffs_inode_t *n = &itab[ni];
    n->type  = type;
    n->mode  = mode & 0777;
    n->uid   = fuse_get_context()->uid;
    n->gid   = fuse_get_context()->gid;
    n->nlink = (type == T_DIR) ? 2 : 1;
    sync_range(n, sizeof(*n));

    int rc = dir_add(parent, leaf, ni);
    if (rc < 0) { free_inode(ni); return rc; }
    return ni;
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
    dir_remove(parent, leaf);
    if (--itab[ino].nlink == 0) free_inode(ino);
    else sync_range(&itab[ino], sizeof(itab[ino]));
    return 0;
}

static int op_rmdir(const char *path) {
    int parent; char leaf[UFFS_MAX_NAME + 1];
    int ino = path_lookup(path, &parent, leaf);
    if (ino < 0) return ino;
    if (itab[ino].type != T_DIR) return -ENOTDIR;
    if (!dir_is_empty(&itab[ino])) return -ENOTEMPTY;
    dir_remove(parent, leaf);
    free_inode(ino);
    return 0;
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
    itab[ino].atime = time(NULL);
    sync_range(&itab[ino], sizeof(itab[ino]));
    return n;
}

static int op_write(const char *path, const char *buf, size_t size, off_t off,
                    struct fuse_file_info *fi) {
    (void)fi;
    int ino = path_lookup(path, NULL, NULL);
    if (ino < 0) return ino;
    return file_write(&itab[ino], buf, size, off);
}

static int op_truncate(const char *path, off_t size, struct fuse_file_info *fi) {
    (void)fi;
    int ino = path_lookup(path, NULL, NULL);
    if (ino < 0) return ino;
    return file_truncate(&itab[ino], size);
}

static int op_chmod(const char *path, mode_t mode, struct fuse_file_info *fi) {
    (void)fi;
    int ino = path_lookup(path, NULL, NULL);
    if (ino < 0) return ino;
    itab[ino].mode = mode & 0777;
    itab[ino].ctime = time(NULL);
    sync_range(&itab[ino], sizeof(itab[ino]));
    return 0;
}

static int op_chown(const char *path, uid_t uid, gid_t gid, struct fuse_file_info *fi) {
    (void)fi;
    int ino = path_lookup(path, NULL, NULL);
    if (ino < 0) return ino;
    if (uid != (uid_t)-1) itab[ino].uid = uid;
    if (gid != (gid_t)-1) itab[ino].gid = gid;
    itab[ino].ctime = time(NULL);
    sync_range(&itab[ino], sizeof(itab[ino]));
    return 0;
}

static int op_utimens(const char *path, const struct timespec tv[2],
                      struct fuse_file_info *fi) {
    (void)fi;
    int ino = path_lookup(path, NULL, NULL);
    if (ino < 0) return ino;
    itab[ino].atime = tv[0].tv_sec;
    itab[ino].mtime = tv[1].tv_sec;
    sync_range(&itab[ino], sizeof(itab[ino]));
    return 0;
}

static int op_rename(const char *from, const char *to, unsigned int flags) {
    if (flags) return -EINVAL;
    int sp; char sl[UFFS_MAX_NAME + 1];
    int src = path_lookup(from, &sp, sl);
    if (src < 0) return src;
    int dp; char dl[UFFS_MAX_NAME + 1];
    int dst = path_lookup(to, &dp, dl);
    if (dst == src) return 0;
    if (dst >= 0) {
        if (itab[dst].type == T_DIR) {
            if (itab[src].type != T_DIR) return -EISDIR;
            if (!dir_is_empty(&itab[dst])) return -ENOTEMPTY;
        } else if (itab[src].type == T_DIR) {
            return -ENOTDIR;
        }
        dir_remove(dp, dl);
        if (--itab[dst].nlink == 0) free_inode(dst);
    } else if (dst != -ENOENT) return dst;
    dir_remove(sp, sl);
    return dir_add(dp, dl, src);
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
    st->f_bsize   = UFFS_BLOCK_SIZE;
    st->f_frsize  = UFFS_BLOCK_SIZE;
    st->f_blocks  = data_blocks;
    st->f_bfree   = data_blocks - used_blocks;
    st->f_bavail  = st->f_bfree;
    st->f_files   = sb->inode_count;
    st->f_ffree   = sb->inode_count - used_inodes;
    st->f_namemax = UFFS_MAX_NAME;
    return 0;
}

static void *op_init(struct fuse_conn_info *conn, struct fuse_config *cfg) {
    (void)conn; (void)cfg;
    return NULL;
}

static void op_destroy(void *priv) {
    (void)priv;
    if (backing_map) msync(backing_map, backing_size, MS_SYNC);
    if (backing_fd >= 0) close(backing_fd);
    fprintf(stderr, "uffs_blocks: synced and closed.\n");
}

static const struct fuse_operations ops = {
    .init     = op_init,
    .destroy  = op_destroy,
    .getattr  = op_getattr,
    .readdir  = op_readdir,
    .create   = op_create,
    .mkdir    = op_mkdir,
    .unlink   = op_unlink,
    .rmdir    = op_rmdir,
    .open     = op_open,
    .read     = op_read,
    .write    = op_write,
    .truncate = op_truncate,
    .chmod    = op_chmod,
    .chown    = op_chown,
    .utimens  = op_utimens,
    .rename   = op_rename,
    .statfs   = op_statfs,
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
        fprintf(stderr, "usage: uffs_blocks -o backing=PATH MOUNTPOINT\n");
        return 1;
    }
    if (open_backing(opts.backing) < 0) return 1;
    return fuse_main(args.argc, args.argv, &ops, NULL);
}
