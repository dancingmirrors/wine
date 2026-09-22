#!/usr/bin/env bash

set -eu

WINE_TREE="${WINE_TREE:-$PWD}"
BRANCH="${BRANCH:-proton_10.0}"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

[ -f "$WINE_TREE/configure.ac" ] || { echo "not a wine tree: $WINE_TREE" >&2; exit 1; }

echo ":: fetching lsteamclient from Proton $BRANCH (sparse, no submodules)"
git clone --depth 1 --branch "$BRANCH" --filter=blob:none --sparse \
    https://github.com/ValveSoftware/Proton "$TMP/proton"
git -C "$TMP/proton" sparse-checkout set lsteamclient

DST="$WINE_TREE/dlls/lsteamclient"
[ -e "$DST" ] && { echo "$DST already exists; remove it first" >&2; exit 1; }
cp -RpP "$TMP/proton/lsteamclient" "$DST"
echo ":: copied $(find "$DST" -type f | wc -l) files, $(du -sh "$DST" | cut -f1)"

if ! grep -q '^UNIX_CFLAGS' "$DST/Makefile.in"; then
    awk 'BEGIN { done = 0 }
         { print }
         !done && /^IMPORTS[ \t]/ { print "UNIX_CFLAGS = -std=c++17 -include wchar.h -DNOMINMAX"; done = 1 }' \
        "$DST/Makefile.in" > "$DST/Makefile.in.new" \
        && mv "$DST/Makefile.in.new" "$DST/Makefile.in"
    echo ":: added UNIX_CFLAGS = -std=c++17 -include wchar.h -DNOMINMAX to Makefile.in"
fi

grep -q 'dlls/lsteamclient' "$WINE_TREE/configure.ac" \
    && echo ":: configure.ac entry present" \
    || echo ":: WARNING: no WINE_CONFIG_MAKEFILE(dlls/lsteamclient) in configure.ac"

cat <<EOF
Note: dlls/lsteamclient/LICENSE is Valve's Steamworks SDK license.
EOF
