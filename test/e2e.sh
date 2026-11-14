#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
TEST_DIR=$(mktemp -d)
cleanup() { rm -r -- "$TEST_DIR"; }
trap cleanup EXIT HUP INT TERM

kernel_release=${E2E_KERNEL_RELEASE:-}
if [ -z "$kernel_release" ]; then
    for headers in /lib/modules/*/build; do
        if [ -f "$headers/Makefile" ] && [ -f "/boot/vmlinuz-$(basename "$(dirname "$headers")")" ]; then
            kernel_release=$(basename "$(dirname "$headers")")
        fi
    done
fi
if [ -z "$kernel_release" ]; then
    echo 'a matching kernel image and headers are required' >&2
    exit 1
fi

mkdir -p "$TEST_DIR/src" "$TEST_DIR/root/bin" "$TEST_DIR/root/proc" "$TEST_DIR/root/sys" "$TEST_DIR/root/dev"
cp "$ROOT/Makefile" "$ROOT/toa.c" "$ROOT/toa.h" "$TEST_DIR/src/"
make -C "$TEST_DIR/src" KERNDIR="/lib/modules/$kernel_release/build" -j2
gcc -static -O2 -Wall -Wextra -o "$TEST_DIR/root/toa-e2e" "$ROOT/test/e2e.c"
cp "$TEST_DIR/src/toa.ko" "$TEST_DIR/root/toa.ko"
cp /bin/busybox "$TEST_DIR/root/bin/busybox"
ln -s busybox "$TEST_DIR/root/bin/sh"
cp "$ROOT/test/e2e-init.sh" "$TEST_DIR/root/init"
chmod +x "$TEST_DIR/root/init"
(
    cd "$TEST_DIR/root"
    find . -print0 | cpio --null -o -H newc 2>/dev/null | gzip -1 >"$TEST_DIR/initramfs.gz"
)
if ! timeout 120 qemu-system-x86_64 -machine accel=tcg -m 1024 -smp 2 -display none \
    -serial stdio -no-reboot -kernel "/boot/vmlinuz-$kernel_release" -initrd "$TEST_DIR/initramfs.gz" \
    -append 'console=ttyS0 panic=-1' >"$TEST_DIR/guest.log" 2>&1; then
    cat "$TEST_DIR/guest.log" >&2
    exit 1
fi
cat "$TEST_DIR/guest.log"
grep -q '^TOA_E2E_PASS' "$TEST_DIR/guest.log"
