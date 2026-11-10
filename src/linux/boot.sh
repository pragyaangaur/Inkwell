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
# Keep the log from growing on the userstore for ever: past 256 KB the old
# one moves to boot.log.1, which replaces the one before.
[ "$(wc -c < "$D/boot.log" 2>/dev/null || echo 0)" -gt 262144 ] && mv -f "$D/boot.log" "$D/boot.log.1"
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

# What differs between Kindles. The PW3 (kernel 3.0) shows the userstore over
# USB with g_file_storage and has an Atheros Wi-Fi chip. The PW4 (kernel 4.1)
# uses g_mass_storage with its own product id and a Broadcom chip, whose
# driver needs firmware paths. Its firmware lists all of that in
# platform_variables, which Amazon's own boot scripts read too.
eval "$(grep -E '^(wifim_START_CMD_wifi|BOARD_ID_PRODUCT|BOARD_STALL)=' /etc/sysconfig/platform_variables 2>/dev/null)"
USB_DRIVE=$(lsmod | awk '$1 == "g_file_storage" || $1 == "g_mass_storage" {print $1; exit}')
echo "board: $(tr '\0' ' ' < /proc/device-tree/compatible 2>/dev/null || echo pw3), usb drive ${USB_DRIVE:-none}"
usb_drive() {
    if [ "$USB_DRIVE" = g_mass_storage ]; then
        if [ "$BOARD_STALL" = 0 ]; then
            modprobe g_mass_storage file="" removable=1 idVendor=0x1949 idProduct=$BOARD_ID_PRODUCT stall=0
        else
            modprobe g_mass_storage file="" removable=1 idVendor=0x1949 idProduct=$BOARD_ID_PRODUCT
        fi
    else
        modprobe g_file_storage
    fi
}
wifi_driver() {
    if [ -n "$wifim_START_CMD_wifi" ]; then
        eval "$wifim_START_CMD_wifi"
    else
        modprobe ath6kl_sdio
    fi
}

# The USB port becomes a network link instead of a drive, and the Wi-Fi chip
# is ours. The drivers load while the splash waits, which saves a second or two.
(
    [ -n "$USB_DRIVE" ] && rmmod "$USB_DRIVE"
    modprobe g_ether
    wifi_driver
) &
drivers=$!

chroot $R /usr/local/bin/splash -w 1 "waking up" "press power now for the stock Kindle UI"
pressed=$?
wait $drivers
if [ $pressed -eq 1 ]; then
    echo "power pressed, booting stock this time"
    rmmod g_ether
    usb_drive
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
# SSH over Wi-Fi, which the SSH app turns on and off (nothing listens here otherwise).
iptables -I INPUT -p tcp --dport 2022 -j ACCEPT

# The touch panel's driver is loaded by an upstart job that runs alongside
# this one. It is up long before this on the PW3, but give it a few seconds.
for i in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15; do
    grep -q '^B: ABS=' /proc/bus/input/devices && break
    sleep 1
done
grep -q '^B: ABS=' /proc/bus/input/devices || echo "no touch panel yet"

chroot $R /usr/bin/env -i TERM=linux /sbin/own-init
rc=$?
echo "own-init exited $rc"

# 10 to 12 are choices made on the home screen, so own Linux stays the default.
# Anything else (13 is the crash guard) leaves the next boot stock. own-init
# may already have re-armed the flag, since a home screen that crashes every
# few seconds is still running at its 20 second check, so disarm it again.
case $rc in
10) rearm; touch "$D/STOCK_ONCE" ;;
11 | 12) rearm ;;
*) [ -f "$D/BOOT_OWN" ] && mv -f "$D/BOOT_OWN" "$D/BOOT_OWN.trying" && echo "next boot is stock" ;;
esac
sync
release
sync
if [ $rc -eq 12 ]; then poweroff; else reboot; fi
exit 0
