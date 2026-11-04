#!/bin/zsh
# Copies the prebuilt iOS pieces Henge links into ios/deps (not in git):
#   StikJIT.xcframework   Built-in StikJIT (MPL-2.0), as Husk vendors it
#   libhusk_rppairing.a   on-device pairing (Husk's src/rppairing-ios, scripts/build_rppairing_ios.sh)
#   libslirp, glib, intl, pcre2 (arm64 iOS static libraries, Husk's scripts/build_ios.sh)
set -e
HUSK=${HUSK:-$HOME/Documents/Coding/Husk}
DEPS="$(cd "$(dirname "$0")/.." && pwd)/deps"
mkdir -p "$DEPS/lib"
rm -rf "$DEPS/StikJIT.xcframework"
ditto "$HUSK/src/app/Frameworks/StikJIT.xcframework" "$DEPS/StikJIT.xcframework"
cp "$HUSK/build/ios-arm64/lib/libhusk_rppairing.a" "$DEPS/lib/"
for l in libslirp libglib-2.0 libintl libpcre2-8; do cp "$HUSK/build/ios-arm64/sysroot/lib/$l.a" "$DEPS/lib/"; done
echo "deps in $DEPS"
