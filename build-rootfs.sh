#!/bin/sh
# Builds out/alpine.img, the ext3 image of our own Linux system for the Kindle:
# the Alpine armv7 minirootfs, the packages in PKGS with their dependencies,
# our overlay in src/rootfs, and the home screen, splash and games. Stages the
# userstore files in kindle/linux/.
set -e
cd "$(dirname "$0")"
A=downloads/alpine
S=out/rootfs-stage
IMG=out/alpine.img
SIZE="${SIZE:-1G}"
# Homebrew keeps e2fsprogs out of PATH. TZ_NAME sets the default time zone,
# which a "timezone" file on the Kindle overrides at boot.
for E2 in /opt/homebrew/opt/e2fsprogs/sbin /usr/local/opt/e2fsprogs/sbin /usr/sbin /sbin; do [ -x "$E2/mke2fs" ] && break; done
TZ_NAME="${TZ_NAME:-UTC}"
MIRROR=https://dl-cdn.alpinelinux.org/alpine/v3.24/main/armv7
PKGS="dropbear dropbear-scp busybox-extras wpa_supplicant tzdata curl"

./build-ui.sh
./build-doom.sh >/dev/null
./build-llm.sh >/dev/null

rm -rf "$S" "$IMG"
mkdir -p "$S"
tar -xpzf "$A"/alpine-minirootfs-3.24.2-armv7.tar.gz -C "$S"

# Unpack the packages directly so everything works on first boot. own-init
# registers them with apk from /root/pkgs the first time it runs.
mkdir -p "$S/root/pkgs"
for p in $(python3 tools/apkdeps.py downloads/APKINDEX.main $PKGS); do
    [ -f "$A/$p.apk" ] || curl -sf -o "$A/$p.apk" "$MIRROR/$p.apk"
    cp "$A/$p.apk" "$S/root/pkgs/"
    tar -xpzf "$A/$p.apk" -C "$S" --exclude '.SIGN*' --exclude '.PKGINFO' --exclude '.pre-*' --exclude '.post-*' --exclude '.trigger'
done

cp -R src/rootfs/ "$S/"
mkdir -p "$S/root/.ssh" "$S/etc/dropbear" "$S/usr/share/fonts/inkwell" "$S/usr/local/games"
# Your own SSH key (made by ./ssh.sh) and a fixed host key, so rebuilding the
# image doesn't trip SSH's host key check. Release images set NO_KEYS=1 and
# ship neither: each Kindle makes its own host key and reads authorized_keys
# from the userstore.
if [ -z "$NO_KEYS" ]; then
    [ -f keys/kindle_ed25519.pub ] && cp keys/kindle_ed25519.pub "$S/root/.ssh/authorized_keys" && chmod 600 "$S/root/.ssh/authorized_keys"
    [ -f keys/dropbear_ed25519_host_key ] && cp keys/dropbear_ed25519_host_key "$S/etc/dropbear/" && chmod 600 "$S/etc/dropbear/dropbear_ed25519_host_key"
fi
chmod 700 "$S/root" "$S/root/.ssh"
chmod 1777 "$S/tmp"
cp fonts/*.ttf "$S/usr/share/fonts/inkwell/"
for p in home splash wifi settings notes sketch browser term chess doom quill inkllm tap; do cp out/$p "$S/usr/local/bin/"; done
cp out/rd-kindle out/triking "$S/usr/local/games/"
mkdir -p "$S/usr/local/share/doom"
cp downloads/doom/doom1.wad "$S/usr/local/share/doom/" # the shareware episode, free to share
cp "$S/usr/share/zoneinfo/$TZ_NAME" "$S/etc/localtime"
echo "$TZ_NAME" > "$S/etc/timezone"
ln -sf /usr/local/bin/kindle "$S/usr/bin/kindle"
ln -sf /usr/local/bin/tap "$S/usr/bin/tap"
ln -sf /usr/bin/dbclient "$S/usr/bin/ssh" # Dropbear's client, under the name people type
ln -sf /bin/busybox-extras "$S/usr/sbin/udhcpd" # applet links come from an apk trigger we skip

"$E2/mke2fs" -q -F -t ext3 -L alpine -E root_owner=0:0 -d "$S" "$IMG" "$SIZE"

# mke2fs copies the Mac's uid and gid, so hand every file to root.
(cd "$S" && find . -mindepth 1 | sed 's|^\.||') | while IFS= read -r f; do
    printf 'sif "%s" uid 0\nsif "%s" gid 0\n' "$f" "$f"
done > out/chown.debugfs
"$E2/debugfs" -w -f out/chown.debugfs "$IMG" >/dev/null 2>&1
"$E2/e2fsck" -fy "$IMG" >/dev/null || true

mkdir -p kindle/linux
cp src/linux/boot.sh src/linux/ownlinux.conf kindle/linux/
ls -l "$IMG"
