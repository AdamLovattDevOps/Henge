#include "translate.h"

#include <string.h>
#include <stdlib.h>

#include "classify.h"
#include "memop.h"
#include "operands.h"

/* A block is laid out as: entry `ldr x0, [x28]`, the translated instructions,
   out-of-line code (taken-branch exits and memory slow paths), then the literal
   pool. Code is built in a static buffer and copied into the JIT region once. */

#define MAX_WORDS 16384
#define MAX_LITS 2048
#define MAX_OOL 1024
#define WORDS_PER_INSN_MAX 96 /* stop the block before the buffer could overflow */

enum { FIX_LDR, FIX_ADR };

typedef struct {
    uint32_t at, kind, idx;
} LitFix;

enum { OOL_EXIT, OOL_SLOW, OOL_SYS };

typedef struct {
    uint32_t at;        /* the branch that jumps here */
    uint32_t join;      /* slow paths come back here */
    uint64_t pc;        /* exit target, or the guest PC of the slow-path instruction */
    uint32_t insn;
    uint8_t kind;
    uint8_t nrestore;
    uint8_t restore[4]; /* scratch registers to reload from cpu->spill first */
    /* Loads and stores: redo the TLB check exactly (the fast one rejects an
       access in a page's last HV_TLB_ADJ bytes) and go back if it passes. */
    uint8_t recheck, addr, t, u, bytes;
    uint32_t tag_off;
} Ool;

typedef struct {
    uint32_t w[MAX_WORDS];
    uint32_t n;
    uint64_t pool[MAX_LITS];
    uint32_t npool;
    LitFix fix[MAX_LITS];
    uint32_t nfix;
    Ool ool[MAX_OOL];
    uint32_t nool;
    uint32_t flags;
    int slow_mem;
    uint64_t page;       /* guest VA of the 16 KB page the block is in */
    int no_ijc;          /* indirect branches always go to the dispatcher (breakpoints, tests' stop PC) */
    HvMachine *m;
} Emit;

static Emit E; /* translation runs on the dispatcher thread only */

/* ---- encodings ---- */

#define STR_X(t, n, off) (0xf9000000u | ((uint32_t)(off) / 8) << 10 | (n) << 5 | (t))
#define LDR_X(t, n, off) (0xf9400000u | ((uint32_t)(off) / 8) << 10 | (n) << 5 | (t))
#define STP_X(t1, t2, n, off) (0xa9000000u | ((((uint32_t)(off) / 8) & 0x7f) << 15) | (t2) << 10 | (n) << 5 | (t1))
#define LDP_X(t1, t2, n, off) (0xa9400000u | ((((uint32_t)(off) / 8) & 0x7f) << 15) | (t2) << 10 | (n) << 5 | (t1))
#define BR_X(n) (0xd61f0000u | (n) << 5)
#define BLR_X(n) (0xd63f0000u | (n) << 5)
#define SBFM_X(d, n, immr, imms) (0x93400000u | (immr) << 16 | (imms) << 10 | (n) << 5 | (d))
#define UBFM_X(d, n, immr, imms) (0xd3400000u | (immr) << 16 | (imms) << 10 | (n) << 5 | (d))
#define BFM_X(d, n, immr, imms) (0xb3400000u | (immr) << 16 | (imms) << 10 | (n) << 5 | (d))
#define B_OP 0x14000000u
#define MOV_X(d, m) (0xaa0003e0u | (m) << 16 | (d))
#define ADD_LSL(d, n, m, sh) (0x8b000000u | (m) << 16 | (sh) << 10 | (n) << 5 | (d))
#define SUB_X(d, n, m) (0xcb000000u | (m) << 16 | (n) << 5 | (d))
#define ADD_EXT(d, n, m, opt, sh) (0x8b200000u | (m) << 16 | (opt) << 13 | (sh) << 10 | (n) << 5 | (d))
#define CBNZ_X(t) (0xb5000000u | (t))

static void patch_branch(uint32_t *w, int64_t off);

static inline int64_t sext(uint64_t v, int bits) {
    return (int64_t)(v << (64 - bits)) >> (64 - bits);
}

static void put(Emit *e, uint32_t w) {
    if (e->n < MAX_WORDS) e->w[e->n] = w;
    e->n++;
}

/* ldr xR, =value */
static void put_lit(Emit *e, int reg, uint64_t value) {
    e->pool[e->npool] = value;
    e->fix[e->nfix++] = (LitFix){e->n, FIX_LDR, e->npool++};
    put(e, 0x58000000u | reg);
}

/* adr xR, <16-byte HvHelperInfo in the pool> */
static void put_info(Emit *e, int reg, uint64_t pc, uint32_t insn, uint32_t op) {
    e->pool[e->npool] = pc;
    e->pool[e->npool + 1] = insn | (uint64_t)op << 32;
    e->fix[e->nfix++] = (LitFix){e->n, FIX_ADR, e->npool};
    e->npool += 2;
    put(e, 0x10000000u | reg);
}

static void put_ool(Emit *e, Ool o) {
    o.at = e->n - 1; /* the branch just emitted */
    e->ool[e->nool++] = o;
}

/* Guest x0/x1 are saved, x0 = target = cpu->pc. Jumps to the target block
   through the indirect-branch cache for this block's flags if it is there and
   nobody asked the CPU to leave, else leaves for the dispatcher (which fills
   the cache). A hit reloads guest x0 and x1, which the hash used as scratch,
   and enters the block directly: no block has a prologue. Tags are VA ^ salt;
   the salt changes when mappings or code do. */
static void put_ijc_jump(Emit *e) {
    if (e->no_ijc) {
        put(e, LDR_X(1, 28, HV_CPU_EXIT));
        put(e, BR_X(1));
        return;
    }
    put(e, 0xb9400000u | (HV_CPU_EXIT_REQ / 4) << 10 | 28 << 5 | 6); /* ldr w6, [x28, #exit_req] */
    uint32_t to_exit = e->n;
    put(e, 0x35000000u | 6);                                /* cbnz w6, exit (patched below) */
    put(e, LDR_X(6, 28, HV_CPU_IJC));
    uint32_t table = (e->flags & (HV_IJC_TABLES - 1)) * HV_IJC_TABLE_BYTES;
    if (table) put(e, 0x91400000u | (table >> 12) << 10 | 6 << 5 | 6); /* add x6, x6, #table */
    put(e, 0xca400000u | 0 << 16 | 16 << 10 | 0 << 5 | 7);   /* eor x7, x0, x0, lsr #16 */
    put(e, UBFM_X(7, 7, 2, 2 + HV_IJC_BITS - 1));           /* ubfx x7, x7, #2, #bits */
    put(e, ADD_LSL(6, 6, 7, 4));
    put(e, LDP_X(7, 15, 6, 0));                             /* x7 = tag, x15 = entry */
    put(e, LDR_X(6, 28, HV_CPU_IJC_SALT + 8 * (e->flags & HV_BLOCK_EL1)));
    put(e, 0xca000000u | 6 << 16 | 7 << 5 | 7);             /* eor x7, x7, x6 */
    put(e, LDR_X(6, 28, HV_CPU_PC));
    put(e, 0xca000000u | 6 << 16 | 7 << 5 | 7);             /* eor x7, x7, x6 */
    uint32_t to_miss = e->n;
    put(e, CBNZ_X(7));
    put(e, LDR_X(0, 28, 0));                                /* guest x0, x1 */
    put(e, LDR_X(1, 28, 8));
    put(e, BR_X(15));                                       /* hit */
    patch_branch(&e->w[to_miss], (int64_t)e->n - to_miss);
    patch_branch(&e->w[to_exit], (int64_t)e->n - to_exit);
    put(e, LDR_X(1, 28, HV_CPU_EXIT));                      /* miss */
    put(e, BR_X(1));
}

/* Leave the block with pc = target. */
static void put_exit(Emit *e, uint64_t target) {
    put(e, STP_X(0, 1, 28, 0));
    put_lit(e, 0, target);
    put(e, STR_X(0, 28, HV_CPU_PC));
    put(e, LDR_X(1, 28, HV_CPU_EXIT));
    put(e, BR_X(1));
}

/* A direct branch to target. Within the block's own 16 KB page the exit is
   chainable: it jumps to the block whose entry the dispatcher stored in this
   exit's slot (plain data, so chaining never rewrites code), as long as nobody
   has asked the CPU to leave. The hit path touches only the free scratch
   registers, so guest x0/x1 stay live and the target needs no prologue. An
   empty slot leaves with its address in last_stub so the dispatcher can fill
   it. A same-page target always has the same physical mapping as this block,
   so the chain stays correct across page-table changes. Other pages go through
   the indirect-branch cache. */
static void put_branch_exit(Emit *e, uint64_t target) {
    if ((target & ~((1ull << HV_TLB_PAGE_BITS) - 1)) != e->page || e->m->nslots == HV_MAX_SLOTS) {
        /* Another page may be remapped under us, so look it up by VA instead of chaining. */
        put(e, STP_X(0, 1, 28, 0));
        put_lit(e, 0, target);
        put(e, STR_X(0, 28, HV_CPU_PC));
        put_ijc_jump(e);
        return;
    }
    uint64_t *slot = &e->m->slots[e->m->nslots++];
    *slot = 0;
    put(e, 0xb9400000u | (HV_CPU_EXIT_REQ / 4) << 10 | 28 << 5 | 15); /* ldr w15, [x28, #exit_req] */
    put_lit(e, 6, (uint64_t)(uintptr_t)slot);
    uint32_t to_exit = e->n;
    put(e, 0x35000000u | 15);                                         /* cbnz w15, exit (patched below) */
    put(e, LDR_X(15, 6, 0));
    uint32_t to_unchained = e->n;
    put(e, 0xb4000000u | 15);                                         /* cbz x15, unchained (patched below) */
    put(e, BR_X(15));                                                 /* x0/x1 still hold guest values */
    patch_branch(&e->w[to_unchained], (int64_t)e->n - to_unchained);
    put(e, STR_X(6, 28, HV_CPU_LAST_STUB));                           /* x6 still holds the slot address */
    patch_branch(&e->w[to_exit], (int64_t)e->n - to_exit);
    put(e, STP_X(0, 1, 28, 0));
    put_lit(e, 6, target);
    put(e, STR_X(6, 28, HV_CPU_PC));
    put(e, LDR_X(1, 28, HV_CPU_EXIT));
    put(e, BR_X(1));

}

/* Call hv_helper(cpu, info) and continue unless it leaves. */
static void put_helper(Emit *e, uint64_t pc, uint32_t insn, uint32_t op) {
    put(e, STP_X(0, 1, 28, 0));
    put(e, STR_X(30, 28, HV_CPU_X(30)));
    put_info(e, 0, pc, insn, op);
    put(e, LDR_X(1, 28, HV_CPU_HELPER));
    put(e, BLR_X(1));
    put(e, LDR_X(0, 28, 0));
}

/* d = n + imm for |imm| < 2^24; n and d are general registers, never SP. */
static void put_add_imm(Emit *e, int d, int n, int64_t imm) {
    if (imm == 0) {
        if (d != n) put(e, MOV_X(d, n));
        return;
    }
    uint32_t op = imm < 0 ? 0xd1000000u : 0x91000000u;
    uint64_t a = imm < 0 ? (uint64_t)-imm : (uint64_t)imm;
    if (a < 4096) {
        put(e, op | (uint32_t)a << 10 | n << 5 | d);
        return;
    }
    put(e, op | 1u << 22 | (uint32_t)(a >> 12) << 10 | n << 5 | d);
    if (a & 0xfff) put(e, op | (uint32_t)(a & 0xfff) << 10 | d << 5 | d);
}

/* ---- scratch registers ---- */

/* Host registers that can be borrowed: any guest-allocated one, saved in cpu->spill. */
/* Host x6, x7 and x15 hold no guest register (guest x6, x7, x15 live in the
   HvCpu, like x18 and x28: they are the least used in compiled code), so they
   are free scratch: picked first and never spilled. */
static const uint8_t candidates[] = {6, 7, 15, 9, 10, 11, 12, 13, 14, 0, 1, 2, 3, 4, 5, 8,
                                     16, 17, 19, 20, 21, 22, 23, 24, 25, 26, 27, 29, 30};

static int is_free(int reg) {
    return reg == 6 || reg == 7 || reg == 15;
}

static int pick(uint32_t *used) {
    for (unsigned i = 0; i < sizeof candidates; i++) {
        int r = candidates[i];
        if (!(*used & (1u << r))) {
            *used |= 1u << r;
            return r;
        }
    }
    return -1;
}

/* Free scratch registers need no saving; the rest go to cpu->spill in order. */
static int need_spill(const uint8_t *regs, int n, uint8_t *out) {
    int k = 0;
    for (int i = 0; i < n; i++)
        if (!is_free(regs[i])) out[k++] = regs[i];
    return k;
}

static void spill(Emit *e, const uint8_t *regs, int n) {
    uint8_t r[4];
    n = need_spill(regs, n, r);
    int i = 0;
    for (; i + 1 < n; i += 2) put(e, STP_X(r[i], r[i + 1], 28, HV_CPU_SPILL(i)));
    if (i < n) put(e, STR_X(r[i], 28, HV_CPU_SPILL(i)));
}

static void unspill(Emit *e, const uint8_t *regs, int n) {
    uint8_t r[4];
    n = need_spill(regs, n, r);
    int i = 0;
    for (; i + 1 < n; i += 2) put(e, LDP_X(r[i], r[i + 1], 28, HV_CPU_SPILL(i)));
    if (i < n) put(e, LDR_X(r[i], 28, HV_CPU_SPILL(i)));
}

/* Where a register the translator does not keep in a host register lives. */
static int is_stolen(int reg) {
    return reg == 18 || reg == 28 || is_free(reg);
}

static uint32_t slot_of(int reg) {
    return reg == 31 ? HV_CPU_SP : HV_CPU_X(reg);
}

/* ---- instruction groups ---- */

static void translate_copy(Emit *e, uint32_t insn) {
    uint32_t fields = hv_gpr_fields(insn);
    if (!fields) {
        put(e, insn);
        return;
    }
    static const uint32_t sp_flag[4] = {HV_FIELD_RD_SP, HV_FIELD_RN_SP, 0, 0};
    int stolen[4] = {-1, -1, -1, -1}; /* per field: the guest register to rename, or -1 */
    uint32_t used = 0;
    int any = 0;
    for (int f = 0; f < 4; f++) {
        if (!(fields & (1u << f))) continue;
        int r = hv_field_reg(insn, f);
        used |= 1u << r;
        if (is_stolen(r) || (r == 31 && (fields & sp_flag[f]))) {
            stolen[f] = r;
            any = 1;
        }
    }
    if (!any) {
        put(e, insn);
        return;
    }
    /* One scratch per distinct stolen register; x6, x7 and x15 use their own host register. */
    int guest_of[4] = {-1, -1, -1, -1}, scratch_of[4];
    uint8_t regs[4];
    int n = 0;
    for (int f = 0; f < 4; f++) {
        if (stolen[f] < 0) continue;
        int k = 0;
        while (k < n && guest_of[k] != stolen[f]) k++;
        if (k == n) {
            guest_of[n] = stolen[f];
            scratch_of[n] = is_free(stolen[f]) ? stolen[f] : pick(&used);
            regs[n] = scratch_of[n];
            n++;
        }
    }
    spill(e, regs, n);
    for (int k = 0; k < n; k++) put(e, LDR_X(scratch_of[k], 28, slot_of(guest_of[k])));
    uint32_t out = insn;
    for (int f = 0; f < 4; f++) {
        if (stolen[f] < 0) continue;
        int k = 0;
        while (guest_of[k] != stolen[f]) k++;
        out = hv_field_set(out, f, scratch_of[k]);
    }
    put(e, out);
    if (stolen[0] >= 0) { /* Rd was renamed: every copied form with an Rd field writes it */
        int k = 0;
        while (guest_of[k] != stolen[0]) k++;
        put(e, STR_X(scratch_of[k], 28, slot_of(stolen[0])));
    }
    unspill(e, regs, n);
}

/* Write a translation-time constant to guest register rd (31 is XZR here). */
static void put_const_to(Emit *e, int rd, uint64_t value) {
    if (rd == 31) return;
    if (!is_stolen(rd)) {
        put_lit(e, rd, value);
        return;
    }
    uint8_t s = 15; /* free scratch: the spill below is empty */
    spill(e, &s, 1);
    put_lit(e, s, value);
    put(e, STR_X(s, 28, slot_of(rd)));
    unspill(e, &s, 1);
}

static void translate_pcrel(Emit *e, uint32_t insn, uint64_t pc) {
    int64_t imm = sext(((insn >> 5) & 0x7ffff) << 2 | ((insn >> 29) & 3), 21);
    uint64_t value = (insn >> 31) ? (pc & ~0xfffull) + ((uint64_t)imm << 12) : pc + imm;
    put_const_to(e, insn & 0x1f, value);
}

/* Bits 54..48 take bit 55's value. Guest pointers are never signed, so for a
   canonical pointer this changes nothing; it only clears a stray PAC field. */
static void put_strip(Emit *e, int r) {
    if (r == 31) return;
    uint32_t used = 1u << r;
    if (!is_stolen(r)) {
        uint8_t t = pick(&used);
        spill(e, &t, 1);
        put(e, SBFM_X(t, r, 55, 55));
        put(e, BFM_X(r, t, 16, 6));
        unspill(e, &t, 1);
        return;
    }
    uint8_t regs[2];
    regs[0] = pick(&used);
    regs[1] = pick(&used);
    spill(e, regs, 2);
    put(e, LDR_X(regs[0], 28, slot_of(r)));
    put(e, SBFM_X(regs[1], regs[0], 55, 55));
    put(e, BFM_X(regs[0], regs[1], 16, 6));
    put(e, STR_X(regs[0], 28, slot_of(r)));
    unspill(e, regs, 2);
}

static void translate_pac(Emit *e, uint32_t insn) {
    if ((insn & 0xfffff01f) == 0xd503201f) { /* hint space */
        uint32_t imm = ((insn >> 8) & 0xf) << 3 | ((insn >> 5) & 7);
        if (imm == 7 || imm >= 28) put_strip(e, 30);       /* XPACLRI, AUTIASP and friends */
        else if (imm == 12 || imm == 14) put_strip(e, 17); /* AUTIA1716, AUTIB1716 */
        return;                                            /* PAC*: no-op */
    }
    int rd = insn & 0x1f;
    if ((insn & 0xffe0fc00) == 0x9ac03000) { /* PACGA: a fixed code keeps sign/verify pairs consistent */
        if (rd == 31) return;
        if (is_stolen(rd)) put(e, STR_X(31, 28, slot_of(rd)));
        else put(e, 0xd2800000u | rd); /* movz xd, #0 */
        return;
    }
    uint32_t opcode = (insn >> 10) & 0x3f;
    if (opcode >= 16 || (opcode & 4)) put_strip(e, rd); /* XPAC*, AUT* */
}

/* Ends the block. */
static void translate_branch(Emit *e, uint32_t insn, uint64_t pc) {
    if ((insn & 0x7c000000) == 0x14000000) { /* B, BL */
        uint64_t target = pc + (sext(insn & 0x3ffffff, 26) << 2);
        if (insn >> 31) put_const_to(e, 30, pc + 4);
        put_branch_exit(e, target);
        return;
    }
    if ((insn & 0xff000000) == 0x54000000) { /* B.cond, BC.cond */
        uint64_t target = pc + (sext((insn >> 5) & 0x7ffff, 19) << 2);
        uint32_t cond = insn & 0xf;
        if (cond >= 14) {
            put_branch_exit(e, target);
            return;
        }
        put(e, 0x54000000u | cond);
        put_ool(e, (Ool){.kind = OOL_EXIT, .pc = target});
        put_branch_exit(e, pc + 4);
        return;
    }
    /* CBZ/CBNZ (imm19) and TBZ/TBNZ (imm14) */
    int tb = (insn & 0x7e000000) == 0x36000000;
    uint64_t target = pc + (tb ? sext((insn >> 5) & 0x3fff, 14) << 2 : sext((insn >> 5) & 0x7ffff, 19) << 2);
    uint32_t bare = tb ? insn & ~(0x3fffu << 5) : insn & ~(0x7ffffu << 5);
    int rt = insn & 0x1f;
    if (!is_stolen(rt)) {
        put(e, bare);
        put_ool(e, (Ool){.kind = OOL_EXIT, .pc = target});
        put_branch_exit(e, pc + 4);
        return;
    }
    uint8_t s = 15; /* free scratch: the spill below is empty */
    spill(e, &s, 1);
    put(e, LDR_X(s, 28, slot_of(rt)));
    put(e, (bare & ~0x1fu) | s);
    put_ool(e, (Ool){.kind = OOL_EXIT, .pc = target, .nrestore = 1, .restore = {s}});
    unspill(e, &s, 1);
    put_branch_exit(e, pc + 4);
}

/* BR, BLR, RET and their PAC forms. Ends the block. The target is looked up
   inline in the indirect-branch cache for this block's flags: a hit jumps
   straight to the target block's first entry (which reloads guest x1 and x0);
   a miss leaves for the dispatcher, which fills the cache. Tags are VA ^ salt,
   and the salt changes whenever mappings or code change. */
static void translate_branch_reg(Emit *e, uint32_t insn, uint64_t pc) {
    uint32_t opc = (insn >> 21) & 3;
    int rn = (insn >> 5) & 0x1f;
    if (opc == 2 && ((insn >> 11) & 0x1f) == 1) rn = 30; /* RETAA, RETAB: the Rn field is all ones, the target is x30 */
    put(e, STP_X(0, 1, 28, 0));
    if (is_stolen(rn)) put(e, LDR_X(0, 28, slot_of(rn)));
    else if (rn == 31) put(e, 0xd2800000u);                 /* movz x0, #0 */
    else if (rn != 0) put(e, MOV_X(0, rn));
    put(e, STR_X(0, 28, HV_CPU_PC));
    if (opc == 1) put_lit(e, 30, pc + 4);                   /* BLR: after reading Rn */
    put_ijc_jump(e);
}

/* Loads and stores. A, T and U are borrowed host registers: A holds the guest
   address and then the host address, T and U index the TLB and then stand in
   for x18/x28 operands. A miss goes out of line to hv_mem_emulate, which does
   the whole instruction (writeback included) and comes back to the join. */
static void translate_mem(Emit *e, uint32_t insn, uint64_t pc) {
    HvMemOp op;
    if (hv_mem_decode(insn, &op) != 0) {
        put_helper(e, pc, insn, HV_OP_UNDEF);
        return;
    }
    if (e->slow_mem || op.kind == HV_MEM_EXCL || op.kind == HV_MEM_CASP) {
        put_helper(e, pc, insn, HV_OP_MEM);
        return;
    }
    int literal = op.mode == HV_ADDR_LITERAL;
    int vec_kind = op.kind == HV_MEM_VEC_MULTI || op.kind == HV_MEM_VEC_SINGLE;
    int pair = op.kind == HV_MEM_PAIR;
    int rmw = op.kind == HV_MEM_CAS || op.kind == HV_MEM_ATOMIC;
    int rm_used = op.mode == HV_ADDR_REG || (vec_kind && op.mode == HV_ADDR_POST && op.rm != 31);

    uint32_t used = 0;
    if (!literal) used |= 1u << op.rn;
    if (!op.vec) used |= 1u << op.rt | (pair ? 1u << op.rt2 : 0);
    if (rm_used) used |= 1u << op.rm;
    if (rmw) used |= 1u << op.rs;
    uint8_t s[3];
    for (int i = 0; i < 3; i++) s[i] = pick(&used);
    int A = s[0], T = s[1], U = s[2];
    spill(e, s, 3);

    /* Effective address into A, or used where it is: a guest base register with nothing added. */
    int base_stolen = !literal && (op.rn == 31 || is_stolen(op.rn));
    int direct = !literal && !base_stolen &&
                 (op.mode == HV_ADDR_POST || ((op.mode == HV_ADDR_OFFSET || op.mode == HV_ADDR_PRE) && op.imm == 0));
    int addr = direct ? op.rn : A;
    int base = op.rn;
    if (literal) {
        put_lit(e, A, pc + op.imm);
    } else {
        if (base_stolen) {
            put(e, LDR_X(A, 28, slot_of(op.rn)));
            base = A;
        }
        switch (op.mode) {
        case HV_ADDR_OFFSET:
        case HV_ADDR_PRE:
            if (!direct) put_add_imm(e, A, base, op.imm);
            break;
        case HV_ADDR_REG: {
            int rm = op.rm;
            if (is_stolen(rm)) {
                put(e, LDR_X(T, 28, slot_of(rm)));
                rm = T;
            }
            put(e, ADD_EXT(A, base, rm, op.ext, op.shift));
            break;
        }
        default:
            if (!direct && base != A) put(e, MOV_X(A, base));
            break;
        }
    }

    /* TLB: index from the first byte; the tag check (see HvTlbEntry) also
       makes an access that runs into the next page miss. Uses no flags. */
    int el_idx = op.unpriv || !(e->flags & HV_BLOCK_EL1) ? 0 : (e->flags & HV_BLOCK_PAN) ? 2 : 1;
    uint32_t tlb = HV_CPU_TLB;
    put(e, UBFM_X(T, addr, HV_TLB_PAGE_BITS, HV_TLB_PAGE_BITS + HV_TLB_BITS - 1));
    put(e, ADD_LSL(T, 28, T, 5));
    /* EL1's table lies beyond a load's immediate range: step over EL0's. */
    if (el_idx) put(e, 0x91400000u | (uint32_t)((el_idx * HV_CPU_TLB_EL) >> 12) << 10 | T << 5 | T); /* add T, T, #table */
    put(e, LDR_X(U, T, tlb + (op.store ? 8 : 0)));
    put(e, SUB_X(U, addr, U));
    if (op.align && op.bytes > 1) {
        /* and U, U, #~(0x3fff & ~(bytes-1)) after taking the adjustment back off:
           zero only if same page and aligned */
        uint32_t k = __builtin_ctz(op.bytes);
        put(e, 0xd1000000u | HV_TLB_ADJ << 10 | U << 5 | U); /* sub U, U, #ADJ */
        put(e, 0x92400000u | 50u << 16 | (49u + k) << 10 | U << 5 | U);
    } else {
        if (op.bytes > HV_TLB_ADJ + 1) put_add_imm(e, U, U, op.bytes - 1 - HV_TLB_ADJ);
        put(e, UBFM_X(U, U, HV_TLB_PAGE_BITS, 63));
    }
    put(e, CBNZ_X(U));
    put_ool(e, (Ool){.kind = OOL_SLOW, .pc = pc, .insn = insn, .nrestore = 3, .restore = {A, T, U},
                     .recheck = !(op.align && op.bytes > 1) && op.bytes <= HV_TLB_ADJ + 1, .addr = (uint8_t)addr,
                     .t = (uint8_t)T, .u = (uint8_t)U, .bytes = (uint8_t)op.bytes,
                     .tag_off = tlb + (op.store ? 8 : 0)});
    uint32_t slow = e->nool - 1;
    put(e, LDR_X(U, T, tlb + 24));
    /* Single loads and stores take [addr, addend] directly; the rest want the host address in A. */
    int reg_offset = op.kind == HV_MEM_SINGLE || op.kind == HV_MEM_LITERAL || op.kind == HV_MEM_LDRA;
    if (!reg_offset) put(e, ADD_LSL(A, addr, U, 0));

    /* Stand-ins for x18/x28 operands: Rt (or Rs for RMW) in T, the other in U. */
    int rt = op.rt, rt2 = op.rt2, rs = op.rs;
    int rt_stolen = !op.vec && is_stolen(op.rt);
    int rt2_stolen = pair && !op.vec && is_stolen(op.rt2);
    int rs_stolen = rmw && is_stolen(op.rs);
    if (rmw) {
        if (rs_stolen) rs = T;
        if (rt_stolen) rt = (op.rt == op.rs) ? T : U;
    } else {
        if (rt_stolen) rt = T;
        if (rt2_stolen) rt2 = (op.rt2 == op.rt) ? T : U;
    }
    if (rs_stolen) put(e, LDR_X(rs, 28, slot_of(op.rs)));
    /* Only RMW forms have an Rs; elsewhere bits 20..16 are immediate bits. */
    if (rt_stolen && !(rmw && rs_stolen && rt == rs)) put(e, LDR_X(rt, 28, slot_of(op.rt)));
    if (rt2_stolen && rt2 != rt) put(e, LDR_X(rt2, 28, slot_of(op.rt2)));

    uint32_t out;
    switch (op.kind) {
    case HV_MEM_PAIR: {
        uint32_t opc = op.vec ? op.size - 2u : (op.sign ? 1u : op.size == 3 ? 2u : 0u);
        out = opc << 30 | 0x29000000u | (uint32_t)op.vec << 26 | (uint32_t)op.load << 22 | rt2 << 10 | A << 5 | rt;
        break;
    }
    case HV_MEM_SINGLE:
    case HV_MEM_LITERAL:
    case HV_MEM_LDRA: {
        uint32_t size, opc;
        if (op.vec) {
            size = op.size == 4 ? 0 : op.size;
            opc = (op.size == 4 ? 2u : 0u) | op.load;
        } else {
            size = op.size;
            opc = op.store ? 0 : op.sign == 64 ? 2 : op.sign == 32 ? 3 : 1;
        }
        /* LDR/STR (register), LSL #0: [addr, U] */
        out = size << 30 | 0x38206800u | (uint32_t)op.vec << 26 | opc << 22 | (uint32_t)U << 16 | (uint32_t)addr << 5 | rt;
        break;
    }
    case HV_MEM_VEC_MULTI:
    case HV_MEM_VEC_SINGLE:
        out = (insn & ~(1u << 23) & ~(0x1fu << 16) & ~(0x1fu << 5)) | A << 5;
        break;
    default: /* ORDERED, CAS, ATOMIC: same instruction on the host address */
        out = (insn & ~(0x1fu << 5) & ~0x1fu) | A << 5 | rt;
        if ((insn & 0x3f200c00) == 0x19000000) out &= ~(0x1ffu << 12); /* LDAPUR/STLUR: drop imm9 */
        if (rmw) out = (out & ~(0x1fu << 16)) | rs << 16;
        break;
    }
    put(e, out);

    if (rmw) {
        if (rs_stolen && op.kind == HV_MEM_CAS) put(e, STR_X(rs, 28, slot_of(op.rs)));
        if (rt_stolen && op.kind == HV_MEM_ATOMIC) put(e, STR_X(rt, 28, slot_of(op.rt)));
    } else if (op.load) {
        if (rt_stolen) put(e, STR_X(rt, 28, slot_of(op.rt)));
        if (rt2_stolen) put(e, STR_X(rt2, 28, slot_of(op.rt2)));
    }

    /* Base writeback. */
    if (op.mode == HV_ADDR_PRE || op.mode == HV_ADDR_POST) {
        int b = op.rn;
        if (base_stolen) {
            put(e, LDR_X(T, 28, slot_of(op.rn)));
            b = T;
        }
        if (vec_kind && op.rm != 31) {
            int rm = op.rm;
            if (is_stolen(rm)) {
                put(e, LDR_X(U, 28, slot_of(rm)));
                rm = U;
            }
            put(e, ADD_LSL(b, b, rm, 0));
        } else {
            put_add_imm(e, b, b, op.imm);
        }
        if (base_stolen) put(e, STR_X(T, 28, slot_of(op.rn)));
    }
    unspill(e, s, 3);
    e->ool[slow].join = e->n;
}

/* DC ZVA: zero the 64-byte block holding Xt, on the host's own copy when the
   TLB maps it writable (xnu's bzero does this a million times a second).
   The host's DC ZVA block is 64 bytes too (checked at translation time);
   a miss, a code page or a device takes the SYS helper. */
static int host_zva_is_64(void) {
    static int known = -1;
    if (known < 0) {
        uint64_t dczid;
        __asm__ volatile("mrs %0, dczid_el0" : "=r"(dczid));
        known = (dczid & 0x1f) == 4;
    }
    return known;
}

static void translate_dc_zva(Emit *e, uint32_t insn, uint64_t pc) {
    int rt = insn & 0x1f;
    uint32_t used = 1u << rt;
    uint8_t s[3];
    for (int i = 0; i < 3; i++) s[i] = pick(&used);
    int A = s[0], T = s[1], U = s[2];
    spill(e, s, 3);
    if (is_stolen(rt)) put(e, LDR_X(A, 28, slot_of(rt)));
    else put(e, MOV_X(A, rt)); /* rt 31 is XZR here */
    put(e, 0x92400000u | 58u << 16 | 57u << 10 | A << 5 | A); /* and A, A, #~63 */
    int el_idx = !(e->flags & HV_BLOCK_EL1) ? 0 : (e->flags & HV_BLOCK_PAN) ? 2 : 1;
    put(e, UBFM_X(T, A, HV_TLB_PAGE_BITS, HV_TLB_PAGE_BITS + HV_TLB_BITS - 1));
    put(e, ADD_LSL(T, 28, T, 5));
    if (el_idx) put(e, 0x91400000u | (uint32_t)((el_idx * HV_CPU_TLB_EL) >> 12) << 10 | T << 5 | T);
    put(e, LDR_X(U, T, HV_CPU_TLB + 8)); /* write tag */
    put(e, SUB_X(U, A, U));
    put(e, UBFM_X(U, U, HV_TLB_PAGE_BITS, 63));
    put(e, CBNZ_X(U));
    put_ool(e, (Ool){.kind = OOL_SYS, .pc = pc, .insn = insn, .nrestore = 3, .restore = {A, T, U}});
    uint32_t slow = e->nool - 1;
    put(e, LDR_X(U, T, HV_CPU_TLB + 24));
    put(e, ADD_LSL(A, A, U, 0));
    put(e, 0xd50b7420u | A); /* dc zva, A */
    unspill(e, s, 3);
    e->ool[slow].join = e->n;
}

/* System registers that are plain storage, or host registers that already
   hold the guest's value: MRS/MSR to these need no helper and do not end the
   block. Anything with side effects (DAIF, SPSel writes, the MMU, timers, GIC)
   still goes through the helper. Returns 1 if handled. */
#define SR(op0, op1, crn, crm, op2) ((op0) << 14 | (op1) << 11 | (crn) << 7 | (crm) << 3 | (op2))

static void put_load_field(Emit *e, int rt, uint32_t off) {
    if (rt == 31) return;
    if (!is_stolen(rt)) {
        put(e, LDR_X(rt, 28, off));
        return;
    }
    uint8_t s = 15; /* free scratch: the spill below is empty */
    spill(e, &s, 1);
    put(e, LDR_X(s, 28, off));
    put(e, STR_X(s, 28, slot_of(rt)));
    unspill(e, &s, 1);
}

static void put_store_field(Emit *e, int rt, uint32_t off) {
    if (!is_stolen(rt)) {
        put(e, STR_X(rt, 28, off)); /* 31 stores XZR */
        return;
    }
    uint8_t s = 15; /* free scratch: the spill below is empty */
    spill(e, &s, 1);
    put(e, LDR_X(s, 28, slot_of(rt)));
    put(e, STR_X(s, 28, off));
    unspill(e, &s, 1);
}

/* MRS/MSR of NZCV, FPCR, FPSR: the host registers are the guest's. */
static void put_native_sysreg(Emit *e, uint32_t insn, int read) {
    int rt = insn & 0x1f;
    if (!is_stolen(rt)) {
        put(e, insn);
        return;
    }
    uint8_t s = 15; /* free scratch: the spill below is empty */
    spill(e, &s, 1);
    if (read) {
        put(e, (insn & ~0x1fu) | s);
        put(e, STR_X(s, 28, slot_of(rt)));
    } else {
        put(e, LDR_X(s, 28, slot_of(rt)));
        put(e, (insn & ~0x1fu) | s);
    }
    unspill(e, &s, 1);
}

/* MRS/MSR of a PSTATE bit kept in cpu->pstate_extra at the same position as in the register. */
static void put_pstate_bit(Emit *e, int rt, int bit, int read) {
    uint32_t used = 1u << rt;
    uint8_t s[3];
    for (int i = 0; i < 3; i++) s[i] = pick(&used);
    int t = s[0], u = s[1], v = s[2];
    spill(e, s, 3);
    put(e, LDR_X(t, 28, HV_CPU_PSTATE_EXTRA));
    if (read) {
        put(e, UBFM_X(t, t, bit, bit));                       /* ubfx t, t, #bit, #1 */
        put(e, UBFM_X(t, t, (64 - bit) & 63, 63 - bit));      /* lsl t, t, #bit */
        if (rt != 31) put(e, is_stolen(rt) ? STR_X(t, 28, slot_of(rt)) : MOV_X(rt, t));
    } else {
        int r = rt;
        if (rt == 31) {
            put(e, 0xd2800000u | v);                          /* movz v, #0 */
            r = v;
        } else if (is_stolen(rt)) {
            put(e, LDR_X(v, 28, slot_of(rt)));
            r = v;
        }
        put(e, 0xca000000u | (uint32_t)r << 16 | (uint32_t)t << 5 | (uint32_t)u); /* eor u, t, r */
        put(e, UBFM_X(u, u, bit, bit));                       /* the bit that differs */
        put(e, 0xca000000u | (uint32_t)u << 16 | (uint32_t)bit << 10 | (uint32_t)t << 5 | (uint32_t)t); /* eor t, t, u, lsl #bit */
        put(e, STR_X(t, 28, HV_CPU_PSTATE_EXTRA));
    }
    unspill(e, s, 3);
}

static int translate_sysreg_inline(Emit *e, uint32_t insn) {
    if (((insn >> 19) & 3) == 0) return 0; /* MSR (immediate) */
    int read = (insn >> 21) & 1, rt = insn & 0x1f;
    uint32_t reg = (insn >> 5) & 0xffff;
    int el1 = (e->flags & HV_BLOCK_EL1) != 0, spx = (e->flags & HV_BLOCK_SPX) != 0;
    int fp_ok = !(e->flags & HV_BLOCK_FPOFF);
    switch (reg) {
    case SR(3, 3, 4, 2, 0): /* NZCV */
        put_native_sysreg(e, insn, read);
        return 1;
    case SR(3, 3, 4, 4, 0): /* FPCR */
    case SR(3, 3, 4, 4, 1): /* FPSR */
        if (!fp_ok) return 0; /* traps: leave it to the helper */
        put_native_sysreg(e, insn, read);
        return 1;
    case SR(3, 3, 13, 0, 2): /* TPIDR_EL0 */
        if (read) put_load_field(e, rt, HV_CPU_TPIDR_EL0);
        else put_store_field(e, rt, HV_CPU_TPIDR_EL0);
        return 1;
    case SR(3, 3, 13, 0, 3): /* TPIDRRO_EL0: EL0 may only read it */
        if (read) put_load_field(e, rt, HV_CPU_TPIDRRO_EL0);
        else if (el1) put_store_field(e, rt, HV_CPU_TPIDRRO_EL0);
        else return 0;
        return 1;
    case SR(3, 0, 13, 0, 4): /* TPIDR_EL1 */
    case SR(3, 0, 4, 0, 1):  /* ELR_EL1 */
    case SR(3, 0, 4, 0, 0): { /* SPSR_EL1 */
        if (!el1) return 0;
        uint32_t off = reg == SR(3, 0, 13, 0, 4) ? HV_CPU_TPIDR_EL1 : reg == SR(3, 0, 4, 0, 1) ? HV_CPU_ELR_EL1 : HV_CPU_SPSR_EL1;
        if (read) put_load_field(e, rt, off);
        else put_store_field(e, rt, off);
        return 1;
    }
    case SR(3, 0, 1, 0, 0): /* SCTLR_EL1, ESR_EL1, FAR_EL1: reads (writes have side effects or are rare) */
    case SR(3, 0, 5, 2, 0):
    case SR(3, 0, 6, 0, 0): {
        if (!read || !el1) return 0;
        uint32_t off = reg == SR(3, 0, 1, 0, 0) ? HV_CPU_SCTLR_EL1 : reg == SR(3, 0, 5, 2, 0) ? HV_CPU_ESR_EL1 : HV_CPU_FAR_EL1;
        put_load_field(e, rt, off);
        return 1;
    }
    case SR(3, 0, 4, 1, 0): /* SP_EL0: only while it is banked (SPSel = 1) */
        if (!el1 || !spx) return 0;
        if (read) put_load_field(e, rt, HV_CPU_SP_EL0);
        else put_store_field(e, rt, HV_CPU_SP_EL0);
        return 1;
    case SR(3, 3, 4, 2, 1): /* DAIF: reads only; writes can unmask interrupts */
        if (!read || !el1) return 0;
        put_load_field(e, rt, HV_CPU_DAIF);
        return 1;
    case SR(3, 0, 4, 2, 0): /* SPSel */
        if (!read || !el1) return 0;
        put_const_to(e, rt, (uint64_t)spx);
        return 1;
    case SR(3, 0, 4, 2, 2): /* CurrentEL */
        if (!read || !el1) return 0;
        put_const_to(e, rt, 4);
        return 1;
    case SR(3, 3, 4, 2, 5): /* DIT: one bit of PSTATE, nothing depends on it here */
    case SR(3, 3, 4, 2, 6): /* SSBS */
        put_pstate_bit(e, rt, reg == SR(3, 3, 4, 2, 5) ? 24 : 12, read);
        return 1;
    default:
        return 0;
    }
}

static int is_fp_insn(uint32_t insn) {
    uint32_t op0 = (insn >> 25) & 0xf;
    if ((op0 & 0x7) == 0x7) return 1;                        /* SIMD and FP data processing */
    if ((op0 & 0x5) == 0x4 && ((insn >> 26) & 1)) return 1;  /* SIMD and FP loads and stores */
    return 0;
}

/* Returns 1 if the instruction ends the block. */
static int translate_one(Emit *e, uint32_t insn, uint64_t pc) {
    HvInsnKind kind = hv_classify(insn);
    if ((e->flags & HV_BLOCK_FPOFF) && kind != HV_INSN_UNDEFINED && is_fp_insn(insn)) {
        put_helper(e, pc, insn, HV_OP_FPTRAP);
        return 1;
    }
    switch (kind) {
    case HV_INSN_COPY:
        translate_copy(e, insn);
        return 0;
    case HV_INSN_PCREL:
        translate_pcrel(e, insn, pc);
        return 0;
    case HV_INSN_BRANCH:
        translate_branch(e, insn, pc);
        return 1;
    case HV_INSN_BRANCH_REG:
        translate_branch_reg(e, insn, pc);
        return 1;
    case HV_INSN_PAC:
        translate_pac(e, insn);
        return 0;
    case HV_INSN_PREFETCH:
        return 0;
    case HV_INSN_LOAD_LITERAL:
    case HV_INSN_MEMORY:
        translate_mem(e, insn, pc);
        return 0;
    case HV_INSN_SYSREG:
        if (translate_sysreg_inline(e, insn)) return 0;
        if (((insn >> 21) & 1) && ((insn >> 19) & 3)) { /* MRS */
            put_helper(e, pc, insn, HV_OP_MRS);
            return 0;
        }
        put_helper(e, pc, insn, HV_OP_MSR); /* state blocks depend on may change */
        if (((insn >> 19) & 3) == 0 && (e->flags & HV_BLOCK_EL1)) {
            /* MSR SPSel/PAN, #imm at EL1: the flags of the next block are known, so
               go to it through that flags' indirect-branch cache instead of the dispatcher. */
            uint32_t op1 = (insn >> 16) & 7, op2 = (insn >> 5) & 7, imm = (insn >> 8) & 1;
            uint32_t bit = op1 == 0 && op2 == 5 ? HV_BLOCK_SPX : op1 == 0 && op2 == 4 ? HV_BLOCK_PAN : 0;
            if (bit) {
                uint32_t flags = e->flags;
                put(e, STP_X(0, 1, 28, 0));
                put_lit(e, 0, pc + 4);
                put(e, STR_X(0, 28, HV_CPU_PC));
                e->flags = imm ? flags | bit : flags & ~bit;
                put_ijc_jump(e);
                e->flags = flags;
                return 1;
            }
        }
        put_exit(e, pc + 4);
        return 1;
    case HV_INSN_SYS: {
        uint32_t crn = (insn >> 12) & 0xf, crm = (insn >> 8) & 0xf, op1 = (insn >> 16) & 7, op2 = (insn >> 5) & 7;
        /* Data-cache maintenance by VA or set/way: memory here is coherent, so
           there is nothing to do. DC ZVA writes memory and AT translates, so
           those still go to the helper. */
        int dc_zva = op1 == 3 && crm == 4 && op2 == 1;
        int at = crm == 8 || crm == 9;
        if (crn == 7 && crm != 1 && crm != 5 && !dc_zva && !at) return 0;
        if (dc_zva && !e->slow_mem && host_zva_is_64()) {
            translate_dc_zva(e, insn, pc);
            return 0;
        }
        put_helper(e, pc, insn, HV_OP_SYS);
        /* DC and AT continue, and so does IC IVAU: invalidating code sets exit_req,
           so the block leaves at its next exit if it mattered. IC IALLU and TLBI leave. */
        if (crn == 7 && ((crm != 1 && crm != 5) || (op1 == 3 && crm == 5 && op2 == 1))) return 0;
        put_exit(e, pc + 4);
        return 1;
    }
    case HV_INSN_BARRIER: {
        uint32_t op2 = (insn >> 5) & 7;
        if (op2 == 2) {
            put_helper(e, pc, insn, HV_OP_CLREX);
            return 0;
        }
        if (op2 == 6) { /* ISB: later instructions must see earlier system register writes */
            put_exit(e, pc + 4);
            return 1;
        }
        put(e, insn); /* DSB, DMB, SB */
        return 0;
    }
    case HV_INSN_WAIT:
        if (((insn >> 5) & 7) == 3 || ((insn >> 12) & 0xf) == 1) put_helper(e, pc, insn, HV_OP_WFI); /* WFI, WFIT, WFET */
        put_exit(e, pc + 4); /* WFE just yields to the dispatcher */
        return 1;
    case HV_INSN_EXCEPTION:
        put_helper(e, pc, insn, HV_OP_EXCEPTION);
        put_exit(e, pc + 4);
        return 1;
    case HV_INSN_ERET:
        put_helper(e, pc, insn, HV_OP_ERET);
        put_exit(e, pc + 4);
        return 1;
    case HV_INSN_UNDEFINED:
        put_helper(e, pc, insn, HV_OP_UNDEF);
        put_exit(e, pc + 4);
        return 1;
    }
    return 1;
}

static void patch_branch(uint32_t *w, int64_t off) {
    uint32_t op = *w;
    if ((op & 0x7c000000) == B_OP) *w = (op & 0xfc000000) | (off & 0x3ffffff);
    else if ((op & 0x7e000000) == 0x36000000) *w = (op & ~(0x3fffu << 5)) | (uint32_t)(off & 0x3fff) << 5;
    else *w = (op & ~(0x7ffffu << 5)) | (uint32_t)(off & 0x7ffff) << 5; /* B.cond, CBZ, LDR literal */
}

static void emit_ool(Emit *e) {
    for (uint32_t i = 0; i < e->nool; i++) {
        Ool *o = &e->ool[i];
        patch_branch(&e->w[o->at], (int64_t)e->n - o->at);
        if (o->recheck) {
            put(e, LDR_X(o->u, o->t, o->tag_off));
            put(e, SUB_X(o->u, o->addr, o->u));
            put_add_imm(e, o->u, o->u, (int64_t)o->bytes - 1 - HV_TLB_ADJ);
            put(e, UBFM_X(o->u, o->u, HV_TLB_PAGE_BITS, 63));
            put(e, CBNZ_X(o->u) | 2u << 5); /* cbnz u, +2: a real miss */
            put(e, B_OP);
            patch_branch(&e->w[e->n - 1], (int64_t)(o->at + 1) - (e->n - 1));
        }
        unspill(e, o->restore, o->nrestore);
        if (o->kind == OOL_EXIT) {
            put_branch_exit(e, o->pc); /* only branches use OOL exits */
        } else {
            put_helper(e, o->pc, o->insn, o->kind == OOL_SYS ? HV_OP_SYS : HV_OP_MEM);
            put(e, B_OP);
            patch_branch(&e->w[e->n - 1], (int64_t)o->join - (e->n - 1));
        }
    }
}

int hv_translate_block(HvMachine *m, HvBlock *b, const uint32_t *code, uint32_t avail) {
    Emit *e = &E;
    e->n = e->npool = e->nfix = e->nool = 0;
    e->flags = b->flags;
    e->slow_mem = m->slow_mem;
    e->page = b->va & ~((1ull << HV_TLB_PAGE_BITS) - 1);
    e->no_ijc = !m->chain;
    e->m = m;

    /* No prologue: hv_enter, chained exits and the indirect-branch cache all
       enter with guest x0 and x1 live. */
    uint32_t max = avail < HV_MAX_BLOCK_INSNS ? avail : HV_MAX_BLOCK_INSNS;
    uint32_t i = 0;
    int ended = 0;
    while (i < max && !ended) {
        uint64_t pc = b->va + 4ull * i;
        if (i > 0 && pc == m->stop_pc) break; /* so the dispatcher sees stop_pc at a block start */
        ended = translate_one(e, code[i], pc);
        i++;
        if (e->n + WORDS_PER_INSN_MAX * 2 > MAX_WORDS || e->npool + 8 > MAX_LITS || e->nool + 4 > MAX_OOL) break;
    }
    if (!ended) put_branch_exit(e, b->va + 4ull * i);
    b->ninsns = i;
    emit_ool(e);

    /* Pool after the code, 8-byte aligned; blocks start 8-byte aligned. */
    if (e->n & 1) put(e, 0xd503201f);
    uint32_t pool_at = e->n;
    uint32_t total = e->n + e->npool * 2;
    if (total > MAX_WORDS) return -1;

    for (uint32_t k = 0; k < e->nfix; k++) {
        LitFix *f = &e->fix[k];
        int64_t off = (int64_t)(pool_at + f->idx * 2) - f->at;
        if (f->kind == FIX_LDR) {
            patch_branch(&e->w[f->at], off);
        } else {
            int64_t bytes = off * 4;
            e->w[f->at] |= (uint32_t)(bytes & 3) << 29 | (uint32_t)((bytes >> 2) & 0x7ffff) << 5;
        }
    }

    if (m->jit.used & 1) m->jit.used++;
    uint32_t *dst = hv_jit_reserve(&m->jit, total);
    if (!dst) return -1;
    hv_jit_begin_write();
    memcpy(dst, e->w, pool_at * 4);
    memcpy(dst + pool_at, e->pool, e->npool * 8);
    hv_jit_end_write(dst, total);
    b->code = dst;
    return 0;
}
