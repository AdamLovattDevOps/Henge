# Light build: one clang call per file, no parallel jobs needed.
CC = clang
CFLAGS = -std=c11 -Wall -Wextra -O2 -g -Icore -Ihost -Idevices
B = build

CORE = $(B)/classify.o $(B)/operands.o $(B)/translate.o $(B)/dispatch.o $(B)/mmu.o $(B)/memop.o \
       $(B)/exception.o $(B)/sysreg.o $(B)/machine.o
HOST = $(B)/jit_region.o $(B)/enter.o $(B)/disk.o
DEV = $(B)/gicv3.o $(B)/timer.o $(B)/pl011.o $(B)/pl031.o $(B)/cfg.o $(B)/bdif.o $(B)/pcie.o \
      $(B)/virtio.o $(B)/virtio_blk.o $(B)/virtio_net.o $(B)/virtio_input.o $(B)/virtio_snd.o $(B)/xhci.o $(B)/vmapple.o $(B)/aes.o $(B)/pl061.o
ENGINE = $(CORE) $(HOST)

TESTS = $(B)/test_classify $(B)/test_jit_region $(B)/test_operands $(B)/test_jit $(B)/test_mem \
        $(B)/test_mmu $(B)/test_exc $(B)/test_devices

all: $(TESTS) $(B)/hengevm $(B)/hengevm-ref $(B)/hvsparse

# Mac-only test tool: the same machine with the CPU on Hypervisor.framework, for comparing runs.
$(B)/hengevm-ref: host/hvf_ref.c host/hvf_ref.entitlements $(ENGINE) $(DEV)
	$(CC) $(CFLAGS) host/hvf_ref.c $(ENGINE) $(DEV) -framework Hypervisor -o $@
	codesign -s - -f --entitlements host/hvf_ref.entitlements $@

# Mac-only graphics device (ParavirtualizedGraphics.framework, as QEMU uses it).
# Built for an older deployment target: the framework's memory callbacks are
# marked obsolete in macOS 27 without a documented replacement.
$(B)/pvg_mac.o: devices/pvg_mac.m devices/*.h core/*.h | $(B)
	$(CC) -fobjc-arc -O2 -g -Wall -mmacosx-version-min=15.2 -Icore -Ihost -Idevices -c $< -o $@

# User-mode NAT for the NIC: Homebrew's libslirp (brew install libslirp).
SLIRP_CFLAGS = $(shell pkg-config --cflags slirp)
SLIRP_LIBS = $(shell pkg-config --libs slirp)
$(B)/net_slirp.o: host/net_slirp.c host/net_slirp.h devices/virtio_net.h | $(B)
	$(CC) $(CFLAGS) $(SLIRP_CFLAGS) -c $< -o $@

$(B)/hengevm: host/main.c $(ENGINE) $(DEV) $(B)/pvg_mac.o $(B)/net_slirp.o $(B)/audio_mac.o $(B)/vm.o
	$(CC) $(CFLAGS) host/main.c $(ENGINE) $(DEV) $(B)/pvg_mac.o $(B)/net_slirp.o $(B)/audio_mac.o $(B)/vm.o $(SLIRP_LIBS) -framework AudioToolbox -framework ParavirtualizedGraphics -framework Metal \
	    -framework Foundation -framework CoreGraphics -framework ImageIO -framework CoreFoundation \
	    -framework AppKit -framework QuartzCore -o $@

$(B)/%.o: core/%.c core/*.h | $(B)
	$(CC) $(CFLAGS) -c $< -o $@
$(B)/%.o: host/%.c host/*.h | $(B)
	$(CC) $(CFLAGS) -c $< -o $@
$(B)/%.o: host/%.S | $(B)
	$(CC) -c $< -o $@
$(B)/%.o: devices/%.c devices/*.h core/*.h host/*.h | $(B)
	$(CC) $(CFLAGS) -c $< -o $@

$(B)/test_classify: tests/test_classify.c tests/classify_cases.s $(B)/classify.o
	$(CC) $(CFLAGS) $^ -o $@
$(B)/test_jit_region: tests/test_jit_region.c $(B)/jit_region.o
	$(CC) $(CFLAGS) $^ -o $@
$(B)/test_operands: tests/test_operands.c tests/operand_cases.s $(B)/operands.o
	$(CC) $(CFLAGS) $^ -o $@
$(B)/test_mmu: tests/test_mmu.c $(ENGINE)
	$(CC) $(CFLAGS) tests/test_mmu.c $(ENGINE) -o $@

$(B)/hvsparse: tools/hvsparse_main.c host/hvsparse.c host/hvsparse.h | $(B)
	$(CC) $(CFLAGS) tools/hvsparse_main.c host/hvsparse.c -o $@

# Block-level tests: test_X.c plus X_programs.S, run natively and through the JIT.
$(B)/test_devices: tests/test_devices.c tests/devices_programs.S tests/harness.S tests/harness.h $(ENGINE) $(DEV)
	$(CC) $(CFLAGS) -Itests tests/test_devices.c tests/devices_programs.S tests/harness.S $(ENGINE) $(DEV) -o $@
$(B)/test_%: tests/test_%.c tests/%_programs.S tests/harness.S tests/harness.h $(ENGINE)
	$(CC) $(CFLAGS) -Itests tests/test_$*.c tests/$*_programs.S tests/harness.S $(ENGINE) -o $@

test: all
	@for t in $(TESTS); do echo "== $$t"; $$t || exit 1; done

$(B):
	mkdir -p $(B)

clean:
	rm -rf $(B)

.PHONY: all test clean

# ---- iOS: the engine as a static library for the Henge app (ios/) ----
# make ios-lib -> build/ios/libhengevm.a. Networking links the arm64 iOS builds
# of libslirp and glib from Husk's sysroot (scripts there make them); see ios/README.md.
IOS_SDK = $(shell xcrun --sdk iphoneos --show-sdk-path)
IOS_CC = xcrun --sdk iphoneos clang -arch arm64 -miphoneos-version-min=17.0 -isysroot $(IOS_SDK)
IOS_DEPS ?= $(HOME)/Documents/Coding/Husk/build/ios-arm64/sysroot
IOS_CFLAGS = -std=c11 -Wall -Wextra -O2 -g -Icore -Ihost -Idevices -I$(IOS_DEPS)/include/slirp
IB = build/ios
IOS_SRC = $(wildcard core/*.c) host/jit_region.c host/disk.c host/vm.c host/net_slirp.c host/audio_mac.c host/hvsparse.c \
          $(filter-out devices/pvg_mac.m,$(wildcard devices/*.c))
IOS_OBJ = $(patsubst %.c,$(IB)/%.o,$(IOS_SRC)) $(IB)/host/enter.o

$(IB)/%.o: %.c core/*.h host/*.h devices/*.h
	@mkdir -p $(dir $@)
	$(IOS_CC) $(IOS_CFLAGS) -c $< -o $@
$(IB)/host/enter.o: host/enter.S
	@mkdir -p $(dir $@)
	$(IOS_CC) -c $< -o $@
$(IB)/libhengevm.a: $(IOS_OBJ)
	rm -f $@
	xcrun --sdk iphoneos libtool -static -o $@ $(IOS_OBJ)
ios-lib: $(IB)/libhengevm.a
.PHONY: ios-lib
