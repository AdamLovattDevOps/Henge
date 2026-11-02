#include "gicv3.h"

#include <stdlib.h>
#include <string.h>

/* Register layout follows the GICv3 architecture specification (IHI 0069);
   behaviour is checked against QEMU's hw/intc/arm_gicv3*. */

#define GICD_CTLR_EN_GRP0 1u
#define GICD_CTLR_EN_GRP1 2u
#define GICD_CTLR_ARE     (1u << 4)
#define GICD_CTLR_DS      (1u << 6)
#define SPURIOUS 1023
#define IDLE_PRIORITY 0x100

#define R(op0, op1, crn, crm, op2) ((op0) << 14 | (op1) << 11 | (crn) << 7 | (crm) << 3 | (op2))

static int pending(HvGic *g, int i) {
    return g->latch[i] || (!g->edge[i] && g->level[i]);
}

static int group_enabled(HvGic *g, int grp) {
    return grp ? (g->ctlr & GICD_CTLR_EN_GRP1) && (g->igrpen1 & 1) : (g->ctlr & GICD_CTLR_EN_GRP0) && (g->igrpen0 & 1);
}

static int running_priority(HvGic *g) {
    return g->depth ? g->stack[g->depth - 1].prio : IDLE_PRIORITY;
}

/* Highest-priority pending interrupt; with `masked`, only one PMR and the running priority let through. */
static int hppi(HvGic *g, int masked) {
    int best = SPURIOUS, best_prio = 0x100;
    for (int i = 0; i < HV_GIC_NUM_IRQS; i++) {
        if (!g->enabled[i] || g->active[i] || !pending(g, i) || !group_enabled(g, g->group[i])) continue;
        if (g->priority[i] < best_prio) {
            best = i;
            best_prio = g->priority[i];
        }
    }
    if (best == SPURIOUS) return SPURIOUS;
    if (masked && (best_prio >= (int)(g->pmr & 0xff) || best_prio >= running_priority(g))) return SPURIOUS;
    return best;
}

void hv_gicv3_update(HvGic *g) {
    int i = hppi(g, 1);
    g->m->irq_line = i != SPURIOUS && g->group[i] == 1;
    g->m->fiq_line = i != SPURIOUS && g->group[i] == 0;
}

void hv_gicv3_set_irq(HvGic *g, int intid, int level) {
    if (intid < 16 || intid >= HV_GIC_NUM_IRQS) return;
    level = level != 0;
    if (g->level[intid] == level) return; /* nothing changed: skip the rescan (the timer re-asserts every tick) */
    if (g->edge[intid] && level) g->latch[intid] = 1;
    g->level[intid] = (uint8_t)level;
    hv_gicv3_update(g);
}

void hv_gicv3_pend(HvGic *g, int intid) {
    if (intid < 32 || intid >= HV_GIC_NUM_IRQS) return;
    g->latch[intid] = 1;
    hv_gicv3_update(g);
}

/* ---- the CPU interface ---- */

static uint64_t acknowledge(HvGic *g, int grp) {
    int i = hppi(g, 1);
    if (i == SPURIOUS || g->group[i] != grp) return SPURIOUS;
    g->active[i] = 1;
    g->latch[i] = 0;
    if (g->depth < 32) g->stack[g->depth++] = (HvGicActive){(uint16_t)i, g->priority[i], (uint8_t)grp};
    hv_gicv3_update(g);
    return (uint64_t)i;
}

static void end_of_interrupt(HvGic *g, int intid) {
    if (intid >= HV_GIC_NUM_IRQS) return;
    if (g->depth) g->depth--; /* priority drop: the most recent acknowledge */
    if (!(g->icc_ctlr & 2)) g->active[intid] = 0; /* EOImode 0 also deactivates */
    hv_gicv3_update(g);
}

static uint64_t icc_read(HvMachine *m, HvCpu *cpu, uint32_t reg) {
    (void)cpu;
    HvGic *g = m->gic;
    int h;
    switch (reg) {
    case R(3, 0, 4, 6, 0): return g->pmr;
    case R(3, 0, 12, 12, 0): return acknowledge(g, 1);                 /* IAR1 */
    case R(3, 0, 12, 8, 0): return acknowledge(g, 0);                  /* IAR0 */
    case R(3, 0, 12, 12, 2):                                           /* HPPIR1 */
        h = hppi(g, 0);
        return h != SPURIOUS && g->group[h] == 1 ? (uint64_t)h : SPURIOUS;
    case R(3, 0, 12, 8, 2):                                            /* HPPIR0 */
        h = hppi(g, 0);
        return h != SPURIOUS && g->group[h] == 0 ? (uint64_t)h : SPURIOUS;
    case R(3, 0, 12, 11, 3): return running_priority(g) == IDLE_PRIORITY ? 0xff : (uint64_t)running_priority(g); /* RPR */
    case R(3, 0, 12, 8, 3): return g->bpr0;
    case R(3, 0, 12, 12, 3): return g->bpr1;
    case R(3, 0, 12, 12, 4): return g->icc_ctlr | 7u << 8 | 1u << 14;  /* CTLR: 8 priority bits, 16-bit IDs, SEIS 0, RSS */
    case R(3, 0, 12, 12, 5): return 7;                                 /* SRE: system registers only */
    case R(3, 0, 12, 12, 6): return g->igrpen0;
    case R(3, 0, 12, 12, 7): return g->igrpen1;
    case R(3, 0, 12, 8, 4): return (uint32_t)g->ap0r;
    case R(3, 0, 12, 9, 0): return (uint32_t)g->ap1r;
    default: return 0;                                                 /* AP*R1-3 and the rest */
    }
}

static void icc_write(HvMachine *m, HvCpu *cpu, uint32_t reg, uint64_t v) {
    HvGic *g = m->gic;
    switch (reg) {
    case R(3, 0, 4, 6, 0): g->pmr = v & 0xff; break;
    case R(3, 0, 12, 12, 1): case R(3, 0, 12, 8, 1): end_of_interrupt(g, (int)(v & 0xffffff)); break; /* EOIR1/0 */
    case R(3, 0, 12, 11, 1):                                           /* DIR */
        if ((v & 0xffffff) < HV_GIC_NUM_IRQS) g->active[v & 0xffffff] = 0;
        break;
    case R(3, 0, 12, 8, 3): g->bpr0 = v & 7; break;
    case R(3, 0, 12, 12, 3): g->bpr1 = v & 7; break;
    case R(3, 0, 12, 12, 4): g->icc_ctlr = v & 3; break;              /* CBPR, EOImode */
    case R(3, 0, 12, 12, 6): g->igrpen0 = v & 1; break;
    case R(3, 0, 12, 12, 7): g->igrpen1 = v & 1; break;
    case R(3, 0, 12, 8, 4): g->ap0r = (uint32_t)v; break;
    case R(3, 0, 12, 9, 0): g->ap1r = (uint32_t)v; break;
    case R(3, 0, 12, 11, 5): case R(3, 0, 12, 11, 6): case R(3, 0, 12, 11, 7): { /* SGI1R, ASGI1R, SGI0R */
        int intid = (v >> 24) & 0xf;
        int irm = (v >> 40) & 1;
        uint64_t aff = (v >> 16 & 0xff) | (v >> 32 & 0xff) << 8 | (v >> 48 & 0xff) << 16;
        uint64_t mine = cpu->mpidr & 0xffffff;
        /* IRM=1 means every PE but this one; with one PE that is nobody. */
        if (!irm && aff == (mine >> 8) && ((v >> 44) & 0xf) == 0 && ((v >> (mine & 0xf)) & 1)) g->latch[intid] = 1;
        break;
    }
    default: break;
    }
    hv_gicv3_update(g);
}

/* ---- MMIO: bitmap, byte and route registers shared by GICD and GICR ---- */

/* Registers indexed by INTID, starting at `first` for this frame. */
static uint32_t bank_read(HvGic *g, uint32_t off, int first, int count) {
    uint32_t v = 0;
    int base;
    if (off >= 0x400 && off < 0x800) { /* IPRIORITYR: one byte each */
        base = first + (int)(off - 0x400);
        for (int b = 0; b < 4; b++)
            if (base + b - first < count) v |= (uint32_t)g->priority[base + b] << (b * 8);
        return v;
    }
    if (off >= 0xc00 && off < 0xd00) { /* ICFGR: two bits each */
        base = first + (int)(off - 0xc00) * 4;
        for (int b = 0; b < 16; b++)
            if (base + b - first < count && g->edge[base + b]) v |= 2u << (b * 2);
        return v;
    }
    uint32_t reg = off & ~0x7fu, idx = (off & 0x7f) / 4;
    base = first + (int)idx * 32;
    for (int b = 0; b < 32; b++) {
        int i = base + b;
        if (i - first >= count) break;
        int bit = 0;
        switch (reg) {
        case 0x080: bit = g->group[i]; break;
        case 0x100: case 0x180: bit = g->enabled[i]; break;
        case 0x200: case 0x280: bit = pending(g, i); break;
        case 0x300: case 0x380: bit = g->active[i]; break;
        case 0xd00: bit = g->grpmod[i]; break;
        default: break;
        }
        v |= (uint32_t)bit << b;
    }
    return v;
}

/* mask has ones in the bytes that were written. */
static void bank_write(HvGic *g, uint32_t off, uint32_t v, uint32_t mask, int first, int count, int sgi_ppi) {
    int base;
    if (off >= 0x400 && off < 0x800) {
        base = first + (int)(off - 0x400);
        for (int b = 0; b < 4; b++)
            if ((mask >> (b * 8)) & 0xff && base + b - first < count) g->priority[base + b] = (uint8_t)(v >> (b * 8));
        return;
    }
    if (off >= 0xc00 && off < 0xd00) {
        base = first + (int)(off - 0xc00) * 4;
        for (int b = 0; b < 16; b++) {
            int i = base + b;
            if (i - first >= count || !((mask >> (b * 2)) & 3)) continue;
            if (sgi_ppi && i < 16) continue; /* SGIs are always edge */
            g->edge[i] = (v >> (b * 2 + 1)) & 1;
        }
        return;
    }
    uint32_t reg = off & ~0x7fu, idx = (off & 0x7f) / 4;
    base = first + (int)idx * 32;
    for (int b = 0; b < 32; b++) {
        int i = base + b;
        if (i - first >= count) break;
        if (!((mask >> b) & 1)) continue;
        int bit = (v >> b) & 1;
        switch (reg) {
        case 0x080: g->group[i] = (uint8_t)bit; break;
        case 0x100: if (bit) g->enabled[i] = 1; break;
        case 0x180: if (bit) g->enabled[i] = 0; break;
        case 0x200: if (bit) g->latch[i] = 1; break;
        case 0x280: if (bit) g->latch[i] = 0; break;
        case 0x300: if (bit) g->active[i] = 1; break;
        case 0x380: if (bit) g->active[i] = 0; break;
        case 0xd00: g->grpmod[i] = (uint8_t)bit; break;
        default: break;
        }
    }
}

static uint32_t size_mask(uint64_t off, unsigned size) {
    uint32_t m = size >= 4 ? 0xffffffffu : (1u << (size * 8)) - 1;
    return m << ((off & 3) * 8);
}

/* Registers for INTIDs 0-31. In the distributor they are reserved (ARE is set);
   the redistributor's SGI frame has only these. */
static int bank0(uint32_t a) {
    if (a >= 0x400 && a < 0x800) return a < 0x420;
    if (a >= 0xc00 && a < 0xd00) return a < 0xc08;
    return (a & 0x7f) < 4;
}

static uint64_t dist_read(void *opaque, uint64_t off, unsigned size) {
    HvGic *g = opaque;
    if (off >= 0x6100 && off < 0x6100 + 8 * HV_GIC_NUM_IRQS) { /* IROUTER<n>, n >= 32 */
        uint64_t v = g->route[(off - 0x6000) / 8];
        return size == 8 ? v : (uint32_t)(v >> ((off & 4) * 8));
    }
    uint32_t word, a = (uint32_t)off & ~3u;
    switch (a) {
    case 0x0000: word = g->ctlr | GICD_CTLR_ARE | GICD_CTLR_DS; break;
    case 0x0004: word = (HV_GIC_NUM_IRQS / 32 - 1) | 15u << 19; break; /* ITLinesNumber, 16 ID bits */
    case 0x0008: word = 0x0300043b; break;                             /* IIDR: Arm, GICv3 */
    case 0xffe8: word = 0x3b; break;                                   /* PIDR2: ArchRev 3 */
    default:
        word = a >= 0x80 && a < 0x1000 && !bank0(a) ? bank_read(g, a, 0, HV_GIC_NUM_IRQS) : 0;
        break;
    }
    return (word >> ((off & 3) * 8)) & (size >= 4 ? 0xffffffffu : (1u << (size * 8)) - 1);
}

static void dist_write(void *opaque, uint64_t off, uint64_t v, unsigned size) {
    HvGic *g = opaque;
    if (off >= 0x6100 && off < 0x6100 + 8 * HV_GIC_NUM_IRQS) {
        uint64_t *r = &g->route[(off - 0x6000) / 8];
        if (size == 8) *r = v;
        else if (off & 4) *r = (*r & 0xffffffffull) | v << 32;
        else *r = (*r & ~0xffffffffull) | (uint32_t)v;
        return;
    }
    uint32_t a = (uint32_t)off & ~3u, mask = size_mask(off, size);
    uint32_t word = (uint32_t)(v << ((off & 3) * 8));
    if (a == 0) {
        g->ctlr = (g->ctlr & ~mask) | (word & mask & (GICD_CTLR_EN_GRP0 | GICD_CTLR_EN_GRP1));
    } else if (a >= 0x80 && a < 0x1000) {
        if (!bank0(a)) bank_write(g, a, word, mask, 0, HV_GIC_NUM_IRQS, 0);
    }
    hv_gicv3_update(g);
}

/* One redistributor: RD_base at 0, SGI_base at 0x10000. */
static uint64_t redist_read(void *opaque, uint64_t off, unsigned size) {
    HvGic *g = opaque;
    uint64_t v = 0;
    if (off >= 0x20000) return 0;
    if (off < 0x10000) {
        switch (off & ~7ull) {
        case 0x0000: v = (uint64_t)0x0300043b << 32; break;            /* CTLR 0, IIDR */
        case 0x0008: v = 1u << 4; break;                                /* TYPER: Last, affinity 0 */
        case 0x0010: v = (uint64_t)g->waker << 32; break;               /* STATUSR, WAKER */
        case 0xffe8: v = 0x3b; break;
        default: break;
        }
        v >>= (off & 7) * 8;
    } else {
        uint32_t a = (uint32_t)(off - 0x10000) & ~3u;
        if (a >= 0x80 && a < 0x1000 && bank0(a))
            v = bank_read(g, a, 0, 32) >> ((off & 3) * 8);
    }
    return size == 8 ? v : v & ((1ull << (size * 8)) - 1);
}

static void redist_write(void *opaque, uint64_t off, uint64_t v, unsigned size) {
    HvGic *g = opaque;
    if (off >= 0x20000) return;
    if (off < 0x10000) {
        if ((off & ~3ull) == 0x14) {
            /* WAKER: clearing ProcessorSleep wakes it; ChildrenAsleep follows at once. */
            uint32_t sleep = (v >> (8 * (off & 3))) & 2;
            g->waker = sleep ? 6 : 0;
        }
        return;
    }
    uint32_t a = (uint32_t)(off - 0x10000) & ~3u, mask = size_mask(off, size);
    uint32_t word = (uint32_t)(v << ((off & 3) * 8));
    if (a >= 0x80 && a < 0x1000 && bank0(a)) bank_write(g, a, word, mask, 0, 32, 1);
    hv_gicv3_update(g);
}

HvGic *hv_gicv3_create(HvMachine *m, uint64_t dist_pa, uint64_t redist_pa) {
    HvGic *g = calloc(1, sizeof *g);
    g->m = m;
    g->waker = 6; /* ProcessorSleep and ChildrenAsleep until the guest wakes it */
    for (int i = 0; i < 16; i++) g->edge[i] = 1;
    m->gic = g;
    m->icc_read = icc_read;
    m->icc_write = icc_write;
    hv_machine_add_mmio(m, dist_pa, 0x10000, g, dist_read, dist_write, "gicv3-dist");
    hv_machine_add_mmio(m, redist_pa, 0x400000, g, redist_read, redist_write, "gicv3-redist");
    return g;
}
