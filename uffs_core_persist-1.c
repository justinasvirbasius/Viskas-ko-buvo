/*
 * uffs_core_persist.c — storage core with persistent backing and
 * multi-node support. Combines uffs_persist's mmap'd inode table with
 * uffs_core's RPC server.
 *
 * Build:
 *   cc -Wall -O2 -pthread uffs_core_persist.c -o uffs_core_persist
 *
 * Run:
 *   ./uffs_core_persist /tmp/uffs.img /tmp/uffs_core.sock
 *
 * Then mount as many nodes as you want against the same socket:
 *   ./uffs_node -f -o core=/tmp/uffs_core.sock /tmp/uffs_A &
 *   ./uffs_node -f -o core=/tmp/uffs_core.sock /tmp/uffs_B &
 *
 * The nodes MUST be started with kernel-side caching disabled or they
 * will see each other stale. Use:
 *   ./uffs_node -f -o core=...,entry_timeout=0,attr_timeout=0,ac_attr_timeout=0 MNT
 *
 * Caveats (still): crash during write may corrupt the inode table;
 * msync is MS_ASYNC; no journaling; coarse global lock around all ops.
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
#include <pthread.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/mman.h>
#include <sys/stat.h>

#define UFFS_MAGIC        0x55464653u
#define UFFS_VERSION      1
#define UFFS_MAX_INODES   1024
#define UFFS_MAX_NAME     255
#define UFFS_MAX_CHILDREN 128
#define UFFS_BLOCK_SIZE   4096
#define UFFS_FILE_SLOT    (256 * 1024)

enum { T_REG=0, T_DIR=1, T_LNK=2 };

/* On-disk inode (mmap'd directly). Fixed-width fields, no pointers. */
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
    char     data[UFFS_FILE_SLOT];
} uffs_inode_t;

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t max_inodes;
    uint32_t file_slot;
} uffs_super_t;

static uffs_super_t *sb_super;
static uffs_inode_t *sb;
static size_t        backing_size;
static int           backing_fd = -1;

/* Coarse lock around the entire inode table. Held across each op. */
static pthread_mutex_t fs_lock = PTHREAD_MUTEX_INITIALIZER;

/* ---- mmap helpers ----------------------------------------------------- */

static void sync_inode(int ino) {
    char *base = (char *)&sb[ino];
    long ps = sysconf(_SC_PAGESIZE);
    uintptr_t start = (uintptr_t)base & ~(ps - 1);
    uintptr_t end   = ((uintptr_t)base + sizeof(uffs_inode_t) + ps - 1) & ~(ps - 1);
    msync((void *)start, end - start, MS_ASYNC);
}

static int open_backing(const char *path) {
    int fd = open(path, O_RDWR | O_CREAT, 0644);
    if (fd < 0) { perror("open backing"); return -1; }
    size_t need = sizeof(uffs_super_t) + sizeof(uffs_inode_t) * UFFS_MAX_INODES;
    struct stat st; fstat(fd, &st);
    int fresh = (st.st_size == 0);
    if (fresh && ftruncate(fd, need) < 0) { perror("ftruncate"); close(fd); return -1; }
    void *map = mmap(NULL, need, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) { perror("mmap"); close(fd); return -1; }

    sb_super = (uffs_super_t *)map;
    sb       = (uffs_inode_t *)((char *)map + sizeof(uffs_super_t));
    backing_fd   = fd;
    backing_size = need;

    if (fresh) {
        sb_super->magic       = UFFS_MAGIC;
        sb_super->version     = UFFS_VERSION;
        sb_super->max_inodes  = UFFS_MAX_INODES;
        sb_super->file_slot   = UFFS_FILE_SLOT;
        memset(&sb[1], 0, sizeof(sb[1]));
        sb[1].in_use = 1;
        sb[1].type   = T_DIR;
        sb[1].mode   = 0755;
        sb[1].nlink  = 2;
        sb[1].uid    = getuid();
        sb[1].gid    = getgid();
        sb[1].atime = sb[1].mtime = sb[1].ctime = time(NULL);
        msync(map, need, MS_SYNC);
        fprintf(stderr, "core_persist: created fresh image at %s\n", path);
    } else {
        if (sb_super->magic != UFFS_MAGIC) {
            fprintf(stderr, "core_persist: bad magic in %s\n", path);
            return -1;
        }
        fprintf(stderr, "core_persist: reopened image at %s\n", path);
    }
    return 0;
}

/* ---- inode/dir helpers (locked by caller) ---------------------------- */

static int alloc_inode(void) {
    for (uint32_t i = 1; i < sb_super->max_inodes; i++)
        if (!sb[i].in_use) {
            memset(&sb[i], 0, sizeof(sb[i]));
            sb[i].in_use = 1;
            sb[i].atime = sb[i].mtime = sb[i].ctime = time(NULL);
            return i;
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
    char leaf[UFFS_MAX_NAME+1] = "";
    const char *p = path + 1;
    while (*p) {
        const char *slash = strchr(p, '/');
        size_t len = slash ? (size_t)(slash - p) : strlen(p);
        if (len == 0) { p++; continue; }
        if (len > UFFS_MAX_NAME) return -ENAMETOOLONG;
        char comp[UFFS_MAX_NAME+1];
        memcpy(comp, p, len); comp[len] = 0;
        if (sb[cur].type != T_DIR) return -ENOTDIR;
        int next = -1;
        for (uint32_t i = 0; i < sb[cur].n_children; i++)
            if (strcmp(sb[cur].children[i].name, comp) == 0) {
                next = sb[cur].children[i].ino; break;
            }
        parent = cur; strncpy(leaf, comp, sizeof(leaf));
        if (next < 0) {
            if (slash == NULL || *(slash+1) == 0) {
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

static int dir_add(int parent, const char *name, int child) {
    if (sb[parent].n_children >= UFFS_MAX_CHILDREN) return -ENOSPC;
    uint32_t n = sb[parent].n_children++;
    strncpy(sb[parent].children[n].name, name, UFFS_MAX_NAME);
    sb[parent].children[n].name[UFFS_MAX_NAME] = 0;
    sb[parent].children[n].ino = child;
    sb[parent].mtime = sb[parent].ctime = time(NULL);
    sync_inode(parent);
    return 0;
}

static int dir_remove(int parent, const char *name) {
    for (uint32_t i = 0; i < sb[parent].n_children; i++)
        if (strcmp(sb[parent].children[i].name, name) == 0) {
            sb[parent].children[i] = sb[parent].children[--sb[parent].n_children];
            sb[parent].mtime = sb[parent].ctime = time(NULL);
            sync_inode(parent);
            return 0;
        }
    return -ENOENT;
}

/* ---- wire I/O --------------------------------------------------------- */

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

/* Per-connection write lock so a multi-frame reply doesn't interleave
 * with another thread's reply on the same socket. (Each connection has
 * its own thread, so this is per-thread / per-socket.) */
static int send_reply(int fd, int32_t status, const void *payload, uint32_t plen) {
    uffs_frame_hdr_t h = {
        .length      = sizeof(h) - 4 + plen,
        .opcode      = 0,
        .status      = status,
        .payload_len = plen,
    };
    if (write_all(fd, &h, sizeof(h)) < 0) return -1;
    if (plen && write_all(fd, payload, plen) < 0) return -1;
    return 0;
}

/* ---- op handlers (each one takes the fs_lock) ------------------------ */

static void fill_attr(int ino, uffs_attr_t *a) {
    uint32_t tb = S_IFREG;
    if (sb[ino].type == T_DIR) tb = S_IFDIR;
    else if (sb[ino].type == T_LNK) tb = S_IFLNK;
    a->mode  = sb[ino].mode | tb;
    a->nlink = sb[ino].nlink;
    a->uid   = sb[ino].uid;
    a->gid   = sb[ino].gid;
    a->size  = sb[ino].size;
    a->atime = sb[ino].atime;
    a->mtime = sb[ino].mtime;
    a->ctime = sb[ino].ctime;
    a->ino   = ino;
}

static int handle_getattr(int fd, const char *path) {
    pthread_mutex_lock(&fs_lock);
    int ino = path_lookup(path, NULL, NULL);
    if (ino < 0) { pthread_mutex_unlock(&fs_lock); return send_reply(fd, ino, NULL, 0); }
    uffs_attr_t a; fill_attr(ino, &a);
    pthread_mutex_unlock(&fs_lock);
    return send_reply(fd, 0, &a, sizeof(a));
}

static int handle_readdir(int fd, const char *path) {
    pthread_mutex_lock(&fs_lock);
    int ino = path_lookup(path, NULL, NULL);
    if (ino < 0) { pthread_mutex_unlock(&fs_lock); return send_reply(fd, ino, NULL, 0); }
    if (sb[ino].type != T_DIR) { pthread_mutex_unlock(&fs_lock); return send_reply(fd, -ENOTDIR, NULL, 0); }

    char buf[16384]; size_t pos = 0;
    for (uint32_t i = 0; i < sb[ino].n_children; i++) {
        size_t L = strlen(sb[ino].children[i].name) + 1;
        if (pos + L + 1 > sizeof(buf)) break;
        memcpy(buf + pos, sb[ino].children[i].name, L);
        pos += L;
    }
    buf[pos++] = 0;
    pthread_mutex_unlock(&fs_lock);
    return send_reply(fd, 0, buf, pos);
}

static int handle_create_or_mkdir(int fd, const char *path, uint32_t mode, int type) {
    pthread_mutex_lock(&fs_lock);
    int parent; char leaf[UFFS_MAX_NAME+1];
    int ino = path_lookup(path, &parent, leaf);
    if (ino >= 0)        { pthread_mutex_unlock(&fs_lock); return send_reply(fd, -EEXIST, NULL, 0); }
    if (ino != -ENOENT)  { pthread_mutex_unlock(&fs_lock); return send_reply(fd, ino,     NULL, 0); }
    if (parent < 0)      { pthread_mutex_unlock(&fs_lock); return send_reply(fd, -EINVAL, NULL, 0); }

    int ni = alloc_inode();
    if (ni < 0) { pthread_mutex_unlock(&fs_lock); return send_reply(fd, ni, NULL, 0); }
    sb[ni].type  = type;
    sb[ni].mode  = mode & 0777;
    sb[ni].nlink = (type == T_DIR) ? 2 : 1;
    sync_inode(ni);
    int rc = dir_add(parent, leaf, ni);
    if (rc < 0) { free_inode(ni); pthread_mutex_unlock(&fs_lock); return send_reply(fd, rc, NULL, 0); }
    pthread_mutex_unlock(&fs_lock);
    return send_reply(fd, 0, NULL, 0);
}

static int handle_unlink(int fd, const char *path) {
    pthread_mutex_lock(&fs_lock);
    int parent; char leaf[UFFS_MAX_NAME+1];
    int ino = path_lookup(path, &parent, leaf);
    if (ino < 0) { pthread_mutex_unlock(&fs_lock); return send_reply(fd, ino, NULL, 0); }
    if (sb[ino].type == T_DIR) { pthread_mutex_unlock(&fs_lock); return send_reply(fd, -EISDIR, NULL, 0); }
    dir_remove(parent, leaf);
    if (--sb[ino].nlink == 0) free_inode(ino);
    else sync_inode(ino);
    pthread_mutex_unlock(&fs_lock);
    return send_reply(fd, 0, NULL, 0);
}

static int handle_rmdir(int fd, const char *path) {
    pthread_mutex_lock(&fs_lock);
    int parent; char leaf[UFFS_MAX_NAME+1];
    int ino = path_lookup(path, &parent, leaf);
    if (ino < 0)                       { pthread_mutex_unlock(&fs_lock); return send_reply(fd, ino, NULL, 0); }
    if (sb[ino].type != T_DIR)         { pthread_mutex_unlock(&fs_lock); return send_reply(fd, -ENOTDIR, NULL, 0); }
    if (sb[ino].n_children > 0)        { pthread_mutex_unlock(&fs_lock); return send_reply(fd, -ENOTEMPTY, NULL, 0); }
    dir_remove(parent, leaf);
    free_inode(ino);
    pthread_mutex_unlock(&fs_lock);
    return send_reply(fd, 0, NULL, 0);
}

static int handle_read(int fd, const char *path, uint64_t off, uint32_t size) {
    pthread_mutex_lock(&fs_lock);
    int ino = path_lookup(path, NULL, NULL);
    if (ino < 0) { pthread_mutex_unlock(&fs_lock); return send_reply(fd, ino, NULL, 0); }
    uffs_inode_t *n = &sb[ino];
    if (off >= n->size) { pthread_mutex_unlock(&fs_lock); return send_reply(fd, 0, NULL, 0); }
    if (off + size > n->size) size = n->size - off;
    /* copy out before unlocking so the buffer doesn't get mutated under us */
    char *tmp = malloc(size);
    if (!tmp) { pthread_mutex_unlock(&fs_lock); return send_reply(fd, -ENOMEM, NULL, 0); }
    memcpy(tmp, n->data + off, size);
    n->atime = time(NULL);
    sync_inode(ino);
    pthread_mutex_unlock(&fs_lock);
    int rc = send_reply(fd, size, tmp, size);
    free(tmp);
    return rc;
}

static int handle_write(int fd, const char *path, uint64_t off,
                        const char *data, uint32_t size) {
    pthread_mutex_lock(&fs_lock);
    int ino = path_lookup(path, NULL, NULL);
    if (ino < 0) { pthread_mutex_unlock(&fs_lock); return send_reply(fd, ino, NULL, 0); }
    uffs_inode_t *n = &sb[ino];
    if (off + size > UFFS_FILE_SLOT) { pthread_mutex_unlock(&fs_lock); return send_reply(fd, -EFBIG, NULL, 0); }
    memcpy(n->data + off, data, size);
    if (off + size > n->size) n->size = off + size;
    n->mtime = n->ctime = time(NULL);
    sync_inode(ino);
    pthread_mutex_unlock(&fs_lock);
    return send_reply(fd, size, NULL, 0);
}

static int handle_truncate(int fd, const char *path, uint64_t size) {
    pthread_mutex_lock(&fs_lock);
    int ino = path_lookup(path, NULL, NULL);
    if (ino < 0) { pthread_mutex_unlock(&fs_lock); return send_reply(fd, ino, NULL, 0); }
    if (size > UFFS_FILE_SLOT) { pthread_mutex_unlock(&fs_lock); return send_reply(fd, -EFBIG, NULL, 0); }
    uffs_inode_t *n = &sb[ino];
    if (size > n->size) memset(n->data + n->size, 0, size - n->size);
    n->size = size;
    n->mtime = n->ctime = time(NULL);
    sync_inode(ino);
    pthread_mutex_unlock(&fs_lock);
    return send_reply(fd, 0, NULL, 0);
}

static int handle_rename(int fd, const char *from, const char *to) {
    pthread_mutex_lock(&fs_lock);
    int sp; char sl[UFFS_MAX_NAME+1];
    int src = path_lookup(from, &sp, sl);
    if (src < 0) { pthread_mutex_unlock(&fs_lock); return send_reply(fd, src, NULL, 0); }
    int dp; char dl[UFFS_MAX_NAME+1];
    int dst = path_lookup(to, &dp, dl);
    if (dst == src) { pthread_mutex_unlock(&fs_lock); return send_reply(fd, 0, NULL, 0); }
    if (dst >= 0) {
        if (sb[dst].type == T_DIR) {
            if (sb[src].type != T_DIR) { pthread_mutex_unlock(&fs_lock); return send_reply(fd, -EISDIR, NULL, 0); }
            if (sb[dst].n_children > 0) { pthread_mutex_unlock(&fs_lock); return send_reply(fd, -ENOTEMPTY, NULL, 0); }
        } else if (sb[src].type == T_DIR) {
            pthread_mutex_unlock(&fs_lock);
            return send_reply(fd, -ENOTDIR, NULL, 0);
        }
        dir_remove(dp, dl);
        if (--sb[dst].nlink == 0) free_inode(dst);
    } else if (dst != -ENOENT) {
        pthread_mutex_unlock(&fs_lock);
        return send_reply(fd, dst, NULL, 0);
    }
    dir_remove(sp, sl);
    int rc = dir_add(dp, dl, src);
    pthread_mutex_unlock(&fs_lock);
    return send_reply(fd, rc, NULL, 0);
}

/* ---- dispatcher ------------------------------------------------------- */

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
    case OP_GETATTR:  rc = handle_getattr(fd, payload); break;
    case OP_READDIR:  rc = handle_readdir(fd, payload); break;
    case OP_CREATE: {
        uffs_create_req_t *r = (uffs_create_req_t *)payload;
        rc = handle_create_or_mkdir(fd, payload + sizeof(*r), r->mode, T_REG);
        break;
    }
    case OP_MKDIR: {
        uffs_create_req_t *r = (uffs_create_req_t *)payload;
        rc = handle_create_or_mkdir(fd, payload + sizeof(*r), r->mode, T_DIR);
        break;
    }
    case OP_UNLINK:   rc = handle_unlink(fd, payload); break;
    case OP_RMDIR:    rc = handle_rmdir(fd, payload); break;
    case OP_READ: {
        uffs_read_req_t *r = (uffs_read_req_t *)payload;
        rc = handle_read(fd, payload + sizeof(*r), r->offset, r->size);
        break;
    }
    case OP_WRITE: {
        uffs_write_req_t *r = (uffs_write_req_t *)payload;
        const char *path = payload + sizeof(*r);
        size_t plen = strlen(path) + 1;
        rc = handle_write(fd, path, r->offset, path + plen, r->size);
        break;
    }
    case OP_TRUNCATE: {
        uffs_truncate_req_t *r = (uffs_truncate_req_t *)payload;
        rc = handle_truncate(fd, payload + sizeof(*r), r->size);
        break;
    }
    case OP_RENAME: {
        const char *from = payload;
        const char *to   = payload + strlen(from) + 1;
        rc = handle_rename(fd, from, to);
        break;
    }
    default:
        rc = send_reply(fd, -ENOSYS, NULL, 0);
        break;
    }
    free(payload);
    return rc;
}

/* ---- one thread per connection --------------------------------------- */

static void *conn_thread(void *arg) {
    int fd = (int)(intptr_t)arg;
    fprintf(stderr, "core_persist: node connected (fd %d)\n", fd);
    while (serve_one(fd) == 0) {}
    fprintf(stderr, "core_persist: node disconnected (fd %d)\n", fd);
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
    const char *img_path  = argv[1];
    const char *sock_path = argv[2];

    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT,  on_signal);
    signal(SIGTERM, on_signal);

    if (open_backing(img_path) < 0) return 1;

    unlink(sock_path);
    g_srv = socket(AF_UNIX, SOCK_STREAM, 0);
    if (g_srv < 0) { perror("socket"); return 1; }
    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    strncpy(addr.sun_path, sock_path, sizeof(addr.sun_path) - 1);
    if (bind(g_srv, (struct sockaddr *)&addr, sizeof(addr)) < 0) { perror("bind"); return 1; }
    if (listen(g_srv, 8) < 0) { perror("listen"); return 1; }
    fprintf(stderr, "core_persist: listening on %s\n", sock_path);

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

    if (sb_super) msync(sb_super, backing_size, MS_SYNC);
    if (backing_fd >= 0) close(backing_fd);
    unlink(sock_path);
    fprintf(stderr, "core_persist: shutdown, image synced\n");
    return 0;
}
