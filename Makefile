# Light build: one clang call per file, no parallel jobs needed.
CC = clang
CFLAGS = -std=c11 -Wall -Wextra -O2 -g -Icore -Ihost -Idevices
B = build

CORE = $(B)/classify.o $(B)/operands.o $(B)/translate.o $(B)/dispatch.o $(B)/mmu.o $(B)/memop.o \
       $(B)/exception.o $(B)/sysreg.o $(B)/machine.o
HOST = $(B)/jit_region.o $(B)/enter.o
DEV = $(B)/gicv3.o $(B)/timer.o $(B)/pl011.o $(B)/pl031.o $(B)/cfg.o $(B)/bdif.o $(B)/pcie.o \
      $(B)/virtio.o $(B)/virtio_blk.o $(B)/virtio_net.o
ENGINE = $(CORE) $(HOST)

TESTS = $(B)/test_classify $(B)/test_jit_region $(B)/test_operands $(B)/test_jit $(B)/test_mem \
        $(B)/test_mmu $(B)/test_exc $(B)/test_devices

all: $(TESTS)

$(B)/%.o: core/%.c core/*.h | $(B)
	$(CC) $(CFLAGS) -c $< -o $@
$(B)/%.o: host/%.c host/*.h | $(B)
	$(CC) $(CFLAGS) -c $< -o $@
$(B)/%.o: host/%.S | $(B)
	$(CC) -c $< -o $@
$(B)/%.o: devices/%.c devices/*.h core/*.h | $(B)
	$(CC) $(CFLAGS) -c $< -o $@

$(B)/test_classify: tests/test_classify.c tests/classify_cases.s $(B)/classify.o
	$(CC) $(CFLAGS) $^ -o $@
$(B)/test_jit_region: tests/test_jit_region.c $(B)/jit_region.o
	$(CC) $(CFLAGS) $^ -o $@
$(B)/test_operands: tests/test_operands.c tests/operand_cases.s $(B)/operands.o
	$(CC) $(CFLAGS) $^ -o $@
$(B)/test_mmu: tests/test_mmu.c $(ENGINE)
	$(CC) $(CFLAGS) tests/test_mmu.c $(ENGINE) -o $@

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
