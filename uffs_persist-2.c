/*
 * uffs_persist.c — Universal Fast File System, persistent variant
 *
 * Same FUSE-mounted shape as uffs.c, but state survives unmount: the entire
 * inode table is held in an mmap'd backing file. msync() on writes flushes
 * to disk. The on-disk layout is:
 *
 *   [ superblock | inode 0 | inode 1 | ... | inode N-1 ]
 *
 * File data is stored *inside* each inode (fixed slot), which keeps the
 * layout trivially flat — no block allocator yet. This caps file size at
 * UFFS_FILE_SLOT but it's a clean place to bolt on extents/blocks later.
 *
 * Build:
 *   cc -Wall -O2 uffs_persist.c `pkg-config fuse3 --cflags --libs` -o uffs_persist
 *
 * Run:
 *   mkdir /tmp/uffs
 *   ./uffs_persist -f -o backing=/tmp/uffs.img /tmp/uffs
 *   # First run creates the image; subsequent runs reopen and resume.
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
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/statvfs.h>

#define UFFS_MAGIC        0x55464653u   /* 'UFFS' */
#define UFFS_VERSION      1
#define UFFS_MAX_INODES   1024
#define UFFS_MAX_NAME     255
#define UFFS_MAX_CHILDREN 128
#define UFFS_BLOCK_SIZE   4096
#define UFFS_FILE_SLOT    (256 * 1024)  /* 256 KiB per file (fits in inode) */

enum uffs_type { UFFS_REG = 0, UFFS_DIR = 1, UFFS_LNK = 2 };

/* On-disk inode. All fields are fixed-width / fixed-layout so the struct
 * can be mmap'd directly. No pointers stored here. */
typedef struct {
    uint32_t in_use;
    uint32_t type;
    uint32_t mode;
    uint32_t uid;
    uint32_t gid;
    uint32_t nlink;
    uint64_t size;
    int64_t  atime, mtime, ctime;

    struct {
        char     name[UFFS_MAX_NAME + 1];
        uint32_t ino;
    } children[UFFS_MAX_CHILDREN];
    uint32_t n_children;

    /* file payload lives inline; symlink target reuses the same slot */
    char     data[UFFS_FILE_SLOT];
} uffs_inode_t;

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t max_inodes;
    uint32_t file_slot;
} uffs_super_t;

/* mmap layout: super at offset 0, inode N at offset sizeof(super) + N*sizeof(inode) */
static uffs_super_t *sb_super;
static uffs_inode_t *sb;       /* points at inode 0 */
static int           backing_fd = -1;
static size_t        backing_size;
static char         *backing_path;

/* ---- helpers ---------------------------------------------------------- */

static void sync_inode(int ino) {
    /* msync just the page range covering this inode. Cheap durability. */
    char *base = (char *)&sb[ino];
    size_t len = sizeof(uffs_inode_t);
    /* round to page boundaries */
    long ps = sysconf(_SC_PAGESIZE);
    uintptr_t start = (uintptr_t)base & ~(ps - 1);
    uintptr_t end   = ((uintptr_t)base + len + ps - 1) & ~(ps - 1);
    msync((void *)start, end - start, MS_ASYNC);
}

static int alloc_inode(void) {
    for (uint32_t i = 1; i < sb_super->max_inodes; i++) {
        if (!sb[i].in_use) {
            memset(&sb[i], 0, sizeof(sb[i]));
            sb[i].in_use = 1;
            sb[i].atime = sb[i].mtime = sb[i].ctime = time(NULL);
            return i;
        }
    }
    return -ENOSPC;
}

static void free_inode(int ino) {
    if (ino <= 0 || (uint32_t)ino >= sb_super->max_inodes) return;
    memset(&sb[ino], 0, sizeof(sb[ino]));
    sync_inode(ino);
}

static int path_lookup(const char *path, int *parent_out, char *leaf_out) {
    if (path[0] != '/') return -EINVAL;
    int cur = 1, parent = -1;
    char leaf[UFFS_MAX_NAME + 1] = "";

    const char *p = path + 1;
    while (*p) {
        const char *slash = strchr(p, '/');
        size_t len = slash ? (size_t)(slash - p) : strlen(p);
        if (len == 0) { p++; continue; }
        if (len > UFFS_MAX_NAME) return -ENAMETOOLONG;

        char comp[UFFS_MAX_NAME + 1];
        memcpy(comp, p, len); comp[len] = 0;

        if (sb[cur].type != UFFS_DIR) return -ENOTDIR;

        int next = -1;
        for (uint32_t i = 0; i < sb[cur].n_children; i++)
            if (strcmp(sb[cur].children[i].name, comp) == 0) {
                next = sb[cur].children[i].ino; break;
            }
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

static int dir_add(int parent, const char *name, int child_ino) {
    if (sb[parent].n_children >= UFFS_MAX_CHILDREN) return -ENOSPC;
    uint32_t n = sb[parent].n_children++;
    strncpy(sb[parent].children[n].name, name, UFFS_MAX_NAME);
    sb[parent].children[n].name[UFFS_MAX_NAME] = 0;
    sb[parent].children[n].ino = child_ino;
    sb[parent].mtime = sb[parent].ctime = time(NULL);
    sync_inode(parent);
    return 0;
}

static int dir_remove(int parent, const char *name) {
    for (uint32_t i = 0; i < sb[parent].n_children; i++) {
        if (strcmp(sb[parent].children[i].name, name) == 0) {
            sb[parent].children[i] = sb[parent].children[--sb[parent].n_children];
            sb[parent].mtime = sb[parent].ctime = time(NULL);
            sync_inode(parent);
            return 0;
        }
    }
    return -ENOENT;
}

/* ---- FUSE ops (same shape as uffs.c) ---------------------------------- */

static int uffs_getattr(const char *path, struct stat *st,
                        struct fuse_file_info *fi) {
    (void)fi;
    int ino = path_lookup(path, NULL, NULL);
    if (ino < 0) return ino;
    uffs_inode_t *n = &sb[ino];
    memset(st, 0, sizeof(*st));
    st->st_ino = ino;
    mode_t tb = S_IFREG;
    if (n->type == UFFS_DIR) tb = S_IFDIR;
    else if (n->type == UFFS_LNK) tb = S_IFLNK;
    st->st_mode  = n->mode | tb;
    st->st_nlink = n->nlink;
    st->st_uid   = n->uid;
    st->st_gid   = n->gid;
    st->st_size  = n->size;
    st->st_atime = n->atime;
    st->st_mtime = n->mtime;
    st->st_ctime = n->ctime;
    st->st_blksize = UFFS_BLOCK_SIZE;
    return 0;
}

static int uffs_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
                        off_t off, struct fuse_file_info *fi,
                        enum fuse_readdir_flags fl) {
    (void)off; (void)fi; (void)fl;
    int ino = path_lookup(path, NULL, NULL);
    if (ino < 0) return ino;
    if (sb[ino].type != UFFS_DIR) return -ENOTDIR;
    filler(buf, ".", NULL, 0, 0);
    filler(buf, "..", NULL, 0, 0);
    for (uint32_t i = 0; i < sb[ino].n_children; i++)
        filler(buf, sb[ino].children[i].name, NULL, 0, 0);
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
    uffs_inode_t *n = &sb[ni];
    n->type  = type;
    n->mode  = mode & 0777;
    n->uid   = fuse_get_context()->uid;
    n->gid   = fuse_get_context()->gid;
    n->nlink = (type == UFFS_DIR) ? 2 : 1;
    sync_inode(ni);

    int rc = dir_add(parent, leaf, ni);
    if (rc < 0) { free_inode(ni); return rc; }
    return ni;
}

static int uffs_create(const char *path, mode_t mode, struct fuse_file_info *fi) {
    (void)fi; int rc = mknod_common(path, mode, UFFS_REG); return rc < 0 ? rc : 0;
}
static int uffs_mkdir(const char *path, mode_t mode) {
    int rc = mknod_common(path, mode, UFFS_DIR); return rc < 0 ? rc : 0;
}

static int uffs_unlink(const char *path) {
    int parent; char leaf[UFFS_MAX_NAME + 1];
    int ino = path_lookup(path, &parent, leaf);
    if (ino < 0) return ino;
    if (sb[ino].type == UFFS_DIR) return -EISDIR;
    dir_remove(parent, leaf);
    if (--sb[ino].nlink == 0) free_inode(ino);
    else sync_inode(ino);
    return 0;
}

static int uffs_rmdir(const char *path) {
    int parent; char leaf[UFFS_MAX_NAME + 1];
    int ino = path_lookup(path, &parent, leaf);
    if (ino < 0) return ino;
    if (sb[ino].type != UFFS_DIR) return -ENOTDIR;
    if (sb[ino].n_children > 0) return -ENOTEMPTY;
    dir_remove(parent, leaf);
    free_inode(ino);
    return 0;
}

static int uffs_open(const char *path, struct fuse_file_info *fi) {
    (void)fi;
    int ino = path_lookup(path, NULL, NULL);
    if (ino < 0) return ino;
    if (sb[ino].type == UFFS_DIR) return -EISDIR;
    return 0;
}

static int uffs_read(const char *path, char *buf, size_t size, off_t off,
                     struct fuse_file_info *fi) {
    (void)fi;
    int ino = path_lookup(path, NULL, NULL);
    if (ino < 0) return ino;
    uffs_inode_t *n = &sb[ino];
    if (off >= (off_t)n->size) return 0;
    if (off + (off_t)size > (off_t)n->size) size = n->size - off;
    memcpy(buf, n->data + off, size);
    n->atime = time(NULL);
    sync_inode(ino);
    return size;
}

static int uffs_write(const char *path, const char *buf, size_t size,
                      off_t off, struct fuse_file_info *fi) {
    (void)fi;
    int ino = path_lookup(path, NULL, NULL);
    if (ino < 0) return ino;
    uffs_inode_t *n = &sb[ino];
    if (off + size > UFFS_FILE_SLOT) return -EFBIG;
    memcpy(n->data + off, buf, size);
    if (off + size > n->size) n->size = off + size;
    n->mtime = n->ctime = time(NULL);
    sync_inode(ino);
    return size;
}

static int uffs_truncate(const char *path, off_t size, struct fuse_file_info *fi) {
    (void)fi;
    int ino = path_lookup(path, NULL, NULL);
    if (ino < 0) return ino;
    if (size > UFFS_FILE_SLOT) return -EFBIG;
    uffs_inode_t *n = &sb[ino];
    if (size > (off_t)n->size) memset(n->data + n->size, 0, size - n->size);
    n->size = size;
    n->mtime = n->ctime = time(NULL);
    sync_inode(ino);
    return 0;
}

static int uffs_chmod(const char *path, mode_t mode, struct fuse_file_info *fi) {
    (void)fi;
    int ino = path_lookup(path, NULL, NULL);
    if (ino < 0) return ino;
    sb[ino].mode = mode & 0777;
    sb[ino].ctime = time(NULL);
    sync_inode(ino);
    return 0;
}

static int uffs_chown(const char *path, uid_t uid, gid_t gid, struct fuse_file_info *fi) {
    (void)fi;
    int ino = path_lookup(path, NULL, NULL);
    if (ino < 0) return ino;
    if (uid != (uid_t)-1) sb[ino].uid = uid;
    if (gid != (gid_t)-1) sb[ino].gid = gid;
    sb[ino].ctime = time(NULL);
    sync_inode(ino);
    return 0;
}

static int uffs_utimens(const char *path, const struct timespec tv[2],
                        struct fuse_file_info *fi) {
    (void)fi;
    int ino = path_lookup(path, NULL, NULL);
    if (ino < 0) return ino;
    sb[ino].atime = tv[0].tv_sec;
    sb[ino].mtime = tv[1].tv_sec;
    sync_inode(ino);
    return 0;
}

static int uffs_symlink(const char *target, const char *path) {
    int ni = mknod_common(path, 0777, UFFS_LNK);
    if (ni < 0) return ni;
    size_t tlen = strlen(target);
    if (tlen >= UFFS_FILE_SLOT) return -ENAMETOOLONG;
    memcpy(sb[ni].data, target, tlen + 1);
    sb[ni].size = tlen;
    sync_inode(ni);
    return 0;
}

static int uffs_readlink(const char *path, char *buf, size_t size) {
    int ino = path_lookup(path, NULL, NULL);
    if (ino < 0) return ino;
    if (sb[ino].type != UFFS_LNK) return -EINVAL;
    strncpy(buf, sb[ino].data, size - 1);
    buf[size - 1] = 0;
    return 0;
}

static int uffs_link(const char *from, const char *to) {
    int src = path_lookup(from, NULL, NULL);
    if (src < 0) return src;
    if (sb[src].type == UFFS_DIR) return -EPERM;

    int parent; char leaf[UFFS_MAX_NAME + 1];
    int existing = path_lookup(to, &parent, leaf);
    if (existing >= 0)       return -EEXIST;
    if (existing != -ENOENT) return existing;
    if (parent < 0)          return -EINVAL;

    int rc = dir_add(parent, leaf, src);
    if (rc < 0) return rc;
    sb[src].nlink++;
    sb[src].ctime = time(NULL);
    sync_inode(src);
    return 0;
}

static int uffs_rename(const char *from, const char *to, unsigned int flags) {
    if (flags) return -EINVAL;
    int sparent; char sleaf[UFFS_MAX_NAME + 1];
    int src = path_lookup(from, &sparent, sleaf);
    if (src < 0) return src;

    int dparent; char dleaf[UFFS_MAX_NAME + 1];
    int dst = path_lookup(to, &dparent, dleaf);
    if (dst == src) return 0;
    if (dst >= 0) {
        if (sb[dst].type == UFFS_DIR) {
            if (sb[src].type != UFFS_DIR) return -EISDIR;
            if (sb[dst].n_children > 0)   return -ENOTEMPTY;
        } else if (sb[src].type == UFFS_DIR) {
            return -ENOTDIR;
        }
        dir_remove(dparent, dleaf);
        if (--sb[dst].nlink == 0) free_inode(dst);
    } else if (dst != -ENOENT) return dst;

    dir_remove(sparent, sleaf);
    return dir_add(dparent, dleaf, src);
}

static int uffs_statfs(const char *path, struct statvfs *st) {
    (void)path;
    uint32_t used = 0;
    for (uint32_t i = 1; i < sb_super->max_inodes; i++) if (sb[i].in_use) used++;
    memset(st, 0, sizeof(*st));
    st->f_bsize   = UFFS_BLOCK_SIZE;
    st->f_frsize  = UFFS_BLOCK_SIZE;
    st->f_blocks  = sb_super->max_inodes;
    st->f_bfree   = sb_super->max_inodes - used;
    st->f_bavail  = st->f_bfree;
    st->f_files   = sb_super->max_inodes;
    st->f_ffree   = sb_super->max_inodes - used;
    st->f_namemax = UFFS_MAX_NAME;
    return 0;
}

/* ---- backing-file management ----------------------------------------- */

static int open_backing(const char *path) {
    int fd = open(path, O_RDWR | O_CREAT, 0644);
    if (fd < 0) { perror("open backing"); return -1; }

    size_t need = sizeof(uffs_super_t) + sizeof(uffs_inode_t) * UFFS_MAX_INODES;
    struct stat st;
    fstat(fd, &st);
    int fresh = (st.st_size == 0);
    if (fresh && ftruncate(fd, need) < 0) { perror("ftruncate"); close(fd); return -1; }

    void *map = mmap(NULL, need, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) { perror("mmap"); close(fd); return -1; }

    sb_super = (uffs_super_t *)map;
    sb = (uffs_inode_t *)((char *)map + sizeof(uffs_super_t));
    backing_fd = fd;
    backing_size = need;

    if (fresh) {
        sb_super->magic = UFFS_MAGIC;
        sb_super->version = UFFS_VERSION;
        sb_super->max_inodes = UFFS_MAX_INODES;
        sb_super->file_slot  = UFFS_FILE_SLOT;
        /* build root inode */
        memset(&sb[1], 0, sizeof(sb[1]));
        sb[1].in_use = 1;
        sb[1].type   = UFFS_DIR;
        sb[1].mode   = 0755;
        sb[1].nlink  = 2;
        sb[1].uid    = getuid();
        sb[1].gid    = getgid();
        sb[1].atime  = sb[1].mtime = sb[1].ctime = time(NULL);
        msync(map, need, MS_SYNC);
        fprintf(stderr, "uffs_persist: created fresh image at %s\n", path);
    } else {
        if (sb_super->magic != UFFS_MAGIC) {
            fprintf(stderr, "uffs_persist: bad magic in backing file\n");
            return -1;
        }
        fprintf(stderr, "uffs_persist: reopened image at %s (%u inodes)\n",
                path, sb_super->max_inodes);
    }
    return 0;
}

static void *uffs_init(struct fuse_conn_info *conn, struct fuse_config *cfg) {
    (void)conn; (void)cfg;
    return NULL;
}

static void uffs_destroy(void *priv) {
    (void)priv;
    if (sb_super) msync(sb_super, backing_size, MS_SYNC);
    if (backing_fd >= 0) close(backing_fd);
    fprintf(stderr, "uffs_persist: synced and closed.\n");
}

static const struct fuse_operations uffs_ops = {
    .init     = uffs_init,
    .destroy  = uffs_destroy,
    .getattr  = uffs_getattr,
    .readdir  = uffs_readdir,
    .create   = uffs_create,
    .mkdir    = uffs_mkdir,
    .unlink   = uffs_unlink,
    .rmdir    = uffs_rmdir,
    .open     = uffs_open,
    .read     = uffs_read,
    .write    = uffs_write,
    .truncate = uffs_truncate,
    .chmod    = uffs_chmod,
    .chown    = uffs_chown,
    .utimens  = uffs_utimens,
    .symlink  = uffs_symlink,
    .readlink = uffs_readlink,
    .link     = uffs_link,
    .rename   = uffs_rename,
    .statfs   = uffs_statfs,
};

/* Custom option parsing so we can accept -o backing=PATH */
struct uffs_opts { char *backing; };
#define OPT(t, p) { t, offsetof(struct uffs_opts, p), 1 }
static const struct fuse_opt opt_spec[] = {
    OPT("backing=%s", backing),
    FUSE_OPT_END
};

int main(int argc, char *argv[]) {
    struct fuse_args args = FUSE_ARGS_INIT(argc, argv);
    struct uffs_opts opts = {0};
    if (fuse_opt_parse(&args, &opts, opt_spec, NULL) == -1) return 1;
    if (!opts.backing) {
        fprintf(stderr, "usage: uffs_persist -o backing=PATH MOUNTPOINT\n");
        return 1;
    }
    backing_path = opts.backing;
    if (open_backing(backing_path) < 0) return 1;
    return fuse_main(args.argc, args.argv, &uffs_ops, NULL);
}
