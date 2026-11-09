#!/usr/bin/env bash
# dist/PPSA70727 + the generated test map -> dist/osu-ps5-<version>.zip
#   package.sh v0.1.0
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
ver=${1:?version, e.g. v0.1.0}
stage="$root/build/package/osu-ps5-$ver"

rm -rf "$root/build/package"
mkdir -p "$stage/homebrew" "$stage/osu5/Songs"
cp -r "$root/dist/PPSA70727" "$stage/homebrew/"
python3 "$root/tools/make-test-map.py" "$stage/osu5/Songs/osu5 test" >/dev/null
cat > "$stage/INSTALL.txt" <<'EOF'
Copy homebrew/PPSA70727 to /data/homebrew/ on the console and osu5/ to /data/
(so maps end up in /data/osu5/Songs/). ShadowMountPlus picks the title up; launch
"osu!" from the home screen.

Put your own maps in /data/osu5/Songs/<folder>/ (extract the .osz, it's a zip).

First launch asks for a skin and downloads it (the console needs internet for
that). Your own skins go in /data/osu5/skins/<name>/ (unzip the .osk).
EOF

cd "$root/build/package"
python3 - "osu-ps5-$ver" "$root/dist/osu-ps5-$ver.zip" <<'EOF'
import os, sys, zipfile
src, out = sys.argv[1], sys.argv[2]
with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
    for d, _, files in os.walk(src):
        for f in files:
            z.write(os.path.join(d, f))
EOF
ls -l "$root/dist/osu-ps5-$ver.zip"
