#!/usr/bin/env bash
# One-time: PS5_Mesa (RADV with the PS5 winsys) and SharpProspero next to PS5_Vulkan,
# then the release RADV archive the title links.
set -euo pipefail
work=${WORK:-$HOME/work}
cd "$work"

[ -d PS5_Mesa ] || git clone https://github.com/mihawk-99/PS5_Mesa.git
[ -d SharpProspero ] || git clone --depth 1 https://github.com/SvenGDK/SharpProspero.git

apt-get install -y -qq meson ninja-build python3-mako bison flex pkg-config >/dev/null 2>&1 || true

cd PS5_Vulkan
tools/build-radv.sh release
ls -la .deps/native/radv-release/lib
