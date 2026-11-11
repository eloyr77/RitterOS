/*
 * atomhost's oracle (see README): Linux's atom.c built as it is, behind a
 * plain interface, its machine atomhost.c's (atomhost_read/_write/
 * _delay_us/_tick). Built with -I scripts/atomoracle -I tools/src/atom-VER.
 */
#include "atom.c"

/* atomhost.c's machine: kind 'R' a register, 'P' a PLL, 'M' an MC one. */
uint32_t atomhost_read(int kind, uint32_t index);
void atomhost_write(int kind, uint32_t index, uint32_t value);

static uint32_t rr(struct card_info *c, uint32_t i) { (void)c; return atomhost_read('R', i); }
static void rw(struct card_info *c, u32 i, uint32_t v) { (void)c; atomhost_write('R', i, v); }
static uint32_t pr(struct card_info *c, uint32_t i) { (void)c; return atomhost_read('P', i); }
static void pw(struct card_info *c, u32 i, uint32_t v) { (void)c; atomhost_write('P', i, v); }
static uint32_t mr(struct card_info *c, uint32_t i) { (void)c; return atomhost_read('M', i); }
static void mw(struct card_info *c, u32 i, uint32_t v) { (void)c; atomhost_write('M', i, v); }

static struct card_info card = {
    .reg_read = rr, .reg_write = rw, .pll_read = pr, .pll_write = pw,
    .mc_read = mr, .mc_write = mw,
};

void *oracle_parse(void *bios, uint32_t *scratch, int scratch_bytes)
{
    struct atom_context *ctx = amdgpu_atom_parse(&card, bios);
    if (ctx) {
        ctx->scratch = scratch;
        ctx->scratch_size_bytes = scratch_bytes;
    }
    return ctx;
}

int oracle_run(void *ctx, int index, uint32_t *params)
{
    return amdgpu_atom_execute_table(ctx, index, params);
}

void oracle_destroy(void *ctx)
{
    amdgpu_atom_destroy(ctx);
}
