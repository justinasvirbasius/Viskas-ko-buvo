/*
 * uffs.c — Universal Fast File System (skeleton)
 *
 * A minimal in-memory filesystem built on FUSE. Mountable as a real
 * filesystem: the kernel's VFS routes syscalls (open, read, write, stat,
 * readdir, mkdir, unlink, rename, symlink, link, statfs, chmod, chown...)
 * through the FUSE driver to this userspace process. That is the
 * "kernel <-> up above" connection.
 *
 * Storage backend: a flat array of inodes held in RAM. Replace this layer
 * later with a block device, a network "node" backend, or an on-disk
 * superblock+inode format — the VFS-facing operations stay the same.
 *
 * Build (Linux):
 *   cc -Wall -O2 uffs.c `pkg-config fuse3 --cflags --libs` -o uffs
 * Build (FreeBSD/macOS with libfuse2-style fuse):
 *   cc -Wall -O2 -D_FILE_OFFSET_BITS=64 uffs.c -lfuse -o uffs
 *
 * Run:
 *   mkdir /tmp/uffs
 *   ./uffs -f /tmp/uffs        # -f keeps it in the foreground
 *   # in another shell:
 *   ls /tmp/uffs ; echo hi > /tmp/uffs/test ; cat /tmp/uffs/test
 *   fusermount -u /tmp/uffs    # unmount (Linux); on BSD: umount /tmp/uffs
 *
 * NOTE: This is a skeleton. It is not crash-safe, not persistent, not
 * concurrent-safe beyond FUSE's default single-thread mode, and it imposes
 * a fixed inode/file-size cap. It exists so you can hang real storage and
 * real semantics off a working VFS shape.
 */

#define FUSE_USE_VERSION 31

#include <fuse.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/statvfs.h>

/* --- on-"disk" limits (in-RAM here) ------------------------------------ */
#define UFFS_MAX_INODES   1024
#define UFFS_MAX_NAME     255
#define UFFS_MAX_CHILDREN 128
#define UFFS_BLOCK_SIZE   4096
#define UFFS_MAX_FILESZ   (1024 * 1024)   /* 1 MiB per file, plenty for a demo */

/* --- inode: the core unit. Same shape you'd put on disk. --------------- */
/* inode type — was previously just an is_dir flag */
enum uffs_type { UFFS_REG = 0, UFFS_DIR = 1, UFFS_LNK = 2 };

typedef struct uffs_inode {
    int      in_use;
    int      type;           /* enum uffs_type */
    mode_t   mode;
    uid_t    uid;
    gid_t    gid;
    nlink_t  nlink;
    off_t    size;
    time_t   atime, mtime, ctime;

    /* directory: list of (name, child inode) entries */
    struct {
        char name[UFFS_MAX_NAME + 1];
        int  ino;
    } children[UFFS_MAX_CHILDREN];
    int n_children;

    /* file: a single contiguous buffer. Swap for extents/blocks later. */
    char *data;
    size_t cap;

    /* symlink: target path */
    char *link_target;
} uffs_inode_t;

/* --- superblock: the table of inodes. Root is inode 1. ----------------- */
static uffs_inode_t sb[UFFS_MAX_INODES];

/* --- helpers ----------------------------------------------------------- */

static int alloc_inode(void) {
    for (int i = 1; i < UFFS_MAX_INODES; i++) {     /* 0 reserved */
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
    if (ino <= 0 || ino >= UFFS_MAX_INODES) return;
    free(sb[ino].data);
    free(sb[ino].link_target);
    memset(&sb[ino], 0, sizeof(sb[ino]));
}

/* Walk a path like "/a/b/c" and return the inode number, or -errno. */
static int path_lookup(const char *path, int *parent_out, char *leaf_out) {
    if (path[0] != '/') return -EINVAL;

    int cur = 1;            /* root */
    int parent = -1;
    char leaf[UFFS_MAX_NAME + 1] = "";

    const char *p = path + 1;
    while (*p) {
        const char *slash = strchr(p, '/');
        size_t len = slash ? (size_t)(slash - p) : strlen(p);
        if (len == 0) { p++; continue; }
        if (len > UFFS_MAX_NAME) return -ENAMETOOLONG;

        char comp[UFFS_MAX_NAME + 1];
        memcpy(comp, p, len);
        comp[len] = 0;

        if (sb[cur].type != UFFS_DIR) return -ENOTDIR;

        int next = -1;
        for (int i = 0; i < sb[cur].n_children; i++) {
            if (strcmp(sb[cur].children[i].name, comp) == 0) {
                next = sb[cur].children[i].ino;
                break;
            }
        }
        parent = cur;
        strncpy(leaf, comp, sizeof(leaf));

        if (next < 0) {
            /* last component missing is fine for create-style callers */
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
    int n = sb[parent].n_children++;
    strncpy(sb[parent].children[n].name, name, UFFS_MAX_NAME);
    sb[parent].children[n].name[UFFS_MAX_NAME] = 0;
    sb[parent].children[n].ino = child_ino;
    sb[parent].mtime = sb[parent].ctime = time(NULL);
    return 0;
}

static int dir_remove(int parent, const char *name) {
    for (int i = 0; i < sb[parent].n_children; i++) {
        if (strcmp(sb[parent].children[i].name, name) == 0) {
            sb[parent].children[i] = sb[parent].children[--sb[parent].n_children];
            sb[parent].mtime = sb[parent].ctime = time(NULL);
            return 0;
        }
    }
    return -ENOENT;
}

/* ====================================================================== */
/*  FUSE operations — these are the VFS callbacks the kernel invokes.     */
/* ====================================================================== */

static int uffs_getattr(const char *path, struct stat *st,
                        struct fuse_file_info *fi) {
    (void)fi;
    int ino = path_lookup(path, NULL, NULL);
    if (ino < 0) return ino;

    uffs_inode_t *n = &sb[ino];
    memset(st, 0, sizeof(*st));
    st->st_ino   = ino;
    mode_t typebits = S_IFREG;
    if (n->type == UFFS_DIR) typebits = S_IFDIR;
    else if (n->type == UFFS_LNK) typebits = S_IFLNK;
    st->st_mode  = n->mode | typebits;
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
                        off_t offset, struct fuse_file_info *fi,
                        enum fuse_readdir_flags flags) {
    (void)offset; (void)fi; (void)flags;
    int ino = path_lookup(path, NULL, NULL);
    if (ino < 0) return ino;
    if (sb[ino].type != UFFS_DIR) return -ENOTDIR;

    filler(buf, ".",  NULL, 0, 0);
    filler(buf, "..", NULL, 0, 0);
    for (int i = 0; i < sb[ino].n_children; i++)
        filler(buf, sb[ino].children[i].name, NULL, 0, 0);
    return 0;
}

static int uffs_mknod_common(const char *path, mode_t mode, int type) {
    int parent;
    char leaf[UFFS_MAX_NAME + 1];
    int ino = path_lookup(path, &parent, leaf);
    if (ino >= 0) return -EEXIST;
    if (ino != -ENOENT) return ino;
    if (parent < 0) return -EINVAL;

    int new_ino = alloc_inode();
    if (new_ino < 0) return new_ino;

    uffs_inode_t *n = &sb[new_ino];
    n->type   = type;
    n->mode   = mode & 0777;
    n->uid    = fuse_get_context()->uid;
    n->gid    = fuse_get_context()->gid;
    n->nlink  = (type == UFFS_DIR) ? 2 : 1;

    int rc = dir_add(parent, leaf, new_ino);
    if (rc < 0) { free_inode(new_ino); return rc; }
    return new_ino;
}

static int uffs_create(const char *path, mode_t mode,
                       struct fuse_file_info *fi) {
    (void)fi;
    int rc = uffs_mknod_common(path, mode, UFFS_REG);
    return rc < 0 ? rc : 0;
}

static int uffs_mkdir(const char *path, mode_t mode) {
    int rc = uffs_mknod_common(path, mode, UFFS_DIR);
    return rc < 0 ? rc : 0;
}

static int uffs_unlink(const char *path) {
    int parent;
    char leaf[UFFS_MAX_NAME + 1];
    int ino = path_lookup(path, &parent, leaf);
    if (ino < 0) return ino;
    if (sb[ino].type == UFFS_DIR) return -EISDIR;
    dir_remove(parent, leaf);
    if (--sb[ino].nlink == 0) free_inode(ino);
    return 0;
}

static int uffs_rmdir(const char *path) {
    int parent;
    char leaf[UFFS_MAX_NAME + 1];
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
    if (off >= n->size) return 0;
    if (off + (off_t)size > n->size) size = n->size - off;
    memcpy(buf, n->data + off, size);
    n->atime = time(NULL);
    return size;
}

static int uffs_write(const char *path, const char *buf, size_t size,
                      off_t off, struct fuse_file_info *fi) {
    (void)fi;
    int ino = path_lookup(path, NULL, NULL);
    if (ino < 0) return ino;
    uffs_inode_t *n = &sb[ino];

    size_t need = off + size;
    if (need > UFFS_MAX_FILESZ) return -EFBIG;
    if (need > n->cap) {
        size_t newcap = n->cap ? n->cap : UFFS_BLOCK_SIZE;
        while (newcap < need) newcap *= 2;
        char *p = realloc(n->data, newcap);
        if (!p) return -ENOMEM;
        n->data = p;
        n->cap  = newcap;
    }
    memcpy(n->data + off, buf, size);
    if ((off_t)need > n->size) n->size = need;
    n->mtime = n->ctime = time(NULL);
    return size;
}

static int uffs_truncate(const char *path, off_t size,
                         struct fuse_file_info *fi) {
    (void)fi;
    int ino = path_lookup(path, NULL, NULL);
    if (ino < 0) return ino;
    uffs_inode_t *n = &sb[ino];
    if (size > UFFS_MAX_FILESZ) return -EFBIG;
    if ((size_t)size > n->cap) {
        char *p = realloc(n->data, size);
        if (!p) return -ENOMEM;
        n->data = p;
        n->cap  = size;
    }
    if (size > n->size) memset(n->data + n->size, 0, size - n->size);
    n->size = size;
    n->mtime = n->ctime = time(NULL);
    return 0;
}

static int uffs_utimens(const char *path, const struct timespec tv[2],
                        struct fuse_file_info *fi) {
    (void)fi;
    int ino = path_lookup(path, NULL, NULL);
    if (ino < 0) return ino;
    sb[ino].atime = tv[0].tv_sec;
    sb[ino].mtime = tv[1].tv_sec;
    return 0;
}

static int uffs_chmod(const char *path, mode_t mode,
                      struct fuse_file_info *fi) {
    (void)fi;
    int ino = path_lookup(path, NULL, NULL);
    if (ino < 0) return ino;
    sb[ino].mode = mode & 0777;
    sb[ino].ctime = time(NULL);
    return 0;
}

static int uffs_chown(const char *path, uid_t uid, gid_t gid,
                      struct fuse_file_info *fi) {
    (void)fi;
    int ino = path_lookup(path, NULL, NULL);
    if (ino < 0) return ino;
    if (uid != (uid_t)-1) sb[ino].uid = uid;
    if (gid != (gid_t)-1) sb[ino].gid = gid;
    sb[ino].ctime = time(NULL);
    return 0;
}

/* symlink: create a new inode whose payload is the target string. */
static int uffs_symlink(const char *target, const char *path) {
    int new_ino = uffs_mknod_common(path, 0777, UFFS_LNK);
    if (new_ino < 0) return new_ino;
    sb[new_ino].link_target = strdup(target);
    if (!sb[new_ino].link_target) {
        /* roll back: remove the directory entry we just added */
        int parent;
        char leaf[UFFS_MAX_NAME + 1];
        path_lookup(path, &parent, leaf);
        dir_remove(parent, leaf);
        free_inode(new_ino);
        return -ENOMEM;
    }
    sb[new_ino].size = strlen(target);
    return 0;
}

static int uffs_readlink(const char *path, char *buf, size_t size) {
    int ino = path_lookup(path, NULL, NULL);
    if (ino < 0) return ino;
    if (sb[ino].type != UFFS_LNK) return -EINVAL;
    strncpy(buf, sb[ino].link_target, size - 1);
    buf[size - 1] = 0;
    return 0;
}

/* hard link: add a second directory entry pointing at the same inode. */
static int uffs_link(const char *from, const char *to) {
    int src = path_lookup(from, NULL, NULL);
    if (src < 0) return src;
    if (sb[src].type == UFFS_DIR) return -EPERM;

    int parent;
    char leaf[UFFS_MAX_NAME + 1];
    int existing = path_lookup(to, &parent, leaf);
    if (existing >= 0)         return -EEXIST;
    if (existing != -ENOENT)   return existing;
    if (parent < 0)            return -EINVAL;

    int rc = dir_add(parent, leaf, src);
    if (rc < 0) return rc;
    sb[src].nlink++;
    sb[src].ctime = time(NULL);
    return 0;
}

/* rename: simple case — same directory or cross-directory, overwriting any
 * existing destination of a compatible type. Doesn't yet implement
 * RENAME_EXCHANGE / RENAME_NOREPLACE flags. */
static int uffs_rename(const char *from, const char *to, unsigned int flags) {
    if (flags) return -EINVAL;   /* extended flags not supported */

    int sparent;
    char sleaf[UFFS_MAX_NAME + 1];
    int src = path_lookup(from, &sparent, sleaf);
    if (src < 0) return src;

    int dparent;
    char dleaf[UFFS_MAX_NAME + 1];
    int dst = path_lookup(to, &dparent, dleaf);
    if (dst == src) return 0;            /* rename to self: no-op */
    if (dst >= 0) {
        /* destination exists — unlink it first */
        if (sb[dst].type == UFFS_DIR) {
            if (sb[src].type != UFFS_DIR) return -EISDIR;
            if (sb[dst].n_children > 0)    return -ENOTEMPTY;
        } else if (sb[src].type == UFFS_DIR) {
            return -ENOTDIR;
        }
        dir_remove(dparent, dleaf);
        if (--sb[dst].nlink == 0) free_inode(dst);
    } else if (dst != -ENOENT) {
        return dst;
    }

    dir_remove(sparent, sleaf);
    return dir_add(dparent, dleaf, src);
}

static int uffs_statfs(const char *path, struct statvfs *st) {
    (void)path;
    int used = 0;
    for (int i = 1; i < UFFS_MAX_INODES; i++) if (sb[i].in_use) used++;
    memset(st, 0, sizeof(*st));
    st->f_bsize   = UFFS_BLOCK_SIZE;
    st->f_frsize  = UFFS_BLOCK_SIZE;
    st->f_blocks  = UFFS_MAX_INODES;       /* coarse approximation */
    st->f_bfree   = UFFS_MAX_INODES - used;
    st->f_bavail  = st->f_bfree;
    st->f_files   = UFFS_MAX_INODES;
    st->f_ffree   = UFFS_MAX_INODES - used;
    st->f_favail  = st->f_ffree;
    st->f_namemax = UFFS_MAX_NAME;
    return 0;
}

static void *uffs_init(struct fuse_conn_info *conn, struct fuse_config *cfg) {
    (void)conn; (void)cfg;
    /* Build the root inode (ino = 1). */
    memset(sb, 0, sizeof(sb));
    sb[1].in_use = 1;
    sb[1].type   = UFFS_DIR;
    sb[1].mode   = 0755;
    sb[1].nlink  = 2;
    sb[1].uid    = getuid();
    sb[1].gid    = getgid();
    sb[1].atime  = sb[1].mtime = sb[1].ctime = time(NULL);
    fprintf(stderr, "uffs: mounted, root inode initialized.\n");
    return NULL;
}

/* --- op table: what we hand to the kernel via FUSE --------------------- */
static const struct fuse_operations uffs_ops = {
    .init     = uffs_init,
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
    .utimens  = uffs_utimens,
    .chmod    = uffs_chmod,
    .chown    = uffs_chown,
    .symlink  = uffs_symlink,
    .readlink = uffs_readlink,
    .link     = uffs_link,
    .rename   = uffs_rename,
    .statfs   = uffs_statfs,
};

int main(int argc, char *argv[]) {
    return fuse_main(argc, argv, &uffs_ops, NULL);
}
