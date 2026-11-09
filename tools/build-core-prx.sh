#!/usr/bin/env bash
# core -> osu5core.prx via SharpProspero
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
export SHARPPROSPERO_ROOT=${SHARPPROSPERO:-$HOME/work/SharpProspero}
export DOTNET_CLI_TELEMETRY_OPTOUT=1
# its packager insists on seeing the libc.prx the module will run against
export PROSPERO_MODULES=${PROSPERO_MODULES:-${PS5_VULKAN:-$HOME/work/PS5_Vulkan}/runtime}

mkdir -p "$root/core/prx/obj"
clang -c -O2 -fPIC -fno-stack-protector -ffreestanding "$root/core/prx/shim.c" -o "$root/core/prx/obj/shim.o"

# its own projects first, the app build expects them built
dotnet build "$SHARPPROSPERO_ROOT/SharpProspero.slnx" -c Release --nologo -v q
# Folder: a library has no eboot for the package step to find
pwsh "$SHARPPROSPERO_ROOT/build/build-app.ps1" -ProjectPath "$root/core/prx/Osu5Core.Prx.csproj" \
    -Output Folder -OutputFolder "$root/core/prx/out"
find "$root/core/prx" -name '*.prx' -newer "$root/core/prx/Osu5Core.Prx.csproj" | head
