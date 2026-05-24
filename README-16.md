# CRWG — Code Rechts With Guarantees

A minimal Linux distribution. Stage 0 produces a bootable x86_64 ISO around 10 MB.

## Layout

```
crwg/
├── build.sh              # main build orchestrator
├── config/
│   ├── kernel.config     # minimal monolithic kernel config
│   ├── busybox.config    # static busybox config
│   └── isolinux.cfg      # bootloader config
├── rootfs-skel/          # files copied verbatim into the rootfs
│   ├── init              # PID 1 entry point
│   └── etc/
│       ├── inittab
│       ├── os-release
│       ├── hostname
│       ├── passwd
│       ├── group
│       └── init.d/{rcS,rcK}
├── scripts/
│   └── run-qemu.sh       # boot the ISO in QEMU
└── ROADMAP.md            # upgrade plan (daemon tooling first)
```

## Building

Host needs: `gcc make flex bison bc cpio xorriso wget tar`.

```sh
./build.sh fetch      # download all sources
./build.sh kernel     # build the kernel
./build.sh userland   # build busybox
./build.sh rootfs     # assemble rootfs + initramfs
./build.sh iso        # produce the bootable ISO
# or
./build.sh all
```

Output lands in `build/out/crwg.iso`.

## Testing

```sh
./scripts/run-qemu.sh
```

You should see SYSLINUX, then the kernel boot, then a login on the serial console.

## What comes next

See `ROADMAP.md`. Short version: supervisor + service tooling first (s6/runit, dropbear, supervised cron), package manager second, distro infrastructure later.

## Design choices worth knowing

- **No loadable kernel modules.** `CONFIG_MODULES` is off. The kernel is fixed at boot — part of the integrity story, not an oversight.
- **musl, not glibc.** Smaller, simpler, easier to static-link.
- **BusyBox for now.** Will get swapped applet-by-applet as real daemons replace placeholders.
- **No systemd.** Out of scope.
