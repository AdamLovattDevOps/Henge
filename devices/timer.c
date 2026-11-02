#include "timer.h"

#include "sysreg.h"

static uint64_t one(HvGic *g, int ppi, uint64_t ctl, uint64_t cval, uint64_t now, uint64_t offset) {
    int enabled = (ctl & 1) && !(ctl & 2);
    int fired = enabled && now - offset >= cval;
    hv_gicv3_set_irq(g, ppi, fired);
    /* Deadline in host counter units; nothing to wait for if off or already firing. */
    return enabled && !fired ? cval + offset : 0;
}

uint64_t hv_timer_update(HvGic *g, HvCpu *cpu) {
    uint64_t now = hv_counter();
    uint64_t v = one(g, HV_PPI_VTIMER, cpu->cntv_ctl, cpu->cntv_cval, now, cpu->cntvoff);
    uint64_t p = one(g, HV_PPI_PTIMER, cpu->cntp_ctl, cpu->cntp_cval, now, 0);
    if (!v) return p;
    if (!p) return v;
    return v < p ? v : p;
}
