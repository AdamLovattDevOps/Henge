#!/bin/zsh
# Makes a folder Henge on iOS can bring in: drop it on Henge in Finder (the
# iPhone's Files section) or pick it in the Files app.
#   tools/export_ios.sh DISK AUX ECID [NAME] [RAM_MB] [OUTDIR]
# The disk goes in packed (disk.img.hvsparse: holes left out) so that 64 GB of
# mostly empty image moves as what it actually holds. Reads the disk; writes nothing to it.
set -e
DISK=$1; AUX=$2; ECID=$3; NAME=${4:-Mac}; RAM=${5:-4096}; OUT=${6:-$HOME/Desktop/$NAME}
[ -f "$DISK" ] && [ -f "$AUX" ] && [ -n "$ECID" ] || { echo "usage: $0 DISK AUX ECID [NAME] [RAM_MB] [OUTDIR]" >&2; exit 2; }
HERE="$(cd "$(dirname "$0")/.." && pwd)"
make -C "$HERE" build/hvsparse >/dev/null
mkdir -p "$OUT"
cp /System/Library/Frameworks/Virtualization.framework/Resources/AVPBooter.vmapple2.bin "$OUT/"
cp "$AUX" "$OUT/auxiliary.bin"
echo "packing the disk (only its data moves)..."
"$HERE/build/hvsparse" pack "$DISK" "$OUT/disk.img.hvsparse"
cat > "$OUT/machine.json" <<JSON
{"name": "$NAME", "ecid": $ECID, "seed": 12345, "ramMB": $RAM}
JSON
echo "$OUT: $(du -sh "$OUT" | cut -f1)"
