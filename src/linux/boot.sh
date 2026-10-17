#!/bin/sh
# Runs from the ownlinux upstart job while dbus waits. Mounts our Alpine
# image, takes over the USB port and Wi-Fi chip, and runs own-init inside it.
# Leaving own Linux always goes through a reboot. Going back to the stock UI
# in place once left the image's loop device holding the userstore open, and
# the Kindle and the computer then both wrote to the same FAT partition.
# Lives at /mnt/us/linux/boot.sh.
D=/mnt/base-us/linux
IMG=$D/alpine.img
R=/mnt/own

# Run from a copy, so editing boot.sh on the userstore can't break a running boot.
case "$0" in
/tmp/*) ;;
*) cp "$0" /tmp/ownlinux-boot.sh && exec sh /tmp/ownlinux-boot.sh ;;
esac
exec >>"$D/boot.log" 2>&1
echo "=== $(date) boot"

# The home screen's "Kindle UI" button asks for one stock boot.
if [ -f "$D/STOCK_ONCE" ]; then
    rm -f "$D/STOCK_ONCE"
    sync
    echo "stock boot requested"
    exit 0
fi

# One shot: until own-init re-arms the flag, a hang or crash makes the next boot stock.
mv "$D/BOOT_OWN" "$D/BOOT_OWN.trying"
sync

mounted() { grep -q " $1 " /proc/mounts; }

# Stops everything in our system and fully lets go of alpine.img. Fails if the
# image is still open anywhere.
release() {
    for p in /proc/[0-9]*; do
        [ "$(readlink "$p/root")" = "$R" ] && kill -9 "${p#/proc/}" 2>/dev/null
    done
    sleep 1
    sync
    # Everything mounted inside the image, deepest first (own-init adds /tmp and /run).
    for m in $(awk -v r="$R/" 'index($2, r) == 1 {print $2}' /proc/mounts | sort -r); do umount "$m"; done
    mounted $R && umount -d $R
    for b in /sys/block/loop*/loop/backing_file; do
        grep -q alpine.img "$b" 2>/dev/null && losetup -d "/dev/loop/$(basename "$(dirname "$(dirname "$b")")" | sed 's/loop//')"
    done
    sync
    if mounted $R || grep -qs alpine.img /sys/block/loop*/loop/backing_file; then
        echo "alpine.img is still in use"
        return 1
    fi
}

rearm() { [ -f "$D/BOOT_OWN.trying" ] && mv "$D/BOOT_OWN.trying" "$D/BOOT_OWN"; }

[ -d $R ] || { mntroot rw; mkdir -p $R; mntroot ro; }
if ! mounted $R; then
    e2fsck -p "$IMG"
    mount -o loop,noatime -t ext3 "$IMG" $R || { echo "mount failed"; rearm; exit 1; }
fi
for m in dev dev/pts proc sys; do mounted "$R/$m" || mount -o bind "/$m" "$R/$m"; done
mkdir -p $R/mnt/us
mounted $R/mnt/us || mount -o bind /mnt/base-us $R/mnt/us

# The USB port becomes a network link instead of a drive, and the Wi-Fi chip
# is ours. The drivers load while the splash waits, which saves a second or two.
(
    lsmod | grep -q '^g_file_storage' && rmmod g_file_storage
    modprobe g_ether
    modprobe ath6kl_sdio
) &
drivers=$!

chroot $R /usr/local/bin/splash -w 1 "waking up" "press power now for the stock Kindle UI"
pressed=$?
wait $drivers
if [ $pressed -eq 1 ]; then
    echo "power pressed, booting stock this time"
    rmmod g_ether
    modprobe g_file_storage
    rearm
    # Nothing but the splash has run, so the stock boot can carry on in place.
    release && exit 0
    touch "$D/STOCK_ONCE"
    sync
    reboot
    exit 0
fi

# Amazon's cron and syslog have nothing to do in our system, and neither has
# fsp, its FUSE layer over the userstore (we use /mnt/base-us directly).
stop cron
stop syslog
umount -l /mnt/us
killall fsp 2>/dev/null
iptables -I INPUT -p tcp --dport 22 -j ACCEPT
iptables -I INPUT -p tcp --dport 2222 -j ACCEPT

chroot $R /usr/bin/env -i TERM=linux /sbin/own-init
rc=$?
echo "own-init exited $rc"

# 10 to 12 are choices made on the home screen, so own Linux stays the default.
# Anything else (13 is the crash guard) leaves the next boot stock.
case $rc in
10) rearm; touch "$D/STOCK_ONCE" ;;
11 | 12) rearm ;;
esac
sync
release
sync
if [ $rc -eq 12 ]; then poweroff; else reboot; fi
exit 0
