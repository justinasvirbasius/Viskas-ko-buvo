# CRWG Upgrade Roadmap

Per project direction: **daemon tooling and entities are upgraded first; distro entities and broader guarantees (rechts) come after.**

The base build (`./build.sh all`) gets you a bootable system with kernel, init, BusyBox userland, and a placeholder for the package manager. Everything below is sequenced for *after* that boots cleanly.

## Stage 0 — Base (current)

- Linux 6.6 LTS, monolithic, no modules
- musl libc
- BusyBox init + coreutils
- SYSLINUX bootloader
- ~10 MB bootable ISO target

Verify: `./scripts/run-qemu.sh` boots to a shell prompt.

## Stage 1 — Daemon tooling (PRIORITY)

This is the first upgrade target. Goal: a real supervision layer so services can be defined, started, restarted, and logged.

1. **Service supervisor.** Add `runit` or `s6` (both small, both static-linkable against musl, both better than SysV scripts). Lean toward `s6` for its supervision tree and readiness protocol.
2. **Logger.** `s6-log` or `svlogd` — per-service log directories with rotation. No syslog daemon yet.
3. **Service definitions.** Convert `rcS`/`rcK` to service directories under `/etc/service/`.
4. **Network daemon hooks.** `udhcpc` (already in BusyBox) wrapped as a supervised service.
5. **SSH.** Add `dropbear` (static, musl-friendly) as the first real network daemon. Supervised.
6. **Cron.** BusyBox `crond` supervised.

Deliverable for this stage: services survive crashes, log to disk, come up in a defined order.

## Stage 2 — Package manager

Bootstrap `apk-tools` from Alpine, then host our own repo.

1. Cross-build `apk-tools` static against musl.
2. Generate a signing key. **Unsigned packages are refused, no override flag.** This is the first concrete "guarantee" the distro name promises.
3. Build a tiny repo with: kernel, busybox, dropbear, s6, ca-certificates.
4. `apk add` / `apk del` / `apk upgrade` working end-to-end.
5. Reproducible builds: pinned source URLs + SHA256 in every package recipe.

## Stage 3 — Distro entities (deprioritized per direction)

These are the social/structural pieces of being a distro, intentionally deferred:

- Release channels (stable / edge)
- Mirror infrastructure
- Bug tracker, package maintainer roles
- Security advisory format
- Reproducible-build verification by third parties
- ABI stability policy

## Stage 4 — Guarantees (the "G" in CRWG)

What "with guarantees" means concretely, formalized only after Stage 1–2 work:

- **Boot integrity.** Measured boot via TPM, kernel + initramfs hashes published per release.
- **Package signatures.** Already enforced from Stage 2; document threat model.
- **No silent module loading.** Kernel built without `CONFIG_MODULES` — fixed at boot, cannot be extended at runtime. This is intentional and load-bearing for the integrity story.
- **Determinism.** Same source + same toolchain → byte-identical ISO. Hash published.
- **Audit log.** Every `apk` transaction journaled to append-only log on disk.

## Explicitly out of scope (for now)

- systemd
- glibc
- GUI / Wayland / X
- Loadable kernel modules
- Multi-arch (x86_64 only until Stage 2 is solid)

## Sanity checks before each stage

- ISO still under 25 MB through Stage 2
- Boot to shell under 3 seconds in QEMU
- No new daemon added without a supervisor definition
- No new package added without a signed recipe
