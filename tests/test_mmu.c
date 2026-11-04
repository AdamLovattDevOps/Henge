// Stage 1 walker: granules, block sizes, TTBR0/TTBR1, TBI, permissions and
// fault codes, checked directly against hv_mmu_walk.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "machine.h"
#include "mmu.h"
#include "sysreg.h"

#define PT_PA 0x80000000ull
#define PT_SIZE (1u << 20)

static uint8_t *pt;
static uint64_t pt_next;
static int failures, checks;

static uint64_t *host(uint64_t pa) {
    return (uint64_t *)(pt + (pa - PT_PA));
}

static uint64_t alloc_table(void) {
    uint64_t pa = PT_PA + pt_next;
    pt_next += 65536; /* big enough and aligned for any granule */
    memset(host(pa), 0, 65536);
    return pa;
}

/* Install a leaf for va at `leaf_level`, creating tables from the start level. */
static uint64_t *map(uint64_t root, int g, int inputsize, uint64_t va, int leaf_level, uint64_t desc,
                     uint64_t table_bits) {
    int stride = g - 3;
    int levels = (inputsize - g + stride - 1) / stride;
    uint64_t table = root;
    for (int level = 4 - levels;; level++) {
        int shift = g + stride * (3 - level);
        int idx_bits = level == 4 - levels ? inputsize - shift : stride;
        uint64_t *e = &host(table)[(va >> shift) & ((1ull << idx_bits) - 1)];
        if (level == leaf_level) {
            *e = desc;
            return e;
        }
        if (!(*e & 1)) *e = alloc_table() | 3 | table_bits;
        table = *e & 0x0000fffffffff000ull & ~((1ull << g) - 1);
    }
}

#define AF (1ull << 10)
#define PAGE 3ull
#define BLOCK 1ull
#define AP(x) ((uint64_t)(x) << 6)
#define PXN (1ull << 53)
#define UXN (1ull << 54)

static void expect(const char *what, HvCpu *cpu, uint64_t va, HvAccess acc, int el, uint64_t want_pa, uint32_t want_fsc) {
    HvWalk w;
    hv_mmu_walk(cpu, va, acc, el, &w);
    checks++;
    if (w.fsc != want_fsc || (!want_fsc && w.pa != want_pa)) {
        printf("  FAIL %s: va %llx -> pa %llx fsc %02x, expected pa %llx fsc %02x\n", what, (unsigned long long)va,
               (unsigned long long)w.pa, w.fsc, (unsigned long long)want_pa, want_fsc);
        failures++;
    }
}

int main(void) {
    static HvMachine m;
    static HvCpu cpu;
    hv_machine_init(&m, 1 << 20);
    pt = aligned_alloc(65536, PT_SIZE);
    hv_machine_add_ram(&m, PT_PA, PT_SIZE, pt, 0);
    hv_cpu_reset(&cpu, &m);

    expect("mmu off", &cpu, 0x12345678, HV_ACC_WRITE, 1, 0x12345678, 0);

    /* 16 KB granule, 48-bit TTBR0 (four levels), 39-bit TTBR1 (three levels). */
    uint64_t r0 = alloc_table(), r1 = alloc_table();
    cpu.ttbr0_el1 = r0 | 0x1234ull << 48; /* an ASID must not leak into the table address */
    cpu.ttbr1_el1 = r1;
    cpu.mair_el1 = 0x00ff;
    cpu.tcr_el1 = 16 | 2u << 14 | 25u << 16 | 1u << 30 | 5ull << 32 | 1ull << 37; /* T0SZ 16, TG0 16K, T1SZ 25, TG1 16K, TBI0 */
    cpu.sctlr_el1 |= 1;

    map(r0, 14, 48, 0x100004000, 3, 0x40008000 | PAGE | AF | AP(0), 0);
    expect("16K page", &cpu, 0x100004abc, HV_ACC_READ, 1, 0x40008abc, 0);
    expect("16K page, next page unmapped", &cpu, 0x100008000, HV_ACC_READ, 1, 0, HV_FSC_TRANSLATION(3));
    expect("16K, no level 1 table", &cpu, 0x7f0000000000, HV_ACC_READ, 1, 0, HV_FSC_TRANSLATION(1));
    expect("16K, VA above T0SZ", &cpu, 0x0001000000000000, HV_ACC_READ, 1, 0, HV_FSC_TRANSLATION(0));
    expect("TBI0 ignores the top byte", &cpu, 0x5a00000100004010, HV_ACC_READ, 1, 0x40008010, 0);
    cpu.tcr_el1 &= ~(1ull << 37);
    expect("without TBI0 a tag faults", &cpu, 0x5a00000100004010, HV_ACC_READ, 1, 0, HV_FSC_TRANSLATION(0));
    cpu.tcr_el1 |= 1ull << 37;

    map(r0, 14, 48, 0x200000000, 2, 0x82000000 | BLOCK | AF, 0);
    expect("16K: 32 MB block", &cpu, 0x201234560, HV_ACC_WRITE, 1, 0x83234560, 0);
    map(r0, 14, 48, 0x300000000, 3, 0x40010000 | PAGE, 0);
    expect("access flag clear", &cpu, 0x300000000, HV_ACC_READ, 1, 0, HV_FSC_ACCESS(3));

    /* Permissions. */
    map(r0, 14, 48, 0x100010000, 3, 0x40010000 | PAGE | AF | AP(2), 0); /* EL1 RO */
    expect("EL1 RO: read", &cpu, 0x100010000, HV_ACC_READ, 1, 0x40010000, 0);
    expect("EL1 RO: write", &cpu, 0x100010000, HV_ACC_WRITE, 1, 0, HV_FSC_PERMISSION(3));
    expect("EL1 only: EL0 read", &cpu, 0x100004000, HV_ACC_READ, 0, 0, HV_FSC_PERMISSION(3));
    map(r0, 14, 48, 0x100014000, 3, 0x40014000 | PAGE | AF | AP(1), 0); /* EL0 and EL1 RW */
    expect("EL0 RW: EL0 write", &cpu, 0x100014000, HV_ACC_WRITE, 0, 0x40014000, 0);
    expect("EL0 RW: EL1 cannot execute", &cpu, 0x100014000, HV_ACC_EXEC, 1, 0, HV_FSC_PERMISSION(3));
    expect("EL0 RW: EL0 executes", &cpu, 0x100014000, HV_ACC_EXEC, 0, 0x40014000, 0);
    cpu.pstate_extra |= HV_PSTATE_PAN;
    expect("PAN: EL1 read of an EL0 page", &cpu, 0x100014000, HV_ACC_READ, 1, 0, HV_FSC_PERMISSION(3));
    expect("PAN: EL1 page unaffected", &cpu, 0x100004000, HV_ACC_READ, 1, 0x40008000, 0);
    expect("PAN: LDTR uses EL0 rights", &cpu, 0x100014000, HV_ACC_READ, 0, 0x40014000, 0);
    cpu.pstate_extra &= ~(uint64_t)HV_PSTATE_PAN;
    map(r0, 14, 48, 0x100018000, 3, 0x40018000 | PAGE | AF | AP(3) | UXN, 0); /* RO both, EL0 XN */
    expect("UXN: EL0 exec", &cpu, 0x100018000, HV_ACC_EXEC, 0, 0, HV_FSC_PERMISSION(3));
    expect("UXN: EL1 exec", &cpu, 0x100018000, HV_ACC_EXEC, 1, 0x40018000, 0);
    map(r0, 14, 48, 0x10001c000, 3, 0x4001c000 | PAGE | AF | PXN, 0);
    expect("PXN: EL1 exec", &cpu, 0x10001c000, HV_ACC_EXEC, 1, 0, HV_FSC_PERMISSION(3));
    cpu.sctlr_el1 |= 1u << 19;
    expect("WXN: writable is not executable", &cpu, 0x100004000, HV_ACC_EXEC, 1, 0, HV_FSC_PERMISSION(3));
    cpu.sctlr_el1 &= ~(1ull << 19);
    expect("no WXN: executable", &cpu, 0x100004000, HV_ACC_EXEC, 1, 0x40008000, 0);

    /* APTable on a new level 1 table: bit 61 = no EL0, bit 62 = no writes. */
    map(r0, 14, 48, 0x500000000000, 3, 0x40020000 | PAGE | AF | AP(1), 1ull << 61);
    expect("APTable no-EL0", &cpu, 0x500000000000, HV_ACC_READ, 0, 0, HV_FSC_PERMISSION(3));
    map(r0, 14, 48, 0x600000000000, 3, 0x40024000 | PAGE | AF | AP(0), 1ull << 62);
    expect("APTable read-only", &cpu, 0x600000000000, HV_ACC_WRITE, 1, 0, HV_FSC_PERMISSION(3));
    expect("APTable read-only: read", &cpu, 0x600000000000, HV_ACC_READ, 1, 0x40024000, 0);

    /* TTBR1, 39-bit: starts at level 1. */
    map(r1, 14, 39, 0xffffffc012340000, 3, 0x40028000 | PAGE | AF, 0);
    expect("TTBR1 page", &cpu, 0xffffffc012343210, HV_ACC_READ, 1, 0x4002b210, 0);
    expect("TTBR1: hole between ranges", &cpu, 0xffff000012340000, HV_ACC_READ, 1, 0, HV_FSC_TRANSLATION(0));
    cpu.tcr_el1 |= 1u << 23;
    expect("EPD1 disables TTBR1 walks", &cpu, 0xffffffc012343210, HV_ACC_READ, 1, 0, HV_FSC_TRANSLATION(0));
    cpu.tcr_el1 &= ~(1ull << 23);

    /* The TLB only takes 16 KB or larger mappings. */
    uint64_t pa;
    hv_tlb_flush(&cpu);
    hv_mmu_access(&cpu, 0x100004000, HV_ACC_READ, 1, &pa);
    checks++;
    /* 0x40008000 is not RAM here, so nothing may be filled */
    if (cpu.tlb[(0x100004000 >> 14) & (HV_TLB_SIZE - 1)][1].read_tag != HV_TLB_INVALID) {
        printf("  FAIL TLB filled for non-RAM\n");
        failures++;
    }

    /* 4 KB granule, 39-bit TTBR0 (levels 1-3): pages, 2 MB and 1 GB blocks. */
    pt_next = 2 * 65536;
    uint64_t r4 = alloc_table();
    cpu.ttbr0_el1 = r4;
    cpu.tcr_el1 = 25 | 0u << 14 | 1u << 23 | 5ull << 32;
    map(r4, 12, 39, 0x12345000, 3, 0x40001000 | PAGE | AF, 0);
    expect("4K page", &cpu, 0x12345678, HV_ACC_READ, 1, 0x40001678, 0);
    map(r4, 12, 39, 0x40200000, 2, 0x80400000 | BLOCK | AF, 0);
    expect("4K: 2 MB block", &cpu, 0x40212345, HV_ACC_READ, 1, 0x80412345, 0);
    map(r4, 12, 39, 0x4000000000 - 0x40000000, 1, 0xc0000000 | BLOCK | AF, 0);
    expect("4K: 1 GB block", &cpu, 0x3fc0123456, HV_ACC_READ, 1, 0xc0123456, 0);
    hv_tlb_flush(&cpu);
    hv_mmu_access(&cpu, 0x12345678, HV_ACC_READ, 1, &pa);
    checks++;
    if (cpu.tlb[(0x12345678 >> 14) & (HV_TLB_SIZE - 1)][1].read_tag != HV_TLB_INVALID) {
        printf("  FAIL a 4K mapping went into the 16K TLB\n");
        failures++;
    }

    /* 64 KB granule, 42-bit TTBR0 (levels 2-3): pages and 512 MB blocks. */
    uint64_t r6 = alloc_table();
    cpu.ttbr0_el1 = r6;
    cpu.tcr_el1 = 22 | 1u << 14 | 1u << 23 | 5ull << 32;
    map(r6, 16, 42, 0x123450000, 3, 0x40050000 | PAGE | AF, 0);
    expect("64K page", &cpu, 0x12345abcd, HV_ACC_READ, 1, 0x4005abcd, 0);
    map(r6, 16, 42, 0x200000000, 2, 0x60000000 | BLOCK | AF, 0);
    expect("64K: 512 MB block", &cpu, 0x21234abcd, HV_ACC_READ, 1, 0x7234abcd, 0);
    map(r6, 16, 42, 0x300000000, 3, 0x40060000 | BLOCK | AF, 0);
    expect("block descriptor at level 3 is invalid", &cpu, 0x300000000, HV_ACC_READ, 1, 0, HV_FSC_TRANSLATION(3));

    /* TLBI RVAE1IS: 6 pages of 16 KB from the base drop, the ones after stay; also for a kernel range. */
    {
        static const uint64_t bases[2] = {0x100000000ull, 0xfffffe0012340000ull};
        for (int b = 0; b < 2; b++) {
            hv_tlb_flush(&cpu);
            int table = (bases[b] >> 55) & 1 ? 1 : 0;
            for (int k = 0; k < 8; k++) {
                uint64_t va = bases[b] + ((uint64_t)k << 14);
                HvTlbEntry *t = &cpu.tlb[(va >> 14) & (HV_TLB_SIZE - 1)][table];
                t->read_tag = va - HV_TLB_ADJ;
                t->addend = 0;
            }
            cpu.el = 1;
            cpu.x[1] = 5ull << 48 | 2ull << 46 | 0ull << 44 | 2ull << 39 | ((bases[b] >> 14) & 0x1fffffffffull);
            hv_sysreg_sys(&cpu, 0xd5088221u, 0); /* tlbi rvae1is, x1 */
            for (int k = 0; k < 8; k++) {
                uint64_t va = bases[b] + ((uint64_t)k << 14);
                int dropped = cpu.tlb[(va >> 14) & (HV_TLB_SIZE - 1)][table].read_tag == HV_TLB_INVALID;
                checks++;
                if (dropped != (k < 6)) {
                    printf("  FAIL range TLBI, base %llx page %d: %s\n", (unsigned long long)bases[b], k,
                           dropped ? "dropped" : "kept");
                    failures++;
                }
            }
        }
    }

    /* A TLBI by VA and ASID forgets the indirect-branch cache entries of that page
       for that address space, and nothing else. Entries are filled the way the dispatcher does it. */
    {
        hv_cpu_reset(&cpu, &m);
        cpu.tcr_el1 = 0;
        cpu.ttbr0_el1 = 7ull << 48;
        hv_ijc_switch(&cpu);
        uint64_t salt = cpu.ijc_salt[0];
        static const uint64_t vas[3] = {0x100004010ull, 0x100007ffcull, 0x100008010ull}; /* page P twice, then page Q */
        uint64_t *ents[3];
        static HvBlock fake[3];
        for (int i = 0; i < 3; i++) {
            fake[i] = (HvBlock){.va = vas[i], .flags = 0, .code = (uint32_t *)(uintptr_t)0x1234};
            hv_ijc_fill(&cpu, &fake[i]);
            ents[i] = m.percpu[0].ijc + (size_t)hv_ijc_index(vas[i] ^ salt) * 2;
        }
        hv_tlb_flush_va_asid(&cpu, 0x100004000ull, 7);
        int want[3] = {0, 0, 1};
        for (int i = 0; i < 3; i++) {
            checks++;
            if ((ents[i][0] != 0) != want[i]) {
                printf("  FAIL branch cache after TLBI of page P: entry %d %s\n", i, ents[i][0] ? "kept" : "dropped");
                failures++;
            }
        }
        /* Another address space's TLBI of page Q leaves this one's entry alone. */
        hv_tlb_flush_va_asid(&cpu, 0x100008000ull, 9);
        checks++;
        if (!ents[2][0]) {
            printf("  FAIL another ASID's TLBI dropped our entry\n");
            failures++;
        }
        hv_tlb_flush_va_asid(&cpu, 0x100008000ull, -1); /* all address spaces */
        checks++;
        uint64_t now = vas[2] ^ cpu.ijc_salt[0]; /* what a lookup would compare against now */
        if (m.percpu[0].ijc[(size_t)hv_ijc_index(now) * 2] == now) {
            printf("  FAIL an all-ASID TLBI kept the entry\n");
            failures++;
        }
    }

    printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
}
