/*
 * uffs_node.c — FUSE-facing node. Owns no state. Every VFS op becomes
 * an RPC to the core over a Unix socket.
 *
 * Build:
 *   cc -Wall -O2 uffs_node.c `pkg-config fuse3 --cflags --libs` -o uffs_node
 *
 * Run (after starting uffs_core):
 *   ./uffs_node -f -o core=/tmp/uffs_core.sock /tmp/uffs
 */
#define FUSE_USE_VERSION 31
#define _GNU_SOURCE

#include "uffs_proto.h"

#include <fuse.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <pthread.h>

static int           core_fd = -1;
static pthread_mutex_t core_mu = PTHREAD_MUTEX_INITIALIZER;
static char         *core_path;

/* ---- low-level wire I/O ---------------------------------------------- */

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

/* Send one request, read one reply. Reply payload (if any) is written into
 * *out_payload (caller frees). Returns the status field (>=0 on success,
 * -errno on failure). */
static int rpc(uint32_t op, const void *payload, uint32_t plen,
               void **out_payload, uint32_t *out_len) {
    pthread_mutex_lock(&core_mu);

    uffs_frame_hdr_t h = {
        .length      = sizeof(h) - 4 + plen,
        .opcode      = op,
        .status      = 0,
        .payload_len = plen,
    };
    if (write_all(core_fd, &h, sizeof(h)) < 0) goto bad;
    if (plen && write_all(core_fd, payload, plen) < 0) goto bad;

    uffs_frame_hdr_t r;
    if (read_all(core_fd, &r, sizeof(r)) < 0) goto bad;
    void *buf = NULL;
    if (r.payload_len) {
        buf = malloc(r.payload_len);
        if (!buf) goto bad;
        if (read_all(core_fd, buf, r.payload_len) < 0) { free(buf); goto bad; }
    }
    pthread_mutex_unlock(&core_mu);
    if (out_payload) *out_payload = buf; else free(buf);
    if (out_len)     *out_len = r.payload_len;
    return r.status;

bad:
    pthread_mutex_unlock(&core_mu);
    return -EIO;
}

/* Helper: send op with just a path as payload. */
static int rpc_path(uint32_t op, const char *path,
                    void **out_payload, uint32_t *out_len) {
    return rpc(op, path, strlen(path) + 1, out_payload, out_len);
}

/* ---- FUSE ops --------------------------------------------------------- */

static int n_getattr(const char *path, struct stat *st, struct fuse_file_info *fi) {
    (void)fi;
    void *p = NULL; uint32_t L = 0;
    int rc = rpc_path(OP_GETATTR, path, &p, &L);
    if (rc < 0) { free(p); return rc; }
    if (L < sizeof(uffs_attr_t)) { free(p); return -EIO; }
    uffs_attr_t *a = p;
    memset(st, 0, sizeof(*st));
    st->st_ino   = a->ino;
    st->st_mode  = a->mode;
    st->st_nlink = a->nlink;
    st->st_uid   = a->uid;
    st->st_gid   = a->gid;
    st->st_size  = a->size;
    st->st_atime = a->atime;
    st->st_mtime = a->mtime;
    st->st_ctime = a->ctime;
    free(p);
    return 0;
}

static int n_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
                     off_t off, struct fuse_file_info *fi,
                     enum fuse_readdir_flags fl) {
    (void)off; (void)fi; (void)fl;
    void *p = NULL; uint32_t L = 0;
    int rc = rpc_path(OP_READDIR, path, &p, &L);
    if (rc < 0) { free(p); return rc; }
    filler(buf, ".", NULL, 0, 0);
    filler(buf, "..", NULL, 0, 0);
    const char *s = p;
    while (s < (const char *)p + L && *s) {
        filler(buf, s, NULL, 0, 0);
        s += strlen(s) + 1;
    }
    free(p);
    return 0;
}

static int n_create_or_mkdir(uint32_t op, const char *path, mode_t mode) {
    size_t plen = strlen(path) + 1;
    uffs_create_req_t r = { .mode = mode };
    size_t total = sizeof(r) + plen;
    char *buf = malloc(total);
    memcpy(buf, &r, sizeof(r));
    memcpy(buf + sizeof(r), path, plen);
    int rc = rpc(op, buf, total, NULL, NULL);
    free(buf);
    return rc < 0 ? rc : 0;
}

static int n_create(const char *path, mode_t mode, struct fuse_file_info *fi) {
    (void)fi; return n_create_or_mkdir(OP_CREATE, path, mode);
}
static int n_mkdir(const char *path, mode_t mode) {
    return n_create_or_mkdir(OP_MKDIR, path, mode);
}
static int n_unlink(const char *path) {
    int rc = rpc_path(OP_UNLINK, path, NULL, NULL);
    return rc < 0 ? rc : 0;
}
static int n_rmdir(const char *path) {
    int rc = rpc_path(OP_RMDIR, path, NULL, NULL);
    return rc < 0 ? rc : 0;
}

static int n_open(const char *path, struct fuse_file_info *fi) {
    (void)fi;
    /* Just verify the file exists by getattr'ing it. */
    void *p = NULL; uint32_t L = 0;
    int rc = rpc_path(OP_GETATTR, path, &p, &L);
    free(p);
    return rc < 0 ? rc : 0;
}

static int n_read(const char *path, char *buf, size_t size, off_t off,
                  struct fuse_file_info *fi) {
    (void)fi;
    size_t plen = strlen(path) + 1;
    uffs_read_req_t r = { .offset = off, .size = size };
    char *req = malloc(sizeof(r) + plen);
    memcpy(req, &r, sizeof(r));
    memcpy(req + sizeof(r), path, plen);
    void *p = NULL; uint32_t L = 0;
    int rc = rpc(OP_READ, req, sizeof(r) + plen, &p, &L);
    free(req);
    if (rc < 0) { free(p); return rc; }
    memcpy(buf, p, L);
    free(p);
    return rc;   /* status holds the byte count on success */
}

static int n_write(const char *path, const char *buf, size_t size, off_t off,
                   struct fuse_file_info *fi) {
    (void)fi;
    size_t plen = strlen(path) + 1;
    uffs_write_req_t r = { .offset = off, .size = size };
    size_t total = sizeof(r) + plen + size;
    char *req = malloc(total);
    memcpy(req, &r, sizeof(r));
    memcpy(req + sizeof(r), path, plen);
    memcpy(req + sizeof(r) + plen, buf, size);
    int rc = rpc(OP_WRITE, req, total, NULL, NULL);
    free(req);
    return rc;
}

static int n_truncate(const char *path, off_t size, struct fuse_file_info *fi) {
    (void)fi;
    size_t plen = strlen(path) + 1;
    uffs_truncate_req_t r = { .size = size };
    char *req = malloc(sizeof(r) + plen);
    memcpy(req, &r, sizeof(r));
    memcpy(req + sizeof(r), path, plen);
    int rc = rpc(OP_TRUNCATE, req, sizeof(r) + plen, NULL, NULL);
    free(req);
    return rc < 0 ? rc : 0;
}

static int n_rename(const char *from, const char *to, unsigned int flags) {
    if (flags) return -EINVAL;
    size_t fl = strlen(from) + 1, tl = strlen(to) + 1;
    char *buf = malloc(fl + tl);
    memcpy(buf, from, fl);
    memcpy(buf + fl, to, tl);
    int rc = rpc(OP_RENAME, buf, fl + tl, NULL, NULL);
    free(buf);
    return rc < 0 ? rc : 0;
}

/* ---- connection setup ------------------------------------------------- */

static int connect_core(const char *path) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect"); close(fd); return -1;
    }
    return fd;
}

static void *n_init(struct fuse_conn_info *conn, struct fuse_config *cfg) {
    (void)conn; (void)cfg;
    return NULL;
}

static const struct fuse_operations node_ops = {
    .init     = n_init,
    .getattr  = n_getattr,
    .readdir  = n_readdir,
    .create   = n_create,
    .mkdir    = n_mkdir,
    .unlink   = n_unlink,
    .rmdir    = n_rmdir,
    .open     = n_open,
    .read     = n_read,
    .write    = n_write,
    .truncate = n_truncate,
    .rename   = n_rename,
};

struct node_opts { char *core; };
#define OPT(t, p) { t, offsetof(struct node_opts, p), 1 }
static const struct fuse_opt opt_spec[] = {
    OPT("core=%s", core),
    FUSE_OPT_END
};

int main(int argc, char *argv[]) {
    struct fuse_args args = FUSE_ARGS_INIT(argc, argv);
    struct node_opts opts = {0};
    if (fuse_opt_parse(&args, &opts, opt_spec, NULL) == -1) return 1;
    core_path = opts.core ? opts.core : (char *)UFFS_SOCK_DEFAULT;

    core_fd = connect_core(core_path);
    if (core_fd < 0) {
        fprintf(stderr, "uffs_node: cannot connect to core at %s\n", core_path);
        return 1;
    }
    fprintf(stderr, "uffs_node: connected to core at %s\n", core_path);
    return fuse_main(args.argc, args.argv, &node_ops, NULL);
}
