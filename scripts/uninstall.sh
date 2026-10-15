#!/bin/sh
# Removes the MK11 face fix from a CrossOver bottle.
#
# Usage: ./uninstall.sh <bottle name | path to bottle>
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

EXE=$(find "$BOTTLE/drive_c" -type f -name MK11.exe -path '*Binaries/Retail/*' 2>/dev/null | head -n 1)
if [ -n "$EXE" ]; then
    RETAIL=$(dirname "$EXE")
    if [ -f "$RETAIL/xinput9_1_0.dll" ] && grep -q 'mk11 rgb32 fix' "$RETAIL/xinput9_1_0.dll"; then
        rm -f "$RETAIL/xinput9_1_0.dll" "$RETAIL/mk11_rgb32fix.log"
        echo "removed $RETAIL/xinput9_1_0.dll"
    fi
fi

# Drop the "xinput9_1_0" override from the MK11.exe DllOverrides key. With the
# DLL gone Wine falls back to its builtin one anyway, so this is just cleanup.
/usr/bin/perl -0pi -e '
    s{(\[Software\\\\Wine\\\\AppDefaults\\\\MK11\.exe\\\\DllOverrides\][^\n]*\n(?:[^\[\n][^\n]*\n)*?)"xinput9_1_0"="native,builtin"\n}{$1};
    s{\n\[Software\\\\Wine\\\\AppDefaults\\\\MK11\.exe\\\\DllOverrides\][^\n]*\n(?:#time=[^\n]*\n)?(?=\n|\[|\z)}{\n};
' "$BOTTLE/user.reg"
echo "removed DLL override"
