#!/bin/sh

/bin/busybox mount -t proc proc /proc
/bin/busybox mount -t sysfs sysfs /sys
/bin/busybox mount -t devtmpfs devtmpfs /dev
/bin/busybox ip link set lo up
/bin/busybox ip route add 198.51.100.2/32 dev lo

if /bin/busybox insmod /toa.ko && [ -r /proc/net/toa_stats ] && /toa-e2e && /bin/busybox rmmod toa; then
    echo TOA_E2E_PASS
else
    echo TOA_E2E_FAIL
fi
/bin/busybox poweroff -f
