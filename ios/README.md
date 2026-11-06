# Henge for iOS

The Henge app on HengeVM's engine. Build: `ios/scripts/package_ipa.sh` (writes
`~/Desktop/Henge.ipa`; `--trollstore` writes `Henge.tipa`, for TrollStore and
TrollStore Lite, without the entitlements TrollStore bans). Needs Xcode 27 or later, xcodegen,
and the prebuilt pieces
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
- **Boot ROM**: fetched from Apple at install, not shipped (`Henge/BootROM.swift`,
  [docs/architecture.md](../docs/architecture.md)).
- **Display**: iOS has no ParavirtualizedGraphics framework, so Henge replays the
  guest's GPU commands with Metal (`devices/pvg_lite.c`, `devices/pvg_metal.m`).
- **External display (iOS 27+)**: `SessionView` registers a SwiftUI
  `ExternalNonInteractiveAccessory` during a Mac session. `DisplayRouter` selects
  one live `CAMetalLayer`, preferring the external surface and returning to the
  device on disconnect. The phone's session owns the VM and all input; the
  accessory only presents pixels. The existing guest resolution is unchanged
  (1920×1080 by default on iPhone), fitted to the monitor without stretching.
  USB-C video and a hub's keyboard/mouse can be used together when iOS recognizes
  the peripherals. iOS 17–26 keep system mirroring; AirPlay may also provide an
  external scene, with transport-dependent latency. Run `make test-display-routing`
  for routing lifecycle checks; `ios/scripts/check_display.sh` also compiles all
  app Swift sources and the iOS/macOS renderer without linking external libraries.
  Real-device acceptance: connect before and during
  boot, unplug/replug with an idle desktop, rotate the phone, test keyboard/mouse
  through the dock, and end the session while connected. Monitor hardware testing
  is still required; these checks are not a claim of verified USB-C performance.
- **Sharing** (`Henge/Sharing.swift`, `host/share.c`,
  [docs/architecture.md](../docs/architecture.md)): the Shared folder in Files and
  the clipboard. The Mac's clipboard follows along; going the other way uses the
  system paste button, since iOS asks permission when an app reads it on its own.
- **Hardware input** (`Henge/Hardware.swift`): a keyboard's keys go to the Mac's
  USB keyboard by HID usage, which is how iOS reports them. A trackpad or mouse
  goes through GameController with the pointer locked, which iPadOS allows only
  for an app filling the screen. The first scroll values are logged to
  `henge.log`, to tune scrolling from a real device.
- **Chips**: the virtual CPU offers Armv8.5 instructions (`core/sysreg.c`), which
  macOS uses and which run as they are, so Henge needs an A14 or M1 or newer.

Code from Husk (GPL-2.0-or-later): `Henge/JIT/*`, `Henge/HengeLog.swift`,
`Henge/Native/henge-ios-jit.*`, `henge-brk.S`, `HengeJITHelper/`. StikJIT is
MPL-2.0 and idevice MIT (`Henge/Resources/legal/`); `tcg-apple-jit.h` is QEMU's
(LGPL-2.1). See [docs/licensing.md](../docs/licensing.md).
