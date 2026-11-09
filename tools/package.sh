#!/usr/bin/env bash
# dist/PPSA70727 + the generated test map -> dist/projectcadence-<version>.zip
#   package.sh v0.5.0
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
ver=${1:?version, e.g. v0.5.0}
stage="$root/build/package/projectcadence-$ver"

rm -rf "$root/build/package"
mkdir -p "$stage/homebrew" "$stage/cadence/Songs"
cp -r "$root/dist/PPSA70727" "$stage/homebrew/"
python3 "$root/tools/make-test-map.py" "$stage/cadence/Songs/cadence test" >/dev/null
cat > "$stage/INSTALL.txt" <<'TXT'
Copy homebrew/PPSA70727 to /data/homebrew/ on the console and cadence/ to /data/
(so maps end up in /data/cadence/Songs/). ShadowMountPlus picks the title up; launch
ProjectCadence from the home screen.

Coming from osu-ps5: your /data/osu5 folder (maps, skins, scores) is moved to
/data/cadence the first time ProjectCadence starts.

Get maps from the in-game downloader (Triangle in song select), or put them in
/data/cadence/Songs/<folder>/ (extract the .osz, it's a zip).

First launch asks for a skin and downloads it (the console needs internet for
that). Your own skins go in /data/cadence/skins/<name>/ (unzip the .osk).
TXT

cd "$root/build/package"
python3 - "projectcadence-$ver" "$root/dist/projectcadence-$ver.zip" <<'PY'
import os, sys, zipfile
src, out = sys.argv[1], sys.argv[2]
with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
    for d, _, files in os.walk(src):
        for f in files:
            z.write(os.path.join(d, f))
PY
ls -l "$root/dist/projectcadence-$ver.zip"
