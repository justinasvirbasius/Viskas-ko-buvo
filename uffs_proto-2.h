/*
 * uffs_proto.h — wire protocol between the node (FUSE-facing) and the core
 * (storage-owning). Synchronous request/reply over a Unix socket.
 *
 * Frame format on the wire:
 *   uint32_t total_length    // length of everything after this field
 *   uint32_t opcode
 *   uint32_t status          // 0 on req; -errno on reply
 *   uint32_t payload_len
 *   uint8_t  payload[payload_len]
 *
 * For each opcode the payload is just the C struct laid out below, then any
 * trailing bytes (e.g. write data, path strings). This keeps the parser
 * dumb. It is NOT endian-safe across architectures; both ends run on the
 * same host.
 */
#ifndef UFFS_PROTO_H
#define UFFS_PROTO_H

#include <stdint.h>
#include <sys/types.h>

#define UFFS_SOCK_DEFAULT "/tmp/uffs_core.sock"
#define UFFS_MAX_PATH      4096
#define UFFS_MAX_IO       (256 * 1024)

enum {
    OP_GETATTR = 1,
    OP_READDIR,
    OP_CREATE,
    OP_MKDIR,
    OP_UNLINK,
    OP_RMDIR,
    OP_READ,
    OP_WRITE,
    OP_TRUNCATE,
    OP_RENAME,
};

typedef struct {
    uint32_t length;
    uint32_t opcode;
    int32_t  status;
    uint32_t payload_len;
} uffs_frame_hdr_t;

/* getattr reply payload */
typedef struct {
    uint32_t mode;        /* full mode incl. S_IFDIR/S_IFREG/S_IFLNK */
    uint32_t nlink;
    uint32_t uid;
    uint32_t gid;
    uint64_t size;
    int64_t  atime, mtime, ctime;
    uint64_t ino;
} uffs_attr_t;

/* request payloads (path always trails as a NUL-terminated string) */
typedef struct { uint32_t mode; } uffs_create_req_t;
typedef struct { uint64_t offset; uint32_t size; } uffs_read_req_t;
typedef struct { uint64_t offset; uint32_t size; /* + data follows */ } uffs_write_req_t;
typedef struct { uint64_t size; } uffs_truncate_req_t;
/* rename: payload is "from\0to\0" */

#endif
