#!/usr/bin/env bash
# Build osu5 into dist/PPSA70727. Run inside WSL/Linux after tools/fetch-deps.sh.
#
# Two modules:
#   eboot.bin              host (C), PS5_Vulkan toolchain, RADV linked in
#   sce_module/osu5core.prx  gameplay (C#), SharpProspero NativeAOT
# The link below is tools/build-radv-title.sh's from PS5_Vulkan with our objects.
set -euo pipefail

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
pv=${PS5_VULKAN:-$HOME/work/PS5_Vulkan}
sdk_root="$pv/.deps/native/ps5-payload-sdk"
native="$pv/tooling/native"
tool="$pv/build/host/ps5-native-tool"
archive=${RADV_ARCHIVE:-$pv/.deps/native/radv-release/lib/libvulkan_radeon.ps5.a}
radv_include="$pv/.deps/native/radv-release/include"
work="$root/build"
app="$root/dist/PPSA70727"

for f in "$archive" "$tool" "$sdk_root/bin/prospero-lld"; do
    [[ -e $f ]] || { echo "missing $f (run tools/fetch-deps.sh)" >&2; exit 2; }
done

mkdir -p "$work/obj" "$work/gen" "$work/stubs"
cc() { PS5_PAYLOAD_SDK="$sdk_root" sh "$pv/tooling/prospero-clang18" "$@"; }

# core: SharpProspero's pipeline compiles it; we only take its NativeAOT objects
[[ ${SKIP_CORE:-0} == 1 ]] || bash "$root/tools/build-core-prx.sh"
[[ -f $root/core/prx/obj/Release/net10.0/linux-x64/native/osu5core.o ]] || { echo "core objects missing" >&2; exit 2; }
sp=${SHARP_PROSPERO:-$HOME/work/SharpProspero}
spgen() {
    DOTNET_ROOT=${DOTNET_ROOT:-/opt/dotnet} PATH=/opt/dotnet:$PATH \
        dotnet run --no-build --project "$sp/tools/SharpProspero.Bindings.Generator" -- "$@"
}

# shaders
for s in vert frag; do
    glslangValidator -V --target-env vulkan1.3 --vn "shape_$s" "$root/host/shaders/shape.$s" -o "$work/gen/shape_$s.h" >/dev/null
done

# host; obj/ is globbed into the link, so nothing stale may stay there
rm -f "$work"/obj/*.o
for f in "$root"/host/src/*.c; do
    cc -std=gnu11 -O2 -Wall -Wextra -Wno-unused-parameter -ffunction-sections -fdata-sections \
        -I "$work/gen" -I "$radv_include" -I "$pv/.deps/native/zlib/root/usr/include" \
        -c "$f" -o "$work/obj/$(basename "${f%.c}").o"
done
for f in app_crt app_cpp_runtime; do
    cc -std=c++20 -O2 -fno-exceptions -fno-rtti -ffunction-sections -fdata-sections \
        -c "$native/$f.cpp" -o "$work/obj/$f.o"
done

stub() {
    # not in obj/, everything there goes into the eboot
    cc -std=c11 -O2 -fPIC -c "$pv/$2" -o "$work/stubs/${1}_stub.o"
    "$sdk_root/bin/prospero-lld" --shared -soname "$1.prx" -o "$work/stubs/$1.so" "$work/stubs/${1}_stub.o"
}
stub libSceAgc vendor/ps5/sdk/stubs/agc_canary_link_stub.c
stub libSceAgcDriver vendor/ps5/sdk/stubs/agc_driver_canary_link_stub.c
# shellcheck source=/dev/null
source "$pv/tools/radv-link.sh"
radv_link_recipe "$pv" "$sdk_root" "$archive" || exit 2

# The C# core goes in as plain objects: NativeAOT output, the runtime archives it
# needs, and SharpProspero's compat object for the Linux names the runtime calls.
# A title can't load a module of its own at run time here, so linking is the way.
aot="$root/core/prx/obj/Release/net10.0/linux-x64"
spgen compat --out "$work/compat.raw.o" >/dev/null

# compat.o defines ~130 libc names with Linux behaviour (readdir hands back Linux
# dirents, mmap wants Linux flags...). Only the .NET side may see those, so every
# name it defines becomes __spc_<name>, in compat and in the .NET objects alike;
# the host and RADV keep the real ones. compat's own way out to the real call,
# __sp_device_<name>, becomes plain <name>.
objcopy=llvm-objcopy-18
rm -rf "$work/core" && mkdir -p "$work/core"
nm -g --defined-only "$work/compat.raw.o" | awk '$3 !~ /^_Z/ {print $3, "__spc_" $3}' > "$work/core/spc.map"
nm -u "$work/compat.raw.o" | awk '$2 ~ /^__sp_device_/ {n = $2; sub(/^__sp_device_/, "", n); print $2, n}' > "$work/core/device.map"
cat "$work/core/spc.map" "$work/core/device.map" > "$work/core/compat.map"
$objcopy --redefine-syms="$work/core/compat.map" "$work/compat.raw.o" "$work/core/compat.o"

# The runtime walks managed stacks with its own copy of libunwind's C++ templates,
# and the SDK's libunwind.a (in for RADV's C++) has the same ones, built another
# way. They're weak, so the linker keeps one, the SDK's, and the runtime then reads
# an UnwindInfoSections laid out differently: the first GC faulted. Renaming the
# runtime's libunwind:: names keeps the two apart; the C API (_Unwind_*, __unw_*)
# stays shared, C++ exceptions have to go through one unwinder.
nm --defined-only "$aot"/runtime-support/libRuntime*.a 2>/dev/null |
    awk '$3 ~ /^_ZN9libunwind/ {print $3, "__dotnet" $3}' | sort -u > "$work/core/unw.map"
cat "$work/core/spc.map" "$work/core/unw.map" > "$work/core/core.map"

# .NET's own zlib is left out, RADV already carries one
core_inputs=()
for f in "$aot/native/osu5core.o" "$aot/runtime-support/libbootstrapperdll.o" "$aot"/runtime-support/*.a; do
    [[ $(basename "$f") == libz.a ]] && continue
    $objcopy --redefine-syms="$work/core/core.map" "$f" "$work/core/$(basename "$f")"
    core_inputs+=("$work/core/$(basename "$f")")
done
core_inputs=("${core_inputs[@]:0:2}" "$work/core/compat.o" --start-group "${core_inputs[@]:2}" --end-group)

link() {
    # nostart-stop-gc: the runtime finds its module tables through __start_/__stop_
    # symbols only, and --gc-sections would otherwise throw those sections away.
    # Both sides bring libunwind and operator new/delete; first definition wins, so
    # the order below matters: host + PS5_Vulkan runtime, then RADV (its libunwind
    # knows the PS5 unwind tables), then the core, where compat.o has to come
    # before the runtime archives it overrides.
    "$sdk_root/bin/prospero-lld" -T "$root/host/osu5.ld" \
        --eh-frame-hdr "${radv_link_flags[@]}" \
        --version-script "$native/app-symbols.map" --exclude-libs=ALL -z nostart-stop-gc \
        --allow-multiple-definition -e _start -o "$work/osu5.elf" "$@" \
        "$work"/obj/*.o "$work/stubs/libSceAgc.so" "$work/stubs/libSceAgcDriver.so" \
        "${radv_link_inputs[@]}" "${core_inputs[@]}" \
        --as-needed "$sdk_root"/target/lib/*.so
}

# Mesa's dispatch tables weakly name entry points RADV never defines (vk_common_*
# covers those). Left undefined they land in .dynsym and the module writer takes
# them for imports no SDK stub has. A missing weak symbol is 0 at run time anyway,
# so pin each one to 0 at link time and relink.
link
nm -u "$work/osu5.elf" | awk '$1 == "w" { print "--defsym=" $2 "=0" }' > "$work/weak.rsp"
echo "pinning $(wc -l < "$work/weak.rsp") undefined weak symbols"
# The runtime's assembly helpers also call each other by an underscored name
# (_RhpNewFast for RhpNewFast); SharpProspero's linker folds those together, so do
# the same for every _X left undefined while X is defined.
nm --defined-only "$work/osu5.elf" | awk '{print $3}' | sort -u > "$work/defined.txt"
nm -u "$work/osu5.elf" | awk '$1 == "U" && $2 ~ /^_[A-Za-z]/ {print substr($2, 2), $2}' | sort -u |
    join - "$work/defined.txt" | awk '{print "--defsym=" $2 "=" $1}' > "$work/alias.rsp"
echo "aliasing $(wc -l < "$work/alias.rsp") underscored runtime names"
link "@$work/weak.rsp" "@$work/alias.rsp"

"$tool" link --in "$work/osu5.elf" --out "$work/eboot.elf" \
    --stub-dir "$sdk_root/target/lib" --stub "$work/stubs/libSceAgc.so" \
    --stub "$work/stubs/libSceAgcDriver.so" --module-sdk 0x02000009 \
    --companion-sdk 0x08050001 --file-name eboot.elf

rm -rf -- "$app"
mkdir -p "$app/sce_sys" "$app/sce_module"
"$tool" self --sign --in "$work/eboot.elf" --out "$app/eboot.bin" --magic 0x1D3D154F
cp "$root/sce_sys/param.json" "$root/sce_sys/icon0.png" "$app/sce_sys/"
mkdir -p "$app/assets"
cp "$root/host/fonts/Inter-SemiBold.ttf" "$root/host/fonts/Inter-LICENSE.txt" "$app/assets/"
cp "$pv/runtime/libc.prx" "$app/sce_module/libc.prx"
printf 'built %s (eboot %s bytes)\n' "$app" "$(stat -c %s "$app/eboot.bin")"
