#!/bin/sh
# Name: Inkwell: install
# Author: Pragyaan Gaur
# DontUseFBInk
# Adds Inkwell's boot hook to the Kindle and reboots into Inkwell. The hook is
# one upstart job, /etc/upstart/ownlinux.conf, which does nothing unless
# /mnt/us/linux/BOOT_OWN exists. "Inkwell: uninstall" removes it again.
D=/mnt/us/linux
say() {
    eips -c >/dev/null 2>&1
    eips 2 30 "$1" >/dev/null 2>&1
    eips 2 32 "$2" >/dev/null 2>&1
}
for f in alpine.img boot.sh ownlinux.conf; do
    if [ ! -f "$D/$f" ]; then
        say "Inkwell: $f is missing." "Copy the linux folder to the Kindle first."
        exit 1
    fi
done
say "Installing Inkwell..." "The Kindle reboots in a moment."
mntroot rw
cp "$D/ownlinux.conf" /etc/upstart/ownlinux.conf
mkdir -p /mnt/own
mntroot ro
if [ ! -f /etc/upstart/ownlinux.conf ]; then
    say "Inkwell: could not write /etc/upstart." "Is the Kindle jailbroken?"
    exit 1
fi
touch "$D/BOOT_OWN"
sync
sleep 2
reboot
