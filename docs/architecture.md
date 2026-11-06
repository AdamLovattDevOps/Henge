# How Henge works

A virtual Mac without a hypervisor: arm64 guest code runs through an arm64-to-arm64
JIT instead of hardware virtualization, so the same engine can run on iOS, where
apps have no hypervisor access. The engine is built and tested on the Mac
(`build/hengevm`) and linked into the iPhone app (`make ios-lib`).

## Layout

- `core/`: guest CPU state, the block translator, the software MMU, and exceptions.
- `devices/`: the virtual Mac's devices (interrupt controller, timers, UART, RTC,
  config region, AES, PCIe with virtio block, network, input, sound and USB, and
  the virtual GPU).
- `host/`: what runs around the engine (memory, JIT region, disk files, the
  install and USB shims, and the Mac's command-line tool).
- `ios/`: the iPhone app. See [ios/README.md](../ios/README.md).
- `tests/`: instruction-level and block-level tests that run on the Mac.
- `third_party/`: the restore stack, from upstream sources. See
  [licensing.md](licensing.md).

## How the translator works

The guest and host are both arm64, so most guest instructions are copied into a
translated block unchanged. Only these are rewritten:

- loads and stores, which go through the guest MMU (an inline TLB, with a slow
  path for misses and device accesses);
- PC-relative instructions and branches, which get the guest PC filled in;
  same-page branches chain directly to the target block, including conditional
  ones, and cross-page branches check that the page still maps the same code.
  Indirect branches go through a small cache and fall back to the dispatcher,
  and returns go through a per-CPU return table;
- system register access, exceptions, and barriers that touch guest state, which
  call helpers;
- pointer-authentication instructions, which become a strip or a no-op, since the
  host's keys cannot be the guest's;
- any instruction that uses a register the translator reserves for itself; those
  registers are kept in the guest context instead. Guest x6, x7, x15, x18 and
  x28 are kept in the CPU context, so the host's x6, x7 and x15 are free scratch.

Each block has a hot part and a cold part, so the paths rarely taken don't sit
between the ones that are.

Guest and host both use 16 KB pages, so guest RAM maps onto host pages directly.

## Device model

The machine follows the memory map and devices of QEMU's open-source `vmapple`
machine (hw/vmapple in QEMU, GPL-2.0-or-later). That code is the reference for
device behaviour, and `devices/aes.c` takes its command and register layout from
it. Nothing here comes from Virtualization.framework.

## Graphics

macOS draws through Apple's paravirtual GPU. iOS has no ParavirtualizedGraphics
framework, so `devices/pvg_lite.c` takes the guest's command streams and
`devices/pvg_metal.m` replays them with Metal.

## The boot ROM

The first code a virtual Mac runs is Apple's AVPBooter, which starts macOS or, for an
install, waits in DFU mode. It lives in macOS's system image, and since macOS 15 those are
encrypted, but the one in Apple's macOS 14.6.1 restore image is not, and its ROM runs later
versions. `ios/Henge/BootROM.swift` fetches it from Apple: the file is part of one
compressed block of that image, so one request of under a megabyte gets all of it. It then
checks the result against a SHA-256 hash. Nothing is decrypted.

## Sharing

`host/share.c` is a small WebDAV server, because macOS has a WebDAV client built in. The
guest reaches it at 10.0.2.4, which the user-mode network hands to a Unix socket in the
app's storage: no TCP port is open, so other apps on the device can't reach it. Under
`/.henge/` it also has the clipboard and what it takes to install `tools/henge-agent.m`, a
small helper that runs inside macOS as a LaunchAgent, mounts the folder at `~/Henge` and
follows the clipboard. The app types the set up command but never presses Return: it can't
see what has the keyboard on the Mac.

## Installing macOS (no Mac needed)

`hengevm --install IPSW` (and the iOS app's Install macOS) install macOS on a
blank machine the way Virtualization.framework's installer does: the boot ROM
starts in DFU mode (cfg `run_installer`) and shows up as a USB device, and a
standard restore runs over that link. Apple's signing server personalizes the
boot chain for the machine's ECID, as for any Mac; nothing is decrypted or
patched on the host (encrypted images go to the guest as they are, and the
restore OS asks for its keys itself; the host only fetches the URL it names).

- The USB port: Apple's virtio USB device controller, PCI 0x106b:0x1a01 for the
  kernel (`AppleVirtIOUSBDeviceController`), and channel 0x100000 of the boot
  ROM's bdif MMIO transport for AVPBooter and iBoot (devices/virtio_usb.c,
  devices/bdif.c). Requests (host to guest) carry {u32 len, u8 endpoint, u8 type}
  and the data; responses {u8 type, u8 endpoint} and the data.
- The host side: libirecovery, usbmuxd and idevicerestore (third_party, upstream
  sources; the boot-ROM exploit is left out) linked against host/libusb_shim.c,
  a libusb whose one device is that port. usbmuxd sends 16 KB transfers (each
  request is one read on the device side), and ASR sends wait for a slow disk.
- iOS: tools/build_ios_restore.sh builds the same stack (with libplist,
  libimobiledevice, libtatsu, libzip, mbedtls for compiling only) and
  host/curl_shim.m puts libcurl's easy API on NSURLSession.
