#!/usr/bin/env bash
# Upload dist/PPSA70727 (and optionally a songs folder) to the console over FTP.
#   PS5_IP=<console address> [PS5_FTP_PORT=2121] deploy.sh [songs-dir]
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
ip=${PS5_IP:?set PS5_IP to the console address}
port=${PS5_FTP_PORT:-2121}
ftp="ftp://$ip:$port"

put_tree() {
    local src=$1 dst=$2
    (cd "$src" && find . -type f) | while read -r f; do
        f=${f#./}
        curl -sS --ftp-create-dirs -T "$src/$f" "$ftp/$dst/$(printf '%s' "$f" | sed 's/ /%20/g')"
    done
}

put_tree "$root/dist/PPSA70727" "data/homebrew/PPSA70727"
echo "title uploaded"

if [[ $# -gt 0 ]]; then
    name=$(basename "$1")
    put_tree "$1" "data/osu5/Songs/$(printf '%s' "$name" | sed 's/ /%20/g')"
    echo "songs uploaded"
fi
