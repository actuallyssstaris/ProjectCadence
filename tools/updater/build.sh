#!/usr/bin/env bash
# the update helper payload; build.sh puts it in the app's assets
set -e
export PS5_PAYLOAD_SDK=${PS5_PAYLOAD_SDK:-$HOME/work/PS5_Vulkan/.deps/native/ps5-payload-sdk}
cd "$(dirname "$0")"
"$PS5_PAYLOAD_SDK/bin/prospero-clang" -Wall -O2 -o updater.elf updater.c -lkernel_sys
