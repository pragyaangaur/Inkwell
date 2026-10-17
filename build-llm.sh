#!/bin/sh
# Builds llama.cpp's libraries for the Kindle and links our chat runner
# (src/chat/inkllm.cpp) against them into out/inkllm.
set -e
cd "$(dirname "$0")"
mkdir -p out
L=downloads/llama/llama.cpp-0.7.0
B=out/llama-build
# Patch: clang defines __ARM_FP16_FORMAT_IEEE on every 32-bit Arm target, so ggml
# converts fp16 scales with __fp16 casts, which on the Cortex-A9 (no fp16
# hardware) become a call to a software routine for every block. Require real
# half-precision hardware (bit 1 of __ARM_FP) so ggml uses its lookup table.
SM=$L/ggml/src/ggml-cpu/simd-mappings.h
grep -q 'INKWELL' $SM || sed -i.orig 's|^#if defined(__ARM_NEON) \&\& defined(__ARM_FP16_FORMAT_IEEE) \&\& |#if defined(__ARM_NEON) \&\& defined(__ARM_FP16_FORMAT_IEEE) \&\& (defined(__aarch64__) \|\| (__ARM_FP \& 2)) /* INKWELL */ \&\& |' $SM
grep -q 'INKWELL' $SM
if [ ! -f $B/build.ninja ]; then
    cmake -S $L -B $B -G Ninja -DCMAKE_TOOLCHAIN_FILE="$PWD/tools/kindle.cmake" -DCMAKE_BUILD_TYPE=Release \
        -DBUILD_SHARED_LIBS=OFF -DGGML_NATIVE=OFF -DGGML_OPENMP=OFF -DGGML_LLAMAFILE=OFF -DLLAMA_CURL=OFF \
        -DLLAMA_BUILD_TESTS=OFF -DLLAMA_BUILD_EXAMPLES=OFF -DLLAMA_BUILD_SERVER=OFF -DLLAMA_BUILD_TOOLS=OFF -DLLAMA_BUILD_COMMON=OFF
fi
cmake --build $B --target llama
tools/kcross-c++ -c -O2 -std=c++17 -I$L/include -I$L/ggml/include -o out/inkllm.o src/chat/inkllm.cpp
tools/kcross-c++ -s -o out/inkllm out/inkllm.o $B/src/libllama.a $B/ggml/src/libggml.a $B/ggml/src/libggml-cpu.a $B/ggml/src/libggml-base.a
ls -l out/inkllm
