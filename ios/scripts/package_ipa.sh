#!/bin/zsh
# Builds Henge for iOS and writes ~/Desktop/Henge.ipa (or ~/Desktop/Henge.tipa with --trollstore).
# Unsigned build, then signed ad hoc inside out with Henge.entitlements, as Husk's
# package_ipa.sh does: the sideloader (SideStore, AltStore, Impactor) re-signs it
# with the user's own certificate and keeps the entitlements it can.
# --trollstore strips the entitlements TrollStore bans under PPL/TXM.
set -e
IOS="$(cd "$(dirname "$0")/.." && pwd)"
TROLLSTORE=false
[ "$1" = "--trollstore" ] && TROLLSTORE=true
OUT="$HOME/Desktop/Henge.ipa"
[ "$TROLLSTORE" = true ] && OUT="$HOME/Desktop/Henge.tipa"
[ -d "$IOS/deps/StikJIT.xcframework" ] || "$IOS/scripts/fetch_deps.sh"
cd "$IOS"
xcodegen generate >/dev/null
DD="$IOS/build/DerivedData"
xcodebuild -project Henge.xcodeproj -scheme Henge -configuration Release -sdk iphoneos -destination 'generic/platform=iOS' \
    -derivedDataPath "$DD" CODE_SIGNING_ALLOWED=NO CODE_SIGNING_REQUIRED=NO CODE_SIGN_IDENTITY="" build | grep -E "error:|BUILD" || true
APP="$DD/Build/Products/Release-iphoneos/Henge.app"
rc=0
for f in "$APP/Henge" "$APP/henge-jit.js" "$APP/PlugIns/HengeJITHelper.appex/HengeJITHelper" \
         "$APP/Frameworks/StikJIT.framework/StikJIT" "$APP/Frameworks/StikJIT.framework/Info.plist"; do
    if [ -f "$f" ]; then printf "  ok       %s\n" "${f#$APP/}"; else echo "  MISSING  ${f#$APP/}" >&2; rc=1; fi
done
[ $rc -eq 0 ] || { echo "==> bundle is not installable; refusing to package" >&2; exit 1; }

STAGE="$(mktemp -d "$DD/stage.XXXXXX")"
trap 'rm -rf "$STAGE"' EXIT
mkdir -p "$STAGE/Payload"
cp -R "$APP" "$STAGE/Payload/"
ENT="$STAGE/entitlements.plist"
cp "$IOS/Henge/Henge.entitlements" "$ENT"
BANNED_KEYS=("com.apple.private.cs.debugger" "dynamic-codesigning" "com.apple.private.skip-library-validation")
if [ "$TROLLSTORE" = true ]; then for k in "${BANNED_KEYS[@]}"; do plutil -remove "$k" "$ENT" 2>/dev/null || true; done; fi
SAPP="$STAGE/Payload/Henge.app"
sign() { codesign --force --sign - --timestamp=none "$@"; }
for fw in "$SAPP"/Frameworks/*.framework; do [ -d "$fw" ] && sign "$fw"; done
for ex in "$SAPP"/PlugIns/*.appex; do [ -d "$ex" ] && sign "$ex"; done
sign --entitlements "$ENT" "$SAPP"
EMBEDDED=$(codesign -d --entitlements - "$SAPP" 2>&1 || true)
for k in get-task-allow increased-memory-limit extended-virtual-addressing; do
    grep -q "$k" <<< "$EMBEDDED" || { echo "==> $k did not embed; refusing to package" >&2; exit 1; }
done
( cd "$STAGE" && zip -qry "$STAGE/package.tmp" Payload )
mv -f "$STAGE/package.tmp" "$OUT" # atomic: a half-written IPA never sits where the good one was
echo "==> $OUT ($(du -h "$OUT" | cut -f1))"
