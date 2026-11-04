# Henge for iOS

The Henge app on HengeVM's engine. Build: `ios/scripts/package_ipa.sh` (writes
`~/Desktop/Henge.ipa`; `--trollstore` writes `Henge.tipa` without the entitlements
TrollStore bans). Needs Xcode, xcodegen, and the prebuilt pieces
`ios/scripts/fetch_deps.sh` copies from Husk (StikJIT, the pairing library,
libslirp and glib for iOS).

- **JIT**: on iOS 26+ (TXM) only an attached debugger can grant executable
  memory. Henge uses the same machinery as Husk: StikDebug, TrollStore, a
  jailbreak, or the built-in StikJIT helper (`HengeJITHelper.appex`, with
  on-device pairing on iOS 27). The app claims one 512 MB region per launch
  (`JITBootstrap.jitBytes`) and the engine writes through a `vm_remap`'d alias
  (`host/jit_region.c`, split mode).
- **Memory**: `increased-memory-limit` is recommended, not required; guest RAM is
  sized to `os_proc_available_memory()` at start.
- **Machines** come from the Mac: `tools/export_ios.sh DISK AUX ECID [NAME]`
  makes a folder (disk packed with its holes left out, see `host/hvsparse.h`);
  drop it on Henge in Finder or pick it in Files.
- **Display**: not yet. iOS has no ParavirtualizedGraphics framework.

Code from Husk (GPL-2.0-or-later): `Henge/JIT/*`, `Henge/HengeLog.swift`,
`Henge/Native/henge-ios-jit.*`, `henge-brk.S`, `HengeJITHelper/`. StikJIT is
MPL-2.0 and idevice MIT (`Henge/Resources/legal/`); `tcg-apple-jit.h` is QEMU's
(LGPL-2.1).
