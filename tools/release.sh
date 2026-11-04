#!/bin/sh
# Builds the release: dist/Inkwell-<version>.zip, which unzips onto the
# Kindle's drive as linux/ and documents/. The optional smart and coder
# models are not in the release, because getmodel fetches them from Hugging
# Face on the Kindle.
# The image carries no SSH keys and defaults to UTC.
# Usage: tools/release.sh 1.0.0
set -e
cd "$(dirname "$0")/.."
V=${1:?usage: tools/release.sh version}
NO_KEYS=1 TZ_NAME=UTC ./build-rootfs.sh
R=out/release/Inkwell
rm -rf out/release dist
mkdir -p $R/linux/models $R/documents dist
cp out/alpine.img src/linux/boot.sh src/linux/ownlinux.conf kindle/linux/wifi.conf.example $R/linux/
echo UTC > $R/linux/timezone
cp downloads/models/SmolLM2-135M-Instruct-Q4_0.gguf $R/linux/models/smollm2-135m-q4_0.gguf
cp kindle/documents/Inkwell\ -\ *.sh $R/documents/
(cd $R && zip -q -r -9 "../../../dist/Inkwell-$V.zip" linux documents)
(cd dist && shasum -a 256 * > SHA256SUMS)
ls -l dist
