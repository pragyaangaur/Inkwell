#!/bin/sh
# Downloads what the build needs into downloads/: the Alpine armv7
# minirootfs and package index, a musl and libc++ sysroot made from Alpine's
# packages, llama.cpp v0.7.0, the shareware Doom WAD and Quill's models.
# Safe to run again: anything already there is kept.
set -e
cd "$(dirname "$0")/.."
D=downloads
ALPINE=https://dl-cdn.alpinelinux.org/alpine/v3.24
mkdir -p $D/alpine $D/alpine-dev $D/sysroot $D/llama $D/doom

get() { # url file [sha256]
    if [ ! -s "$2" ]; then
        echo "fetching $1"
        curl -fL --retry 3 -o "$2.part" "$1"
        mv "$2.part" "$2"
    fi
    if [ -n "$3" ]; then echo "$3  $2" | shasum -a 256 -c >/dev/null; fi
}

get $ALPINE/releases/armv7/alpine-minirootfs-3.24.2-armv7.tar.gz $D/alpine/alpine-minirootfs-3.24.2-armv7.tar.gz \
    5552f1ef2398cee46ef2cf733f8f1258f39a77ad065dfd47282fc0df9fe1976c
for repo in main community; do
    if [ ! -s $D/APKINDEX.$repo ]; then
        get $ALPINE/$repo/armv7/APKINDEX.tar.gz $D/APKINDEX.tar.gz
        tar -xzOf $D/APKINDEX.tar.gz APKINDEX > $D/APKINDEX.$repo
        rm $D/APKINDEX.tar.gz
    fi
done

# The sysroot tools/kcc and tools/kcross compile against: musl, the kernel
# headers, compiler-rt, libc++ and libunwind, all from Alpine's armv7 builds.
if [ ! -f $D/sysroot/usr/lib/libc.a ]; then
    for rp in $(python3 tools/apkdeps.py --exact $D/APKINDEX.main musl musl-dev linux-headers compiler-rt \
        libc++ libc++-dev libc++-static llvm-libunwind-dev llvm-libunwind-static); do
        p=${rp#*/}
        get "$ALPINE/main/armv7/$p.apk" "$D/alpine-dev/$p.apk"
        tar -xzf "$D/alpine-dev/$p.apk" -C $D/sysroot --exclude '.SIGN*' --exclude '.PKGINFO' --exclude '.pre-*' --exclude '.post-*' --exclude '.trigger' 2>/dev/null
    done
fi

get https://github.com/ggml-org/llama.cpp/archive/refs/tags/v0.7.0.tar.gz $D/llama/src.tar.gz \
    55d67acf3ad2ad747a9bbeb75f9f0e173dbcc0d1753a471038a90c819421c5e1
[ -d $D/llama/llama.cpp-0.7.0 ] || tar -xzf $D/llama/src.tar.gz -C $D/llama

# id Software's shareware episode, which may be shared unchanged. Debian
# packages the original file.
if [ ! -s $D/doom/doom1.wad ]; then
    get http://deb.debian.org/debian/pool/non-free/d/doom-wad-shareware/doom-wad-shareware_1.9.fixed-5_all.deb $D/doom/wad.deb
    (cd $D/doom && ar x wad.deb data.tar.xz && tar -xJOf data.tar.xz ./usr/share/games/doom/doom1.wad > doom1.wad && rm wad.deb data.tar.xz)
fi
echo "1d7d43be501e67d927e415e0b8f3e29c3bf33075e859721816f652a526cac771  $D/doom/doom1.wad" | shasum -a 256 -c >/dev/null

tools/get-models.sh
echo "downloads are ready"
