# UFFS — Universal Fast File System (skeleton)

A teaching-grade FUSE filesystem in five increasingly capable variants.
Each one is self-contained C, mountable on Linux (FreeBSD with minor
tweaks). None of these are production-ready. All of them are honest
starting points.

## The five variants

| File                  | Storage              | Concurrency        | Survives unmount? | Crash-safe? |
|-----------------------|----------------------|--------------------|-------------------|-------------|
| `uffs.c`              | RAM                  | single process     | no                | n/a         |
| `uffs_persist.c`      | mmap'd backing file  | single process     | yes               | no          |
| `uffs_core.c` + `uffs_node.c` | RAM (in core)        | 1 node ↔ 1 core    | no                | n/a         |
| `uffs_core_persist.c` + `uffs_node.c` | mmap'd backing file  | N nodes ↔ 1 core   | yes               | no          |
| `uffs_blocks.c`       | block-allocated image | single process    | yes               | no          |
| `uffs_journal.c`      | block-allocated + WAL | single process    | yes               | **yes** (metadata) |
| `uffs_core_full.c` + `uffs_node.c` | block-allocated + WAL | N nodes ↔ 1 core | yes | **yes** (metadata) |
| `uffs_core_full.c` + `uffs_node_ll.c` | same as above, with **push invalidation** | N nodes ↔ 1 core | yes | **yes** (metadata) |

The last row is the final form. The core pushes inode invalidations to
every connected node over a separate socket; the node uses FUSE's
low-level API to forward those invalidations to the kernel, which
drops its attribute cache for the affected inode. This lets you mount
with default cache timeouts (~1 second or more) and still see writes
from one node immediately on another.

The shared wire protocol lives in `uffs_proto.h`. The storage layers of
`uffs_blocks` and `uffs_journal` have no-FUSE smoke tests in
`uffs_blocks_test.c` and `uffs_journal_test.c`. `uffs_core_full` reuses
the same on-disk layout as `uffs_journal`, so existing images and the
journal-recovery test apply to it too.

## Build

```
make            # builds all five
```

You need `libfuse3-dev` (Debian/Ubuntu) or `fuse3-devel` (RHEL/Fedora).
The Makefile falls back to `-lfuse` for older systems but you may need
to adjust `FUSE_USE_VERSION`.

## Run each one

**Variant 1 — single-process in-memory:**
```
mkdir /tmp/uffs
./uffs -f /tmp/uffs
# in another shell: echo hi > /tmp/uffs/x ; cat /tmp/uffs/x
fusermount -u /tmp/uffs
```

**Variant 2 — single-process persistent:**
```
./uffs_persist -f -o backing=/tmp/uffs.img /tmp/uffs
# data in /tmp/uffs survives unmount; reopen the same image to resume
```

**Variant 3 — core/node split, in-memory:**
```
./uffs_core /tmp/uffs_core.sock &
./uffs_node -f -o core=/tmp/uffs_core.sock /tmp/uffs
```

**Variant 4 — core/node split, persistent, multi-node:**
```
./uffs_core_persist /tmp/uffs.img /tmp/uffs_core.sock &

# Mount node A. Note the cache-disable options — REQUIRED when
# multiple nodes share a core, or they will see stale data.
mkdir /tmp/mnt_A /tmp/mnt_B
./uffs_node -f -o core=/tmp/uffs_core.sock,entry_timeout=0,attr_timeout=0,ac_attr_timeout=0 /tmp/mnt_A &
./uffs_node -f -o core=/tmp/uffs_core.sock,entry_timeout=0,attr_timeout=0,ac_attr_timeout=0 /tmp/mnt_B &

# Write from one mount, read from the other:
echo hello > /tmp/mnt_A/x
cat /tmp/mnt_B/x        # → hello
```

**Variant 5 — real block-allocated on-disk format:**
```
./uffs_blocks -f -o backing=/tmp/uffs.img /tmp/uffs
# Image is 16 MiB. Max file ~4 MiB (12 direct + 1024 indirect blocks).
```

**Variant 6 — block-allocated with metadata journaling:**
```
./uffs_journal -f -o backing=/tmp/uffs.img /tmp/uffs
# Same layout as variant 5 plus a 64-block journal region.
# Crashes during metadata ops are recovered on remount.
```

**Variant 7 — the full thing: multi-node + persistent + crash-safe.**
```
./uffs_core_full /tmp/uffs.img /tmp/uffs_core.sock &
mkdir /tmp/mnt_A /tmp/mnt_B
./uffs_node -f -o core=/tmp/uffs_core.sock,entry_timeout=0,attr_timeout=0,ac_attr_timeout=0 /tmp/mnt_A &
./uffs_node -f -o core=/tmp/uffs_core.sock,entry_timeout=0,attr_timeout=0,ac_attr_timeout=0 /tmp/mnt_B &
# Same on-disk format as variant 6; images interchange between the two.
```

**Variant 8 — variant 7, plus push-based cache invalidation.**
```
./uffs_core_full /tmp/uffs.img /tmp/uffs_core.sock &
./uffs_node_ll -f -o core=/tmp/uffs_core.sock /tmp/mnt_A &
./uffs_node_ll -f -o core=/tmp/uffs_core.sock /tmp/mnt_B &
# No need for entry_timeout=0 — the core invalidates entries on mutation.
```

You can verify the storage layers without mounting:
```
make test    # runs both block-allocator and journal recovery tests
```

## Architecture, briefly

The skeleton's job is to expose the *shape* of a filesystem so you can
hang real engineering off it. Three layers:

1. **VFS face** (FUSE ops table) — kernel routes syscalls here.
2. **Filesystem logic** (path lookup, dir entries, inode lifecycle).
3. **Storage backend** — RAM, mmap'd file, or RPC to a remote core.

The split between 2 and 3 is the interesting one. In `uffs.c` and
`uffs_persist.c` they share an address space. In the core/node variants
they're separated by a socket, which makes "the storage layer" a
distinct, replaceable service. That's the architecture real
distributed filesystems (Ceph, Lustre, GlusterFS) extend further:
multiple cores sharding the namespace, replication between cores,
client-side caches with explicit invalidation, etc.

## Known limitations (every variant)

- File size cap: 1 MiB (RAM variants), 256 KiB (early persistent
  variants), or ~4 MiB (block-allocated variants 5–7, limited by
  the single indirect block).
- Crash safety only for **metadata** in variants 6 and 7 (data=ordered).
  Earlier persistent variants (2 and 4) use `MS_ASYNC` and can corrupt
  the inode table on power loss.
- No extended attributes, no ACLs, no quotas, no snapshots.
- Path lookup is O(N) in directory size (linear scan of dirents).
  Replace with a hash or B-tree once directories get large.
- Core/node: no auth on the socket, no reconnect.
- Single global lock on the core; one in-flight journal txn at a time.
- Cross-node coherency relies on disabling FUSE's kernel-side cache.
  The right fix is `fuse_lowlevel_notify_inval_inode` callbacks from
  the core, which requires switching the node to FUSE's low-level API.

## Where to push next

The big architectural pieces — block allocator, journal, multi-node,
push-based cache invalidation — are all in place. Variant 8 is the
final form. Remaining work, in order of how much it matters:

1. **Sharded cores.** Hash inode numbers across multiple core
   instances so the storage layer scales out.
2. **Replication.** Each core writes to a peer before acknowledging,
   so a single core crash doesn't lose data.
3. **Extents instead of indirect blocks.** Real modern filesystems
   (ext4, btrfs, XFS) use extents — (start_block, length) ranges —
   instead of per-block pointers. Bigger files, less metadata overhead.
4. **Journal checksums.** Right now a torn write that happens to
   leave plausible magic bytes will be accepted by replay.
5. **Concurrent journal.** Variant 7/8 serializes all transactions
   through one global lock; real designs interleave transactions
   with checkpointing.
6. **Crash-safe replay.** Current replay isn't itself idempotent
   under a crash during replay. Double-buffer or use a separate
   "replay-in-progress" marker.
7. **Inode-based core API.** The node still talks to the core using
   path strings; a production split would use inode numbers end-to-end
   for the node's own RPC traffic too. Removes the path cache and
   makes the protocol cheaper.

Each is a substantial project; doing them in the above order keeps the
system shippable at every step.
