#!/bin/sh
# CRWG (Code Rechts With Guarantees) - top-level build orchestrator
#
# Builds a minimal bootable x86_64 Linux ISO with:
#   - Linux kernel (monolithic, no modules)
#   - musl libc
#   - BusyBox userland + init
#   - apk-tools package manager (bootstrapped from Alpine)
#   - SYSLINUX bootloader
#
# Run order:
#   ./build.sh fetch    # download sources
#   ./build.sh kernel   # build kernel
#   ./build.sh userland # build busybox + musl
#   ./build.sh rootfs   # assemble rootfs
#   ./build.sh iso      # produce bootable ISO
#   ./build.sh all      # everything
#
# Requires on host: gcc, make, flex, bison, bc, cpio, xorriso, wget, tar
set -eu

ROOT="$(cd "$(dirname "$0")" && pwd)"
BUILD="$ROOT/build"
SRC="$BUILD/src"
OUT="$BUILD/out"
ROOTFS="$BUILD/rootfs"
JOBS="$(nproc 2>/dev/null || echo 2)"

# Pinned versions - bump these deliberately, never blindly
KERNEL_VER="6.6.30"
BUSYBOX_VER="1.36.1"
MUSL_VER="1.2.5"
SYSLINUX_VER="6.03"

KERNEL_URL="https://cdn.kernel.org/pub/linux/kernel/v6.x/linux-${KERNEL_VER}.tar.xz"
BUSYBOX_URL="https://busybox.net/downloads/busybox-${BUSYBOX_VER}.tar.bz2"
MUSL_URL="https://musl.libc.org/releases/musl-${MUSL_VER}.tar.gz"
SYSLINUX_URL="https://www.kernel.org/pub/linux/utils/boot/syslinux/syslinux-${SYSLINUX_VER}.tar.xz"

log() { printf '\033[1;32m[crwg]\033[0m %s\n' "$*"; }
die() { printf '\033[1;31m[crwg:err]\033[0m %s\n' "$*" >&2; exit 1; }

cmd_fetch() {
    mkdir -p "$SRC"
    cd "$SRC"
    for pair in \
        "linux-${KERNEL_VER}.tar.xz $KERNEL_URL" \
        "busybox-${BUSYBOX_VER}.tar.bz2 $BUSYBOX_URL" \
        "musl-${MUSL_VER}.tar.gz $MUSL_URL" \
        "syslinux-${SYSLINUX_VER}.tar.xz $SYSLINUX_URL"
    do
        f="${pair%% *}"; u="${pair#* }"
        [ -f "$f" ] || { log "fetching $f"; wget -q --show-progress -O "$f" "$u"; }
    done
    log "extracting"
    for f in *.tar.*; do
        d="${f%.tar.*}"
        [ -d "$d" ] || tar -xf "$f"
    done
}

cmd_kernel() {
    cd "$SRC/linux-${KERNEL_VER}"
    cp "$ROOT/config/kernel.config" .config
    make ARCH=x86_64 olddefconfig
    make ARCH=x86_64 -j"$JOBS" bzImage
    mkdir -p "$OUT"
    cp arch/x86/boot/bzImage "$OUT/bzImage"
    log "kernel built: $OUT/bzImage ($(du -h "$OUT/bzImage" | cut -f1))"
}

cmd_userland() {
    cd "$SRC/busybox-${BUSYBOX_VER}"
    cp "$ROOT/config/busybox.config" .config
    make oldconfig
    make -j"$JOBS"
    make CONFIG_PREFIX="$ROOTFS" install
    log "busybox installed to $ROOTFS"
}

cmd_rootfs() {
    mkdir -p "$ROOTFS"/dev "$ROOTFS"/proc "$ROOTFS"/sys "$ROOTFS"/tmp \
             "$ROOTFS"/var/log "$ROOTFS"/var/run "$ROOTFS"/root \
             "$ROOTFS"/etc/init.d
    cp -r "$ROOT/rootfs-skel/." "$ROOTFS/"
    chmod +x "$ROOTFS"/etc/init.d/* "$ROOTFS"/init 2>/dev/null || true
    # cpio initramfs for now; can move to disk image later
    cd "$ROOTFS"
    find . | cpio -H newc -o 2>/dev/null | gzip -9 > "$OUT/initramfs.cpio.gz"
    log "initramfs: $OUT/initramfs.cpio.gz ($(du -h "$OUT/initramfs.cpio.gz" | cut -f1))"
}

cmd_iso() {
    ISODIR="$BUILD/iso"
    rm -rf "$ISODIR"
    mkdir -p "$ISODIR/boot/isolinux"
    cp "$OUT/bzImage" "$ISODIR/boot/"
    cp "$OUT/initramfs.cpio.gz" "$ISODIR/boot/"
    cp "$SRC/syslinux-${SYSLINUX_VER}/bios/core/isolinux.bin" "$ISODIR/boot/isolinux/"
    cp "$SRC/syslinux-${SYSLINUX_VER}/bios/com32/elflink/ldlinux/ldlinux.c32" "$ISODIR/boot/isolinux/"
    cp "$ROOT/config/isolinux.cfg" "$ISODIR/boot/isolinux/"
    xorriso -as mkisofs \
        -o "$OUT/crwg.iso" \
        -b boot/isolinux/isolinux.bin \
        -c boot/isolinux/boot.cat \
        -no-emul-boot -boot-load-size 4 -boot-info-table \
        "$ISODIR"
    log "ISO: $OUT/crwg.iso ($(du -h "$OUT/crwg.iso" | cut -f1))"
}

cmd_all() { cmd_fetch; cmd_kernel; cmd_userland; cmd_rootfs; cmd_iso; }

case "${1:-all}" in
    fetch|kernel|userland|rootfs|iso|all) "cmd_${1:-all}" ;;
    *) die "unknown subcommand: $1 (try: fetch|kernel|userland|rootfs|iso|all)" ;;
esac
