#!/bin/sh
# Installs the MK11 face fix into a CrossOver bottle.
#
# Usage: ./install.sh <bottle name | path to bottle>
#
# Quit Mortal Kombat 11 and CrossOver before running.
set -eu

die() { echo "error: $*" >&2; exit 1; }

[ $# -eq 1 ] || die "usage: $0 <bottle name | path to bottle>"

if [ -d "$1" ]; then
    BOTTLE=$1
else
    BOTTLE="$HOME/Library/Application Support/CrossOver/Bottles/$1"
fi
[ -f "$BOTTLE/user.reg" ] || die "no CrossOver bottle at '$BOTTLE'"

HERE=$(cd "$(dirname "$0")" && pwd)
DLL=""
for f in "$HERE/xinput9_1_0.dll" "$HERE/../build/xinput9_1_0.dll"; do
    [ -f "$f" ] && DLL=$f && break
done
[ -n "$DLL" ] || die "xinput9_1_0.dll not found next to this script or in build/ (run 'make' first)"

EXE=$(find "$BOTTLE/drive_c" -type f -name MK11.exe -path '*Binaries/Retail/*' 2>/dev/null | head -n 1)
[ -n "$EXE" ] || die "MK11.exe not found in '$BOTTLE'"
RETAIL=$(dirname "$EXE")

if pgrep -f 'MK11(_DX12)?\.exe' >/dev/null 2>&1; then
    die "Mortal Kombat 11 is running, quit it first"
fi

if [ -f "$RETAIL/xinput9_1_0.dll" ] && ! grep -q 'mk11 rgb32 fix' "$RETAIL/xinput9_1_0.dll"; then
    die "$RETAIL/xinput9_1_0.dll exists and is not this fix; refusing to overwrite it"
fi

cp "$DLL" "$RETAIL/xinput9_1_0.dll"
echo "installed $RETAIL/xinput9_1_0.dll"

# Make Wine load our xinput9_1_0.dll instead of its builtin one, for MK11.exe only.
if grep -q 'AppDefaults\\\\MK11.exe\\\\DllOverrides' "$BOTTLE/user.reg" \
        && grep -q '"xinput9_1_0"="native,builtin"' "$BOTTLE/user.reg"; then
    echo "DLL override already present"
else
    cp "$BOTTLE/user.reg" "$BOTTLE/user.reg.bak-mk11-face-fix"
    printf '\n[Software\\\\Wine\\\\AppDefaults\\\\MK11.exe\\\\DllOverrides] 1791990000\n"xinput9_1_0"="native,builtin"\n' >> "$BOTTLE/user.reg"
    echo "added DLL override (backup: user.reg.bak-mk11-face-fix)"
fi

echo
echo "Done. In CrossOver, set the bottle's graphics backend to DXMT"
echo "and launch MK11.exe (the DX11 build, not MK11_DX12.exe)."
echo "Log: $RETAIL/mk11_rgb32fix.log"
