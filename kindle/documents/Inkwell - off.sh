#!/bin/sh
# Name: Inkwell: off (boot the Kindle UI)
# Author: Pietro Boccaletto
# DontUseFBInk
# The Kindle boots its own UI from now on. "Inkwell: start" turns it back on.
rm -f /mnt/us/linux/BOOT_OWN /mnt/us/linux/BOOT_OWN.trying
sync
