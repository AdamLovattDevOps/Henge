#!/bin/bash
# Source-only verification: no signing assets, external dependencies or simulator needed.
set -euo pipefail
cd "$(dirname "$0")/../.."
OUT="build/display-check"
mkdir -p "$OUT"
IOS_SDK=$(xcrun --sdk iphoneos --show-sdk-path)
SOURCES=()
while IFS= read -r source; do SOURCES+=("$source"); done < <(find ios/Henge -name '*.swift' -print | sort)
SOURCES+=(ios/HengeJITHelper/HengeJITMessages.swift)

# Compile the complete Swift app against the real SDK, keeping the iOS 17 deployment target.
# Linking/package export still require the dependencies described in ios/README.md.
xcrun swiftc -emit-object -whole-module-optimization -swift-version 5 \
  -target arm64-apple-ios17.0 -sdk "$IOS_SDK" \
  -import-objc-header ios/Henge/HengeBridge.h -Ihost -Icore -Idevices \
  "${SOURCES[@]}" -o "$OUT/Henge.o"

# Compile the renderer for both consumers. Suppress only warnings already present upstream.
COMMON=(-std=c11 -Wall -Wextra -Werror -Wno-unused-parameter -Wno-deprecated-declarations -Wno-shadow-ivar -fobjc-arc -Icore -Ihost -Idevices)
xcrun --sdk iphoneos clang -arch arm64 -miphoneos-version-min=17.0 -isysroot "$IOS_SDK" \
  "${COMMON[@]}" -c devices/pvg_metal.m -o "$OUT/pvg-ios.o"
xcrun --sdk macosx clang -arch arm64 "${COMMON[@]}" \
  -c devices/pvg_metal.m -o "$OUT/pvg-mac.o"
make test-display-routing

# The C handoff must be compiled too: display attachment can race lazy GPU creation.
for source in devices/pvg_lite.c host/vm.c; do
  name=$(basename "$source" .c)
  xcrun --sdk iphoneos clang -arch arm64 -miphoneos-version-min=17.0 -isysroot "$IOS_SDK" \
    -std=c11 -Wall -Wextra -Werror -Icore -Ihost -Idevices \
    -c "$source" -o "$OUT/$name-ios.o"
  xcrun --sdk macosx clang -arch arm64 -std=c11 -Wall -Wextra -Werror \
    -Icore -Ihost -Idevices -c "$source" -o "$OUT/$name-mac.o"
done
