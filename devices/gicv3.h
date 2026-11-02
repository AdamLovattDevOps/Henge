#ifndef HENGEVM_GICV3_H
#define HENGEVM_GICV3_H

#include <stdint.h>

#include "machine.h"

/* GICv3 with one security state (DS=1), no LPIs or ITS, one CPU interface:
   the distributor, one redistributor, and the ICC_* system registers.
   INTIDs 0-15 are SGIs, 16-31 PPIs, 32 and up SPIs. */
#define HV_GIC_NUM_IRQS (256 + 32) /* vmapple: 256 SPIs */

typedef struct HvGicActive {
    uint16_t intid;
    uint8_t prio, group;
} HvGicActive;

typedef struct HvGic {
    HvMachine *m;
    uint32_t ctlr;                 /* GICD_CTLR */
    uint8_t group[HV_GIC_NUM_IRQS];
    uint8_t grpmod[HV_GIC_NUM_IRQS];
    uint8_t enabled[HV_GIC_NUM_IRQS];
    uint8_t latch[HV_GIC_NUM_IRQS];  /* edge or software pending */
    uint8_t level[HV_GIC_NUM_IRQS];  /* input line */
    uint8_t active[HV_GIC_NUM_IRQS];
    uint8_t priority[HV_GIC_NUM_IRQS];
    uint8_t edge[HV_GIC_NUM_IRQS];   /* ICFGR: 1 = edge-triggered */
    uint64_t route[HV_GIC_NUM_IRQS];
    uint32_t waker;

    /* CPU interface */
    uint64_t pmr, bpr0, bpr1, icc_ctlr, igrpen0, igrpen1, sre;
    uint64_t ap0r, ap1r;           /* active priority bits, one per priority level >> 3 */
    HvGicActive stack[32];       /* acknowledged, not yet priority-dropped */
    int depth;
} HvGic;

HvGic *hv_gicv3_create(HvMachine *m, uint64_t dist_pa, uint64_t redist_pa);
/* Drive an input line: PPIs (16-31) and SPIs (32+). */
void hv_gicv3_set_irq(HvGic *g, int intid, int level);
/* Make an SPI pending, as an MSI write does. */
void hv_gicv3_pend(HvGic *g, int intid);
/* Recompute the CPU's IRQ and FIQ inputs. */
void hv_gicv3_update(HvGic *g);

#endif
