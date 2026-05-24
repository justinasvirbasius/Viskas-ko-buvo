/*
 * uffs_node_ll.c — low-level FUSE node with cache-invalidation support.
 *
 * Replaces the high-level uffs_node.c for use against uffs_core_full
 * when you want the kernel's attribute cache enabled. The node opens
 * TWO sockets to the core:
 *   - RPC socket: ordinary request-reply, same wire format as before.
 *   - Push socket: core sends invalidation messages whenever an inode
 *     changes. A background thread reads them and calls
 *     fuse_lowlevel_notify_inval_inode() so the kernel drops its cache.
 *
 * The push channel uses uffs_push_t framing (see uffs_proto.h).
 *
 * Build:
 *   cc -Wall -O2 -pthread uffs_node_ll.c \
 *      `pkg-config fuse3 --cflags --libs` -o uffs_node_ll
 *
 * Run (against a core that supports the push channel):
 *   ./uffs_node_ll -f -o core=/tmp/uffs_core.sock /tmp/mnt
 *
 * No need for entry_timeout=0,attr_timeout=0 — the kernel cache is on
 * and the core invalidates entries as they change.
 *
 * Caveats:
 *   - Uses path strings to talk to the core (which is still path-based).
 *     A real production node would speak in inode numbers end-to-end.
 *   - Path cache is unbounded; for a teaching artifact this is fine.
 *   - Crash recovery of the push channel: if the core restarts, this
 *     node should reconnect, but currently doesn't (TODO).
 */
#define FUSE_USE_VERSION 31
#define _GNU_SOURCE
#include "uffs_proto.h"

#include <fuse_lowlevel.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>

/* ---- core connection ------------------------------------------------- */

static int            rpc_fd  = -1;
static int            push_fd = -1;
static pthread_mutex_t rpc_mu = PTHREAD_MUTEX_INITIALIZER;
static struct fuse_session *g_session;   /* needed for notify calls */

/* ---- inode <-> path mapping ----------------------------------------- *
 * The kernel addresses inodes by fuse_ino_t. We use the core's stable
 * inode number as the fuse_ino_t directly. To talk to the path-based
 * core, we keep a table mapping fuse_ino_t → full path. The kernel
 * may forget inodes via op_forget; we drop the table entry there. */
#define MAX_INODES 4096
static char *path_for[MAX_INODES];
static pthread_mutex_t path_mu = PTHREAD_MUTEX_INITIALIZER;

static void path_set(uint64_t ino, const char *path) {
    if (ino == 0 || ino >= MAX_INODES) return;
    pthread_mutex_lock(&path_mu);
    free(path_for[ino]);
    path_for[ino] = strdup(path);
    pthread_mutex_unlock(&path_mu);
}
static char *path_get(uint64_t ino) {
    if (ino == 0 || ino >= MAX_INODES) return NULL;
    pthread_mutex_lock(&path_mu);
    char *p = path_for[ino] ? strdup(path_for[ino]) : NULL;
    pthread_mutex_unlock(&path_mu);
    return p;
}
static void path_drop(uint64_t ino) {
    if (ino == 0 || ino >= MAX_INODES) return;
    pthread_mutex_lock(&path_mu);
    free(path_for[ino]);
    path_for[ino] = NULL;
    pthread_mutex_unlock(&path_mu);
}

/* ---- low-level wire I/O --------------------------------------------- */

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

static int rpc(uint32_t op, const void *payload, uint32_t plen,
               void **out_payload, uint32_t *out_len) {
    pthread_mutex_lock(&rpc_mu);
    uffs_frame_hdr_t h = {
        .length = sizeof(h) - 4 + plen,
        .opcode = op, .status = 0, .payload_len = plen,
    };
    if (write_all(rpc_fd, &h, sizeof(h)) < 0) goto bad;
    if (plen && write_all(rpc_fd, payload, plen) < 0) goto bad;
    uffs_frame_hdr_t r;
    if (read_all(rpc_fd, &r, sizeof(r)) < 0) goto bad;
    void *buf = NULL;
    if (r.payload_len) {
        buf = malloc(r.payload_len);
        if (!buf) goto bad;
        if (read_all(rpc_fd, buf, r.payload_len) < 0) { free(buf); goto bad; }
    }
    pthread_mutex_unlock(&rpc_mu);
    if (out_payload) *out_payload = buf; else free(buf);
    if (out_len) *out_len = r.payload_len;
    return r.status;
bad:
    pthread_mutex_unlock(&rpc_mu);
    return -EIO;
}
static int rpc_path(uint32_t op, const char *path, void **op_out, uint32_t *ol_out) {
    return rpc(op, path, strlen(path) + 1, op_out, ol_out);
}

/* ---- push channel thread -------------------------------------------- */

static void *push_thread(void *arg) {
    (void)arg;
    uffs_push_t msg;
    while (read_all(push_fd, &msg, sizeof(msg)) == 0) {
        if (msg.opcode == PUSH_INVAL_INODE) {
            /* Tell the kernel to drop cached attrs + data for this inode.
             * -1, 0 means "invalidate all data" — for a teaching artifact
             * we're aggressive; a production system might invalidate
             * narrower ranges. */
            int rc = fuse_lowlevel_notify_inval_inode(g_session, msg.ino, -1, 0);
            if (rc < 0 && rc != -ENOENT) {
                /* -ENOENT just means the kernel doesn't have this inode
                 * cached, which is fine. */
                fprintf(stderr, "uffs_node_ll: inval_inode(%lu) failed: %d\n",
                        (unsigned long)msg.ino, rc);
            }
        }
    }
    fprintf(stderr, "uffs_node_ll: push channel closed\n");
    return NULL;
}

/* ---- helpers --------------------------------------------------------- */

static void fill_stat_from_attr(uffs_attr_t *a, struct stat *st) {
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
}

/* Resolve the path of a child given its parent's inode + name. */
static char *child_path(uint64_t parent_ino, const char *name) {
    char *parent = path_get(parent_ino);
    if (!parent) return NULL;
    int parent_len = strlen(parent);
    int need = parent_len + 1 + strlen(name) + 1;
    char *out = malloc(need);
    if (!out) { free(parent); return NULL; }
    if (strcmp(parent, "/") == 0) snprintf(out, need, "/%s", name);
    else                          snprintf(out, need, "%s/%s", parent, name);
    free(parent);
    return out;
}

/* ====================================================================== */
/*  low-level FUSE ops                                                    */
/* ====================================================================== */

static void ll_init(void *userdata, struct fuse_conn_info *conn) {
    (void)userdata; (void)conn;
    /* Root inode is always 1 in our world. Seed the table. */
    path_set(1, "/");
}

static void ll_destroy(void *userdata) {
    (void)userdata;
    fprintf(stderr, "uffs_node_ll: shutting down\n");
}

static void ll_lookup(fuse_req_t req, fuse_ino_t parent, const char *name) {
    char *p = child_path(parent, name);
    if (!p) { fuse_reply_err(req, ENOENT); return; }

    void *buf = NULL; uint32_t len = 0;
    int rc = rpc_path(OP_GETATTR, p, &buf, &len);
    if (rc < 0 || len < sizeof(uffs_attr_t)) {
        free(buf); free(p);
        fuse_reply_err(req, rc < 0 ? -rc : EIO);
        return;
    }
    uffs_attr_t *a = buf;
    struct fuse_entry_param e = {0};
    e.ino = a->ino;
    e.attr_timeout = 60.0;     /* now we can afford big timeouts */
    e.entry_timeout = 60.0;
    fill_stat_from_attr(a, &e.attr);
    path_set(a->ino, p);
    free(buf);
    free(p);
    fuse_reply_entry(req, &e);
}

static void ll_forget(fuse_req_t req, fuse_ino_t ino, uint64_t nlookup) {
    (void)nlookup;
    if (ino != 1) path_drop(ino);   /* never forget root */
    fuse_reply_none(req);
}

static void ll_getattr(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi) {
    (void)fi;
    char *p = path_get(ino);
    if (!p) { fuse_reply_err(req, ENOENT); return; }
    void *buf = NULL; uint32_t len = 0;
    int rc = rpc_path(OP_GETATTR, p, &buf, &len);
    free(p);
    if (rc < 0 || len < sizeof(uffs_attr_t)) {
        free(buf);
        fuse_reply_err(req, rc < 0 ? -rc : EIO);
        return;
    }
    struct stat st;
    fill_stat_from_attr(buf, &st);
    free(buf);
    fuse_reply_attr(req, &st, 60.0);
}

static void ll_readdir(fuse_req_t req, fuse_ino_t ino, size_t size, off_t off,
                       struct fuse_file_info *fi) {
    (void)fi;
    char *p = path_get(ino);
    if (!p) { fuse_reply_err(req, ENOENT); return; }
    void *buf = NULL; uint32_t len = 0;
    int rc = rpc_path(OP_READDIR, p, &buf, &len);
    if (rc < 0) { free(p); free(buf); fuse_reply_err(req, -rc); return; }

    /* Build the FUSE-formatted directory reply. */
    char *out = malloc(size);
    if (!out) { free(p); free(buf); fuse_reply_err(req, ENOMEM); return; }
    size_t used = 0;
    off_t entry_off = 0;

    /* "." and ".." come first */
    struct stat sd = {0}; sd.st_ino = ino; sd.st_mode = S_IFDIR;
    if (entry_off >= off) {
        size_t n = fuse_add_direntry(req, out + used, size - used, ".", &sd, ++entry_off);
        if (n <= size - used) used += n;
    } else { entry_off++; }
    if (entry_off >= off) {
        size_t n = fuse_add_direntry(req, out + used, size - used, "..", &sd, ++entry_off);
        if (n <= size - used) used += n;
    } else { entry_off++; }

    /* Then names from the core. */
    const char *s = buf;
    while (s < (const char *)buf + len && *s) {
        if (entry_off >= off) {
            struct stat sf = {0};
            sf.st_mode = S_IFREG;   /* approximation; kernel will refetch */
            size_t n = fuse_add_direntry(req, out + used, size - used,
                                         s, &sf, ++entry_off);
            if (n > size - used) break;
            used += n;
        } else { entry_off++; }
        s += strlen(s) + 1;
    }
    free(p); free(buf);
    fuse_reply_buf(req, out, used);
    free(out);
}

static void ll_open(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi) {
    /* Could getattr to confirm it exists, but the kernel already called
     * lookup so we trust it. */
    (void)ino;
    fuse_reply_open(req, fi);
}

static void ll_read(fuse_req_t req, fuse_ino_t ino, size_t size, off_t off,
                    struct fuse_file_info *fi) {
    (void)fi;
    char *p = path_get(ino);
    if (!p) { fuse_reply_err(req, ENOENT); return; }
    size_t plen = strlen(p) + 1;
    uffs_read_req_t r = { .offset = off, .size = size };
    char *req_buf = malloc(sizeof(r) + plen);
    memcpy(req_buf, &r, sizeof(r));
    memcpy(req_buf + sizeof(r), p, plen);
    void *out = NULL; uint32_t out_len = 0;
    int rc = rpc(OP_READ, req_buf, sizeof(r) + plen, &out, &out_len);
    free(req_buf); free(p);
    if (rc < 0) { free(out); fuse_reply_err(req, -rc); return; }
    fuse_reply_buf(req, out, out_len);
    free(out);
}

static void ll_write(fuse_req_t req, fuse_ino_t ino, const char *buf, size_t size,
                     off_t off, struct fuse_file_info *fi) {
    (void)fi;
    char *p = path_get(ino);
    if (!p) { fuse_reply_err(req, ENOENT); return; }
    size_t plen = strlen(p) + 1;
    uffs_write_req_t r = { .offset = off, .size = size };
    char *req_buf = malloc(sizeof(r) + plen + size);
    memcpy(req_buf, &r, sizeof(r));
    memcpy(req_buf + sizeof(r), p, plen);
    memcpy(req_buf + sizeof(r) + plen, buf, size);
    int rc = rpc(OP_WRITE, req_buf, sizeof(r) + plen + size, NULL, NULL);
    free(req_buf); free(p);
    if (rc < 0) fuse_reply_err(req, -rc);
    else        fuse_reply_write(req, rc);
}

static void ll_create(fuse_req_t req, fuse_ino_t parent, const char *name,
                      mode_t mode, struct fuse_file_info *fi) {
    char *p = child_path(parent, name);
    if (!p) { fuse_reply_err(req, ENOENT); return; }
    size_t plen = strlen(p) + 1;
    uffs_create_req_t r = { .mode = mode };
    char *buf = malloc(sizeof(r) + plen);
    memcpy(buf, &r, sizeof(r));
    memcpy(buf + sizeof(r), p, plen);
    int rc = rpc(OP_CREATE, buf, sizeof(r) + plen, NULL, NULL);
    free(buf);
    if (rc < 0) { free(p); fuse_reply_err(req, -rc); return; }
    /* re-lookup to get the new inode number */
    void *out = NULL; uint32_t out_len = 0;
    rc = rpc_path(OP_GETATTR, p, &out, &out_len);
    if (rc < 0 || out_len < sizeof(uffs_attr_t)) {
        free(out); free(p);
        fuse_reply_err(req, EIO); return;
    }
    uffs_attr_t *a = out;
    struct fuse_entry_param e = {0};
    e.ino = a->ino;
    e.attr_timeout = 60.0; e.entry_timeout = 60.0;
    fill_stat_from_attr(a, &e.attr);
    path_set(a->ino, p);
    free(out); free(p);
    fuse_reply_create(req, &e, fi);
}

static void ll_mkdir(fuse_req_t req, fuse_ino_t parent, const char *name,
                     mode_t mode) {
    char *p = child_path(parent, name);
    if (!p) { fuse_reply_err(req, ENOENT); return; }
    size_t plen = strlen(p) + 1;
    uffs_create_req_t r = { .mode = mode };
    char *buf = malloc(sizeof(r) + plen);
    memcpy(buf, &r, sizeof(r));
    memcpy(buf + sizeof(r), p, plen);
    int rc = rpc(OP_MKDIR, buf, sizeof(r) + plen, NULL, NULL);
    free(buf);
    if (rc < 0) { free(p); fuse_reply_err(req, -rc); return; }
    void *out = NULL; uint32_t ol = 0;
    rc = rpc_path(OP_GETATTR, p, &out, &ol);
    if (rc < 0 || ol < sizeof(uffs_attr_t)) {
        free(out); free(p); fuse_reply_err(req, EIO); return;
    }
    uffs_attr_t *a = out;
    struct fuse_entry_param e = {0};
    e.ino = a->ino;
    e.attr_timeout = 60.0; e.entry_timeout = 60.0;
    fill_stat_from_attr(a, &e.attr);
    path_set(a->ino, p);
    free(out); free(p);
    fuse_reply_entry(req, &e);
}

static void ll_unlink(fuse_req_t req, fuse_ino_t parent, const char *name) {
    char *p = child_path(parent, name);
    if (!p) { fuse_reply_err(req, ENOENT); return; }
    int rc = rpc_path(OP_UNLINK, p, NULL, NULL);
    free(p);
    fuse_reply_err(req, rc < 0 ? -rc : 0);
}

static void ll_rmdir(fuse_req_t req, fuse_ino_t parent, const char *name) {
    char *p = child_path(parent, name);
    if (!p) { fuse_reply_err(req, ENOENT); return; }
    int rc = rpc_path(OP_RMDIR, p, NULL, NULL);
    free(p);
    fuse_reply_err(req, rc < 0 ? -rc : 0);
}

static void ll_rename(fuse_req_t req, fuse_ino_t parent, const char *name,
                      fuse_ino_t newparent, const char *newname,
                      unsigned int flags) {
    if (flags) { fuse_reply_err(req, EINVAL); return; }
    char *from = child_path(parent, name);
    char *to   = child_path(newparent, newname);
    if (!from || !to) {
        free(from); free(to);
        fuse_reply_err(req, ENOENT);
        return;
    }
    size_t fl = strlen(from) + 1, tl = strlen(to) + 1;
    char *buf = malloc(fl + tl);
    memcpy(buf, from, fl);
    memcpy(buf + fl, to, tl);
    int rc = rpc(OP_RENAME, buf, fl + tl, NULL, NULL);
    free(buf); free(from); free(to);
    fuse_reply_err(req, rc < 0 ? -rc : 0);
}

static const struct fuse_lowlevel_ops ll_ops = {
    .init    = ll_init,
    .destroy = ll_destroy,
    .lookup  = ll_lookup,
    .forget  = ll_forget,
    .getattr = ll_getattr,
    .readdir = ll_readdir,
    .open    = ll_open,
    .read    = ll_read,
    .write   = ll_write,
    .create  = ll_create,
    .mkdir   = ll_mkdir,
    .unlink  = ll_unlink,
    .rmdir   = ll_rmdir,
    .rename  = ll_rename,
};

/* ---- connection setup ----------------------------------------------- */

static int connect_unix(const char *path) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect"); close(fd); return -1;
    }
    return fd;
}

/* ---- main ----------------------------------------------------------- */

struct opt_holder { char *core; };
#define OPT(t, p) { t, offsetof(struct opt_holder, p), 1 }
static const struct fuse_opt opt_spec[] = {
    OPT("core=%s", core),
    FUSE_OPT_END
};

int main(int argc, char *argv[]) {
    struct fuse_args args = FUSE_ARGS_INIT(argc, argv);
    struct opt_holder opts = {0};
    if (fuse_opt_parse(&args, &opts, opt_spec, NULL) == -1) return 1;
    const char *core_path = opts.core ? opts.core : UFFS_SOCK_DEFAULT;

    rpc_fd = connect_unix(core_path);
    if (rpc_fd < 0) {
        fprintf(stderr, "uffs_node_ll: cannot connect RPC socket %s\n", core_path);
        return 1;
    }
    /* push channel: connect to the same socket path, second connection.
     * The core distinguishes them by sending PUSH_HELLO first. */
    push_fd = connect_unix(core_path);
    if (push_fd < 0) {
        fprintf(stderr, "uffs_node_ll: cannot connect push socket\n");
        return 1;
    }
    /* Tell the core "this connection is for pushes" by sending a single
     * marker frame: a regular frame header with opcode 0 (reserved). */
    uffs_frame_hdr_t hello = {
        .length = sizeof(hello) - 4, .opcode = 0, .status = 0, .payload_len = 0
    };
    if (write_all(push_fd, &hello, sizeof(hello)) < 0) {
        fprintf(stderr, "uffs_node_ll: push hello failed\n");
        return 1;
    }
    fprintf(stderr, "uffs_node_ll: connected (RPC + push) to %s\n", core_path);

    /* Set up FUSE session BEFORE starting the push thread so g_session
     * is initialized when push messages start arriving. */
    struct fuse_cmdline_opts cli_opts;
    if (fuse_parse_cmdline(&args, &cli_opts) != 0) return 1;
    if (!cli_opts.mountpoint) {
        fprintf(stderr, "usage: uffs_node_ll -o core=PATH MOUNTPOINT\n");
        return 1;
    }

    g_session = fuse_session_new(&args, &ll_ops, sizeof(ll_ops), NULL);
    if (!g_session) return 1;
    if (fuse_session_mount(g_session, cli_opts.mountpoint) != 0) return 1;
    fuse_daemonize(cli_opts.foreground);

    /* Now launch the push reader. */
    pthread_t pt;
    pthread_create(&pt, NULL, push_thread, NULL);
    pthread_detach(pt);

    int rc = cli_opts.singlethread
        ? fuse_session_loop(g_session)
        : fuse_session_loop_mt(g_session, &(struct fuse_loop_config){ .clone_fd = 0 });

    fuse_session_unmount(g_session);
    fuse_session_destroy(g_session);
    free(cli_opts.mountpoint);
    return rc;
}
