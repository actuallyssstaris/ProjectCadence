#!/usr/bin/env bash
set -e
export PS5_PAYLOAD_SDK=${PS5_PAYLOAD_SDK:-$HOME/work/PS5_Vulkan/.deps/native/ps5-payload-sdk}
cd "$(dirname "$0")"
"$PS5_PAYLOAD_SDK/bin/prospero-clang" -Wall -O2 -o klogdump.elf klogdump.c -lkernel_sys
ls -l klogdump.elf
