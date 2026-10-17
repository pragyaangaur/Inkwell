#!/bin/sh
# Cross-compiles the Kindle PW3 programs (ARMv7, static, no libc) into out/.
set -e
cd "$(dirname "$0")"
MC=src/rd # rdgeneric, from github.com/pragyaangaur/Can-It-Run-Minecraft
F="--target=armv7a-linux-gnueabi -mfloat-abi=softfp -mfpu=vfpv3-d16 -marm -O2 -ffreestanding -fno-builtin -nostdlib -fno-stack-protector -ffp-contract=off -fno-pic -Wall -Isrc"
mkdir -p out/obj
clang $F -I"$MC" -c src/minecraft.c -o out/obj/minecraft.o
clang $F -I"$MC" -c "$MC/rd.c" -o out/obj/rd.o
ld.lld -static -e _start -o out/rd-kindle out/obj/minecraft.o out/obj/rd.o
if [ -f src/triking.c ]; then
  clang $F -c src/triking.c -o out/obj/triking.o
  ld.lld -static -e _start -o out/triking out/obj/triking.o
fi
ls -l out/rd-kindle out/triking 2>/dev/null
