#!/usr/bin/env bash
# host-side syntax check, no PS5 toolchain needed
cd "$(dirname "$0")/../host"
gen=/tmp/osu5-gen
mkdir -p $gen
for s in vert frag; do
    glslangValidator -V --target-env vulkan1.3 --vn shape_$s shaders/shape.$s -o $gen/shape_$s.h || echo "shader $s failed"
done
vk=$HOME/work/PS5_PayloadSDK/include/khronos
for f in src/*.c; do
    clang-18 -std=gnu11 -fsyntax-only -Wall -Wextra -Wno-unused-parameter -I$gen -I$vk "$f" 2>&1 | grep -E "error|warning:" | grep -v third_party | head -8
done
echo checked
