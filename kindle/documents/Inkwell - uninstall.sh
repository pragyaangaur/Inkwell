#!/bin/sh
# Name: Inkwell: uninstall
# Author: Pragyaan Gaur
# DontUseFBInk
# Removes Inkwell's boot hook. The linux folder stays on the drive, and
# deleting it over USB frees the space.
rm -f /mnt/us/linux/BOOT_OWN /mnt/us/linux/BOOT_OWN.trying /mnt/us/linux/STOCK_ONCE
mntroot rw
rm -f /etc/upstart/ownlinux.conf
rmdir /mnt/own 2>/dev/null
mntroot ro
sync
eips -c >/dev/null 2>&1
eips 2 30 "Inkwell is uninstalled." >/dev/null 2>&1
