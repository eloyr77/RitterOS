/*
 * Register allocation (61a): where each value of a function lives. A value
 * is live wherever a later use may still read what it holds; the code is
 * split into blocks (at labels and after jumps), and a value used in more
 * than one block gets the blocks where it is live by the usual backward
 * walk (live at a block's start if used there before being set, live at
 * its end if live at the start of a block that follows). Its interval is
 * then the first to the last instruction where it is live: a register
 * held over that whole span is right, holes and all. Linear scan gives the
 * registers out by interval start; when none is free, the interval that
 * ends last goes to the stack. The backends use only registers a call
 * keeps (callee-saved), so calls need no care here.
 */
#include "regalloc.h"

struct occurrence {
    int at;
    bool def;
};

void regalloc_run(struct ir_func *f, int ngpr, int nfpr, int nfpr_kept, unsigned fpr_classes, const bool *exclude,
                  const unsigned *clobbers, int nsaved, struct regalloc *out)
{
    int nv = f->nvalues, nc = f->ncode;
    out->reg = malloc(sizeof(int) * (size_t)nv);
    out->start = malloc(sizeof(int) * (size_t)nv);
    out->end = malloc(sizeof(int) * (size_t)nv);
    for (int v = 0; v < nv; v++)
        out->reg[v] = -1, out->start[v] = -1, out->end[v] = -1;
    if (!nc)
        return;

    /* blocks: a label starts one, a jump, branch, return or trap ends one */
    int *block_of = malloc(sizeof(int) * (size_t)nc);
    int nblocks = 0;
    int cap = 256;
    int *bstart = malloc(sizeof(int) * (size_t)cap), *bend = malloc(sizeof(int) * (size_t)cap);
    for (int k = 0; k < nc; k++) {
        enum ir_op prev = k ? f->code[k - 1].op : IR_LABEL;
        if (k == 0 || f->code[k].op == IR_LABEL || prev == IR_JMP || prev == IR_BR || prev == IR_RET ||
            prev == IR_TRAP) {
            if (nblocks == cap) {
                cap *= 2;
                bstart = realloc(bstart, sizeof(int) * (size_t)cap);
                bend = realloc(bend, sizeof(int) * (size_t)cap);
            }
            if (nblocks)
                bend[nblocks - 1] = k - 1;
            bstart[nblocks++] = k;
        }
        block_of[k] = nblocks - 1;
    }
    bend[nblocks - 1] = nc - 1;
    int *label_block = calloc((size_t)f->nlabels + 2, sizeof(int));
    for (int k = 0; k < nc; k++)
        if (f->code[k].op == IR_LABEL)
            label_block[f->code[k].label] = block_of[k];
    /* successors (at most two) and predecessors */
    int *succ = malloc(sizeof(int) * 2 * (size_t)nblocks);
    int *npred = calloc((size_t)nblocks, sizeof(int));
    for (int b = 0; b < nblocks; b++) {
        struct ir *last = &f->code[bend[b]];
        int s0 = -1, s1 = -1;
        if (last->op == IR_JMP)
            s0 = label_block[last->label];
        else if (last->op == IR_BR) {
            s0 = label_block[last->label];
            s1 = label_block[last->label2];
        } else if (last->op != IR_RET && last->op != IR_TRAP && b + 1 < nblocks)
            s0 = b + 1;
        succ[2 * b] = s0;
        succ[2 * b + 1] = s1;
        if (s0 >= 0)
            npred[s0]++;
        if (s1 >= 0 && s1 != s0)
            npred[s1]++;
    }
    int *pred_at = malloc(sizeof(int) * (size_t)(nblocks + 1));
    pred_at[0] = 0;
    for (int b = 0; b < nblocks; b++)
        pred_at[b + 1] = pred_at[b] + npred[b];
    int *preds = malloc(sizeof(int) * (size_t)(pred_at[nblocks] + 1));
    int *fill = calloc((size_t)nblocks, sizeof(int));
    for (int b = 0; b < nblocks; b++)
        for (int j = 0; j < 2; j++) {
            int s = succ[2 * b + j];
            if (s < 0 || (j == 1 && s == succ[2 * b]))
                continue;
            preds[pred_at[s] + fill[s]++] = b;
        }

    /* how deep in loops each instruction is (a jump back to a label makes
     * one): a use in a loop counts more when registers are given out */
    int *depth = calloc((size_t)nc + 1, sizeof(int));
    {
        int *label_at = calloc((size_t)f->nlabels + 2, sizeof(int));
        for (int k = 0; k < nc; k++)
            if (f->code[k].op == IR_LABEL)
                label_at[f->code[k].label] = k;
        for (int k = 0; k < nc; k++) {
            struct ir *i = &f->code[k];
            int targets[2] = { i->op == IR_JMP || i->op == IR_BR ? i->label : 0, i->op == IR_BR ? i->label2 : 0 };
            for (int t = 0; t < 2; t++)
                if (targets[t] && label_at[targets[t]] < k) {
                    depth[label_at[targets[t]]]++;
                    depth[k + 1]--;
                }
        }
        for (int k = 1; k <= nc; k++)
            depth[k] += depth[k - 1];
        free(label_at);
    }

    /* every value's occurrences, in order */
    int *count = calloc((size_t)nv, sizeof(int));
#define EACH(i, BODY)                                                                              \
    do {                                                                                           \
        int vv_[4] = { (i)->a, (i)->b, (i)->c, 0 };                                                \
        for (int j_ = 0; j_ < 3; j_++)                                                             \
            if (vv_[j_] > 0) {                                                                     \
                int v = vv_[j_];                                                                   \
                bool def = false;                                                                  \
                BODY;                                                                              \
            }                                                                                      \
        if ((i)->call) {                                                                           \
            for (int j_ = 0; j_ < (i)->call->nargs; j_++) {                                        \
                int v = (i)->call->args[j_].value;                                                 \
                bool def = false;                                                                  \
                if (v > 0) {                                                                       \
                    BODY;                                                                          \
                }                                                                                  \
            }                                                                                      \
            if ((i)->call->ret_buffer > 0) {                                                       \
                int v = (i)->call->ret_buffer;                                                     \
                bool def = false;                                                                  \
                BODY;                                                                              \
            }                                                                                      \
        }                                                                                          \
        if ((i)->dst > 0) {                                                                        \
            int v = (i)->dst;                                                                      \
            bool def = true;                                                                       \
            BODY;                                                                                  \
        }                                                                                          \
    } while (0)
    for (int k = 0; k < nc; k++)
        EACH(&f->code[k], { (void)def; count[v]++; });
    int *occ_at = malloc(sizeof(int) * (size_t)(nv + 1));
    occ_at[0] = 0;
    for (int v = 0; v < nv; v++)
        occ_at[v + 1] = occ_at[v] + count[v];
    struct occurrence *occ = malloc(sizeof(struct occurrence) * (size_t)(occ_at[nv] + 1));
    memset(count, 0, sizeof(int) * (size_t)nv);
    for (int k = 0; k < nc; k++)
        EACH(&f->code[k], {
            occ[occ_at[v] + count[v]].at = k;
            occ[occ_at[v] + count[v]].def = def;
            count[v]++;
        });

    /* intervals */
    char *live_in = calloc((size_t)nblocks, 1), *live_out = calloc((size_t)nblocks, 1);
    char *defines = calloc((size_t)nblocks, 1), *exposed = calloc((size_t)nblocks, 1);
    int *work = malloc(sizeof(int) * (size_t)nblocks);
    for (int v = 1; v < nv; v++) {
        int n = occ_at[v + 1] - occ_at[v];
        if (!n)
            continue;
        struct occurrence *o = &occ[occ_at[v]];
        int b0 = block_of[o[0].at];
        bool local = o[0].def;
        for (int j = 1; j < n && local; j++)
            if (block_of[o[j].at] != b0)
                local = false;
        if (local) {
            out->start[v] = o[0].at;
            out->end[v] = o[n - 1].at;
            continue;
        }
        /* per block: used before being set (exposed)? set? */
        for (int j = 0; j < n; j++) {
            int b = block_of[o[j].at];
            if (!o[j].def && !defines[b])
                exposed[b] = 1;
            if (o[j].def)
                defines[b] = 1;
        }
        /* backward: live in where exposed; then live out at every
         * predecessor, and live in there too unless it sets the value */
        int nwork = 0;
        for (int b = 0; b < nblocks; b++)
            if (exposed[b]) {
                live_in[b] = 1;
                work[nwork++] = b;
            }
        int first = o[0].at, last = o[n - 1].at;
        while (nwork) {
            int b = work[--nwork];
            if (bstart[b] < first)
                first = bstart[b];
            for (int p = pred_at[b]; p < pred_at[b + 1]; p++) {
                int pb = preds[p];
                if (!live_out[pb]) {
                    live_out[pb] = 1;
                    if (bend[pb] > last)
                        last = bend[pb];
                    if (bend[pb] < first)
                        first = bend[pb];
                }
                if (!defines[pb] && !live_in[pb]) {
                    live_in[pb] = 1;
                    work[nwork++] = pb;
                }
            }
        }
        out->start[v] = first;
        out->end[v] = last;
        for (int b = 0; b < nblocks; b++)
            live_in[b] = live_out[b] = defines[b] = exposed[b] = 0;
    }

    /* each value's weight: its occurrences (8 times more for each loop
     * around them) for the length of its interval */
    double *weight = calloc((size_t)nv, sizeof(double));
    for (int v = 1; v < nv; v++) {
        if (out->start[v] < 0)
            continue;
        double w = 0;
        for (int j = occ_at[v]; j < occ_at[v + 1]; j++) {
            int d = depth[occ[j].at];
            w += d >= 4 ? 4096 : d == 3 ? 512 : d == 2 ? 64 : d == 1 ? 8 : 1;
        }
        weight[v] = w / (double)(out->end[v] - out->start[v] + 1);
    }

    /* linear scan */
    int *order = malloc(sizeof(int) * (size_t)nv), m = 0;
    {
        int *bucket = calloc((size_t)nc + 1, sizeof(int));
        for (int v = 1; v < nv; v++)
            if (exclude && exclude[v])
                out->start[v] = out->end[v] = -1;
        for (int v = 1; v < nv; v++)
            if (out->start[v] >= 0)
                bucket[out->start[v] + 1]++;
        for (int k = 0; k < nc; k++)
            bucket[k + 1] += bucket[k];
        for (int v = 1; v < nv; v++)
            if (out->start[v] >= 0)
                order[bucket[out->start[v]]++] = v;
        m = 0;
        for (int v = 1; v < nv; v++)
            if (out->start[v] >= 0)
                m++;
        free(bucket);
    }
    /* (61c) per register that something overwrites: how many such
     * instructions come before each place */
    int *over[32] = { 0 };
    if (clobbers)
        for (int r = nsaved; r < ngpr && r < 32; r++) {
            over[r] = calloc((size_t)nc + 1, sizeof(int));
            for (int k = 0; k < nc; k++)
                over[r][k + 1] = over[r][k] + ((clobbers[k] >> r) & 1);
        }
#define FITS(v, r) (!over[r] || over[r][out->end[v] + 1] == over[r][out->start[v] + 1])
    /* (62c) bit 31: a barrier (inline assembly), no register of either kind
     * holds a value over it */
    int *barrier = 0;
    if (clobbers) {
        barrier = calloc((size_t)nc + 1, sizeof(int));
        for (int k = 0; k < nc; k++)
            barrier[k + 1] = barrier[k] + (int)((clobbers[k] >> 31) & 1);
    }
#define CROSSES_BARRIER(v) (barrier && barrier[out->end[v] + 1] != barrier[out->start[v] + 1])
    /* (32h) and the instructions that overwrite every register of the
     * second kind: a value of that kind living over one stays on the stack
     * (one that only uses or sets the value there does not count) */
    int *fclob = 0;
    if (clobbers) {
        fclob = calloc((size_t)nc + 1, sizeof(int));
        for (int k = 0; k < nc; k++)
            fclob[k + 1] = fclob[k] + (int)((clobbers[k] & CLOBBERS_FPRS) != 0);
    }
#define CROSSES_FCLOB(v) (fclob && fclob[out->end[v]] != fclob[out->start[v] + 1])
    /* (40d) and those using the second kind's registers from nfpr_kept on:
     * none from a value's start to its end, both included */
    int *lowclob = 0;
    if (clobbers) {
        lowclob = calloc((size_t)nc + 1, sizeof(int));
        for (int k = 0; k < nc; k++)
            lowclob[k + 1] = lowclob[k] + (int)((clobbers[k] & CLOBBERS_LOW_FPRS) != 0);
    }
#define LOW_FITS(v) (!lowclob || lowclob[out->end[v] + 1] == lowclob[out->start[v]])
#define REG_OK(v, r) (cls == 0 ? FITS(v, r) : (r) < nfpr_kept || LOW_FITS(v))
    for (int cls = 0; cls < 2; cls++) {
        int nregs = cls == 0 ? ngpr : nfpr;
        if (!nregs)
            continue;
        int saved = cls == 0 ? nsaved : nregs;
        int active[64], nactive = 0;            /* values in registers, by increasing end */
        bool used[64] = { 0 };
        for (int j = 0; j < m; j++) {
            int v = order[j];
            enum ir_class c = f->values[v].cls;
            bool wants = cls == 0 ? (c == V_I32 || c == V_I64) : (fpr_classes >> c & 1) != 0;
            if (!wants)
                continue;
            int s = out->start[v];
            if (CROSSES_BARRIER(v) || (cls == 1 && CROSSES_FCLOB(v)))
                continue;                       /* (the stack) */
            /* expire; (40a) an operand whose life ends at the instruction
             * that makes v gives v its register: every backend reads its
             * operands before it writes the result (not inline assembly
             * given registers: a template may write its output first) */
            struct ir *ins = &f->code[s];
            bool share = ins->dst == v && ins->op != IR_ASM;
            int keep = 0, pref = -1;
            for (int a = 0; a < nactive; a++) {
                int u = active[a];
                bool reads = false;
                if (share && out->end[u] == s)
                    EACH(ins, { if (!def && v == u) reads = true; });   /* (EACH's v: the operand) */
                if (reads && u == ins->a)
                    pref = out->reg[u];         /* (40a-2: the first operand's, two-operand x86's) */
                if (out->end[u] < s || reads)
                    used[out->reg[u]] = false;
                else
                    active[keep++] = u;
            }
            nactive = keep;
            /* that register first, then those that need no saving */
            int r = pref >= 0 && !used[pref] && REG_OK(v, pref) ? pref : -1;
            for (int n = 0; n < nregs && r < 0; n++) {
                int k = (saved + n) % nregs;
                if (!used[k] && REG_OK(v, k))
                    r = k;
            }
            if (r < 0) {
                /* the lightest (this one or an active one whose register
                 * it may have) goes to the stack */
                int victim = -1;
                for (int a = 0; a < nactive; a++)
                    if (REG_OK(v, out->reg[active[a]]) &&
                        (victim < 0 || weight[active[a]] < weight[active[victim]]))
                        victim = a;
                if (victim < 0 || weight[active[victim]] >= weight[v])
                    continue;
                r = out->reg[active[victim]];
                out->reg[active[victim]] = -1;
                for (int a = victim; a + 1 < nactive; a++)
                    active[a] = active[a + 1];
                nactive--;
            }
            out->reg[v] = r;
            used[r] = true;
            int at = nactive++;
            while (at > 0 && out->end[active[at - 1]] > out->end[v]) {
                active[at] = active[at - 1];
                at--;
            }
            active[at] = v;
        }
    }
#undef FITS
#undef CROSSES_BARRIER
#undef CROSSES_FCLOB
#undef LOW_FITS
#undef REG_OK
    free(barrier);
    free(fclob);
    free(lowclob);
    for (int r = 0; r < 32; r++)
        free(over[r]);
    free(order);
    free(weight);
    free(depth);
    free(block_of);
    free(bstart);
    free(bend);
    free(label_block);
    free(succ);
    free(npred);
    free(pred_at);
    free(preds);
    free(fill);
    free(count);
    free(occ_at);
    free(occ);
    free(live_in);
    free(live_out);
    free(defines);
    free(exposed);
    free(work);
}
