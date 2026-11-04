# HengeVM

A virtual Mac without a hypervisor: arm64 guest code runs through an arm64-to-arm64
JIT instead of hardware virtualization, so the same engine can run on iOS, where
apps have no hypervisor access. Built and tested on the Mac first, then ported to
iOS. This is separate from Henge, whose Mac app uses Virtualization.framework.

## Layout

- `core/`: guest CPU state, the block translator, the software MMU, and exceptions.
- `devices/`: the virtual Mac's devices (interrupt controller, timers, UART, RTC,
  config region, PCIe with virtio block and network).
- `host/`: the macOS host harness (memory, JIT region, disk files, logging). The
  iOS host comes later.
- `tests/`: instruction-level and block-level tests that run on the Mac.

## How the translator works

The guest and host are both arm64, so most guest instructions are copied into a
translated block unchanged. Only these are rewritten:

- loads and stores, which go through the guest MMU (an inline TLB, with a slow
  path for misses and device accesses);
- PC-relative instructions and branches, which get the guest PC filled in;
  same-page branches chain directly to the target block, and indirect branches
  go through a small cache and fall back to the dispatcher;
- system register access, exceptions, and barriers that touch guest state, which
  call helpers;
- pointer-authentication instructions, which become a strip or a no-op, since the
  host's keys cannot be the guest's;
- any instruction that uses a register the translator reserves for itself; those
  registers are kept in the guest context instead. Guest x6, x7, x15, x18 and
  x28 are kept in the CPU context, so the host's x6, x7 and x15 are free scratch.

Guest and host both use 16 KB pages, so guest RAM maps onto host pages directly.

## Device model

The machine follows the memory map and devices of QEMU's open-source `vmapple`
machine (hw/vmapple in QEMU, GPL-2.0-or-later). That code is the reference for
device behaviour; nothing here comes from Virtualization.framework.

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

## Milestones

1. The translator runs small arm64 test programs at EL1 with the MMU off, and the
   results match running the same code natively.
2. MMU on: page-table walks, the TLB, and exceptions and interrupts; the timer and
   GIC deliver interrupts.
3. The device model, then booting the guest disk made by Henge's Mac app.
4. Display, input, and networking.
5. The iOS host: JIT through StikDebug, the memory limits, and the UI.
