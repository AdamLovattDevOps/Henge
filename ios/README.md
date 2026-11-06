# Henge for iOS

The Henge app on HengeVM's engine. Build: `ios/scripts/package_ipa.sh` (writes
`~/Desktop/Henge.ipa`; `--trollstore` writes `Henge.tipa` without the entitlements
TrollStore bans). Needs Xcode, xcodegen, and the prebuilt pieces
`ios/scripts/fetch_deps.sh` copies from Husk (StikJIT, the pairing library,
libslirp and glib for iOS).

- **JIT**: on iOS 26+ (TXM) only an attached debugger can grant executable
  memory. Henge uses the same machinery as Husk: StikDebug, TrollStore, a
  jailbreak, or the built-in StikJIT helper (`HengeJITHelper.appex`, with
  on-device pairing on iOS 27). The app claims one 1 GB region per launch
  (`JITBootstrap.jitBytes`) and the engine writes through a `vm_remap`'d alias
  (`host/jit_region.c`, split mode).
- **Memory**: `increased-memory-limit` is recommended, not required. Once a
  debugger has attached, guest RAM is kept out of the app's memory limit, and if
  iOS refuses that it comes from a file on free storage instead.
- **Machines** are installed on the iPhone from Apple's restore image
  (`Installer.swift`, [docs/architecture.md](../docs/architecture.md)). A machine
  can also be exported from the app, or made on a Mac with
  `tools/export_ios.sh DISK AUX ECID [NAME]` (disk packed with its holes left out,
  see `host/hvsparse.h`), and dropped on Henge in Finder or picked in Files.
- **Display**: iOS has no ParavirtualizedGraphics framework, so Henge replays the
  guest's GPU commands with Metal (`devices/pvg_lite.c`, `devices/pvg_metal.m`).

Code from Husk (GPL-2.0-or-later): `Henge/JIT/*`, `Henge/HengeLog.swift`,
`Henge/Native/henge-ios-jit.*`, `henge-brk.S`, `HengeJITHelper/`. StikJIT is
MPL-2.0 and idevice MIT (`Henge/Resources/legal/`); `tcg-apple-jit.h` is QEMU's
(LGPL-2.1). See [docs/licensing.md](../docs/licensing.md).
