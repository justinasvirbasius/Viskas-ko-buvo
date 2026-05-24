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

The shared wire protocol lives in `uffs_proto.h`. The storage layers of
`uffs_blocks` and `uffs_journal` have no-FUSE smoke tests in
`uffs_blocks_test.c` and `uffs_journal_test.c`.

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

- File size capped (1 MiB in RAM variants, 256 KiB in persistent
  variants because file data lives inline in the inode).
- No journaling. `MS_ASYNC` is used in the persistent variants — a
  crash during write can corrupt the inode table.
- No proper free-block allocator. The next big leap is replacing the
  inline `data[UFFS_FILE_SLOT]` with a block pointer table plus a
  free-block bitmap.
- No extended attributes, no ACLs, no quotas, no snapshots.
- Path lookup is O(N) in directory size (linear scan of `children[]`).
  Replace with a hash table once directories get large.
- Core/node: no auth on the socket, no reconnect, single global lock
  on the core.
- Cross-node coherency relies on disabling FUSE's kernel-side cache.
  The right fix is `fuse_lowlevel_notify_inval_inode` callbacks from
  the core, which requires switching the node to FUSE's low-level API.

## Where to push next

The block allocator (variant 5) and metadata journal (variant 6) are
done. Remaining work, in order of how much it matters:

1. **Merge variants 4 and 6.** A core/node split that uses the
   block-allocated, journaled layout. Combines the distributed shape
   with the crash-safe shape. Mostly a refactor.
2. **Cache invalidation.** Move the node to FUSE low-level API and
   have the core push `inval_inode` to all nodes when an inode changes.
   Lets you re-enable the kernel attribute cache for performance.
3. **Sharded cores.** Hash inode numbers across multiple core
   instances so the storage layer scales out.
4. **Replication.** Each core writes to a peer before acknowledging,
   so a single core crash doesn't lose data.
5. **Extents instead of indirect blocks.** Real modern filesystems
   (ext4, btrfs, XFS) use extents — (start_block, length) ranges —
   instead of per-block pointers. Bigger files, less metadata overhead.
6. **Journal checksums.** Right now a torn write that happens to
   leave plausible magic bytes will be accepted by replay. Real
   journals checksum the descriptor + after-images.
7. **Concurrent ops.** Variants 5 and 6 are single-threaded inside
   FUSE; the journal in particular assumes one transaction in flight.
   Real designs interleave transactions with checkpointing.

Each of these is a substantial project on its own; doing them in the
above order keeps the system shippable at every step.
