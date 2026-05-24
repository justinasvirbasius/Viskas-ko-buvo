/*
 * uffs_core.c — storage core. Owns all filesystem state. Listens on a
 * Unix socket and serves requests from one or more nodes.
 *
 * Build:
 *   cc -Wall -O2 uffs_core.c -o uffs_core
 *
 * Run:
 *   ./uffs_core /tmp/uffs_core.sock
 *
 * This is a stripped-down server: one client at a time, in-memory storage,
 * no persistence. The point is to show the boundary, not to be fast.
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
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <signal.h>

#define UFFS_MAX_INODES   1024
#define UFFS_MAX_NAME     255
#define UFFS_MAX_CHILDREN 128
#define UFFS_MAX_FILESZ   (1 << 20)

enum { T_REG=0, T_DIR=1, T_LNK=2 };

typedef struct {
    int in_use, type;
    uint32_t mode, uid, gid, nlink;
    uint64_t size;
    int64_t atime, mtime, ctime;
    struct { char name[UFFS_MAX_NAME+1]; int ino; } ch[UFFS_MAX_CHILDREN];
    int nch;
    char *data; size_t cap;
} inode_t;

static inode_t sb[UFFS_MAX_INODES];

/* ---- inode helpers (same logic as before, just no FUSE wrappers) ----- */

static int alloc_inode(void) {
    for (int i = 1; i < UFFS_MAX_INODES; i++)
        if (!sb[i].in_use) {
            memset(&sb[i], 0, sizeof(sb[i]));
            sb[i].in_use = 1;
            sb[i].atime = sb[i].mtime = sb[i].ctime = time(NULL);
            return i;
        }
    return -ENOSPC;
}

static void free_inode(int ino) {
    if (ino <= 0 || ino >= UFFS_MAX_INODES) return;
    free(sb[ino].data);
    memset(&sb[ino], 0, sizeof(sb[ino]));
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
        for (int i = 0; i < sb[cur].nch; i++)
            if (strcmp(sb[cur].ch[i].name, comp) == 0) { next = sb[cur].ch[i].ino; break; }
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
    if (sb[parent].nch >= UFFS_MAX_CHILDREN) return -ENOSPC;
    int n = sb[parent].nch++;
    strncpy(sb[parent].ch[n].name, name, UFFS_MAX_NAME);
    sb[parent].ch[n].name[UFFS_MAX_NAME] = 0;
    sb[parent].ch[n].ino = child;
    return 0;
}

static int dir_remove(int parent, const char *name) {
    for (int i = 0; i < sb[parent].nch; i++)
        if (strcmp(sb[parent].ch[i].name, name) == 0) {
            sb[parent].ch[i] = sb[parent].ch[--sb[parent].nch];
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

/* ---- op handlers ------------------------------------------------------ */

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
    int ino = path_lookup(path, NULL, NULL);
    if (ino < 0) return send_reply(fd, ino, NULL, 0);
    uffs_attr_t a; fill_attr(ino, &a);
    return send_reply(fd, 0, &a, sizeof(a));
}

static int handle_readdir(int fd, const char *path) {
    int ino = path_lookup(path, NULL, NULL);
    if (ino < 0) return send_reply(fd, ino, NULL, 0);
    if (sb[ino].type != T_DIR) return send_reply(fd, -ENOTDIR, NULL, 0);

    /* payload: each entry is "name\0", terminated by an extra "\0" */
    char buf[16384]; size_t pos = 0;
    for (int i = 0; i < sb[ino].nch; i++) {
        size_t L = strlen(sb[ino].ch[i].name) + 1;
        if (pos + L + 1 > sizeof(buf)) break;
        memcpy(buf + pos, sb[ino].ch[i].name, L);
        pos += L;
    }
    buf[pos++] = 0;
    return send_reply(fd, 0, buf, pos);
}

static int handle_create_or_mkdir(int fd, const char *path, uint32_t mode, int type) {
    int parent; char leaf[UFFS_MAX_NAME+1];
    int ino = path_lookup(path, &parent, leaf);
    if (ino >= 0) return send_reply(fd, -EEXIST, NULL, 0);
    if (ino != -ENOENT) return send_reply(fd, ino, NULL, 0);
    if (parent < 0) return send_reply(fd, -EINVAL, NULL, 0);

    int ni = alloc_inode();
    if (ni < 0) return send_reply(fd, ni, NULL, 0);
    sb[ni].type  = type;
    sb[ni].mode  = mode & 0777;
    sb[ni].nlink = (type == T_DIR) ? 2 : 1;
    int rc = dir_add(parent, leaf, ni);
    if (rc < 0) { free_inode(ni); return send_reply(fd, rc, NULL, 0); }
    return send_reply(fd, 0, NULL, 0);
}

static int handle_unlink(int fd, const char *path) {
    int parent; char leaf[UFFS_MAX_NAME+1];
    int ino = path_lookup(path, &parent, leaf);
    if (ino < 0) return send_reply(fd, ino, NULL, 0);
    if (sb[ino].type == T_DIR) return send_reply(fd, -EISDIR, NULL, 0);
    dir_remove(parent, leaf);
    if (--sb[ino].nlink == 0) free_inode(ino);
    return send_reply(fd, 0, NULL, 0);
}

static int handle_rmdir(int fd, const char *path) {
    int parent; char leaf[UFFS_MAX_NAME+1];
    int ino = path_lookup(path, &parent, leaf);
    if (ino < 0) return send_reply(fd, ino, NULL, 0);
    if (sb[ino].type != T_DIR) return send_reply(fd, -ENOTDIR, NULL, 0);
    if (sb[ino].nch > 0) return send_reply(fd, -ENOTEMPTY, NULL, 0);
    dir_remove(parent, leaf);
    free_inode(ino);
    return send_reply(fd, 0, NULL, 0);
}

static int handle_read(int fd, const char *path, uint64_t off, uint32_t size) {
    int ino = path_lookup(path, NULL, NULL);
    if (ino < 0) return send_reply(fd, ino, NULL, 0);
    inode_t *n = &sb[ino];
    if (off >= n->size) return send_reply(fd, 0, NULL, 0);
    if (off + size > n->size) size = n->size - off;
    return send_reply(fd, size, n->data + off, size);
}

static int handle_write(int fd, const char *path, uint64_t off,
                        const char *data, uint32_t size) {
    int ino = path_lookup(path, NULL, NULL);
    if (ino < 0) return send_reply(fd, ino, NULL, 0);
    inode_t *n = &sb[ino];
    size_t need = off + size;
    if (need > UFFS_MAX_FILESZ) return send_reply(fd, -EFBIG, NULL, 0);
    if (need > n->cap) {
        size_t nc = n->cap ? n->cap : 4096;
        while (nc < need) nc *= 2;
        char *p = realloc(n->data, nc);
        if (!p) return send_reply(fd, -ENOMEM, NULL, 0);
        n->data = p; n->cap = nc;
    }
    memcpy(n->data + off, data, size);
    if (need > n->size) n->size = need;
    n->mtime = n->ctime = time(NULL);
    return send_reply(fd, size, NULL, 0);
}

static int handle_truncate(int fd, const char *path, uint64_t size) {
    int ino = path_lookup(path, NULL, NULL);
    if (ino < 0) return send_reply(fd, ino, NULL, 0);
    if (size > UFFS_MAX_FILESZ) return send_reply(fd, -EFBIG, NULL, 0);
    inode_t *n = &sb[ino];
    if (size > n->cap) {
        char *p = realloc(n->data, size);
        if (!p) return send_reply(fd, -ENOMEM, NULL, 0);
        n->data = p; n->cap = size;
    }
    if (size > n->size) memset(n->data + n->size, 0, size - n->size);
    n->size = size;
    return send_reply(fd, 0, NULL, 0);
}

static int handle_rename(int fd, const char *from, const char *to) {
    int sp; char sl[UFFS_MAX_NAME+1];
    int src = path_lookup(from, &sp, sl);
    if (src < 0) return send_reply(fd, src, NULL, 0);
    int dp; char dl[UFFS_MAX_NAME+1];
    int dst = path_lookup(to, &dp, dl);
    if (dst == src) return send_reply(fd, 0, NULL, 0);
    if (dst >= 0) {
        if (sb[dst].type == T_DIR) {
            if (sb[src].type != T_DIR) return send_reply(fd, -EISDIR, NULL, 0);
            if (sb[dst].nch > 0) return send_reply(fd, -ENOTEMPTY, NULL, 0);
        } else if (sb[src].type == T_DIR) {
            return send_reply(fd, -ENOTDIR, NULL, 0);
        }
        dir_remove(dp, dl);
        if (--sb[dst].nlink == 0) free_inode(dst);
    } else if (dst != -ENOENT) {
        return send_reply(fd, dst, NULL, 0);
    }
    dir_remove(sp, sl);
    int rc = dir_add(dp, dl, src);
    return send_reply(fd, rc, NULL, 0);
}

/* ---- request dispatcher ---------------------------------------------- */

static int serve_one_request(int fd) {
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

static void init_root(void) {
    sb[1].in_use = 1;
    sb[1].type   = T_DIR;
    sb[1].mode   = 0755;
    sb[1].nlink  = 2;
    sb[1].uid    = getuid();
    sb[1].gid    = getgid();
    sb[1].atime  = sb[1].mtime = sb[1].ctime = time(NULL);
}

int main(int argc, char *argv[]) {
    const char *sock_path = argc > 1 ? argv[1] : UFFS_SOCK_DEFAULT;
    signal(SIGPIPE, SIG_IGN);

    unlink(sock_path);
    int srv = socket(AF_UNIX, SOCK_STREAM, 0);
    if (srv < 0) { perror("socket"); return 1; }
    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    strncpy(addr.sun_path, sock_path, sizeof(addr.sun_path) - 1);
    if (bind(srv, (struct sockaddr *)&addr, sizeof(addr)) < 0) { perror("bind"); return 1; }
    if (listen(srv, 4) < 0) { perror("listen"); return 1; }

    init_root();
    fprintf(stderr, "uffs_core: listening on %s\n", sock_path);

    /* Single-connection-at-a-time loop. */
    for (;;) {
        int cli = accept(srv, NULL, NULL);
        if (cli < 0) { if (errno == EINTR) continue; perror("accept"); break; }
        fprintf(stderr, "uffs_core: node connected\n");
        while (serve_one_request(cli) == 0) {}
        fprintf(stderr, "uffs_core: node disconnected\n");
        close(cli);
    }
    close(srv);
    unlink(sock_path);
    return 0;
}
