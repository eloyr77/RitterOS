#include "nvme.h"
#include "sleep.h"
#include "arch.h"
#include "heap.h"
#include "kprintf.h"
#include "memory.h"
#include "paging.h"
#include "pmm.h"
#include "string.h"
#include "timer.h"
#include "sync.h"

/* Controller registers (NVMe Base Specification 2.0, section 3.1). */
#define REG_CAP     0x00        /* capabilities (64-bit) */
#define REG_VS      0x08        /* version */
#define REG_CC      0x14        /* controller configuration */
#define REG_CSTS    0x1C        /* controller status */
#define REG_AQA     0x24        /* admin queue sizes */
#define REG_ASQ     0x28        /* admin submission queue base (64-bit) */
#define REG_ACQ     0x30        /* admin completion queue base (64-bit) */
#define REG_DOORBELLS 0x1000

#define CC_ENABLE       (1u << 0)
#define CC_IOSQES_64    (6u << 16)   /* I/O submission entries: 2^6 = 64 bytes */
#define CC_IOCQES_16    (4u << 20)   /* I/O completion entries: 2^4 = 16 bytes */
#define CSTS_READY      (1u << 0)
#define CSTS_FATAL      (1u << 1)
#define CC_SHN_NORMAL   (1u << 14)   /* (37g-2) shutdown notification: normal */
#define CC_SHN          (3u << 14)
#define CSTS_SHST       (3u << 2)    /* shutdown status: 2 complete */
#define CSTS_SHST_DONE  (2u << 2)

/* Command opcodes. */
#define ADMIN_CREATE_IO_SQ  0x01
#define ADMIN_CREATE_IO_CQ  0x05
#define ADMIN_IDENTIFY      0x06
#define IO_FLUSH            0x00
#define IO_WRITE            0x01
#define IO_READ             0x02
#define IO_DATASET_MANAGEMENT 0x09    /* with AD: deallocate (TRIM) */
#define DSM_DEALLOCATE      (1u << 2)
#define ONCS_DSM            (1u << 2)   /* identify controller: dataset management */

#define ADMIN_QUEUE_SIZE    32
#define IO_QUEUE_SIZE       64
#define BOUNCE_PAGES        16          /* 64 KiB transfer buffer */
#define STATUS_TIMEOUT      0xFFFF

struct nvme_command {               /* submission queue entry, 64 bytes */
    uint8_t opcode;
    uint8_t flags;
    uint16_t command_id;
    uint32_t nsid;
    uint64_t reserved;
    uint64_t metadata;
    uint64_t prp1;                  /* data pointer: first page */
    uint64_t prp2;                  /* second page, or a list of pages */
    uint32_t cdw10, cdw11, cdw12, cdw13, cdw14, cdw15;
};

struct nvme_completion {            /* completion queue entry, 16 bytes */
    uint32_t result;
    uint32_t reserved;
    uint16_t sq_head;
    uint16_t sq_id;
    uint16_t command_id;
    uint16_t status;                /* bit 0: phase tag; bits 1-15: status */
};

_Static_assert(sizeof(struct nvme_command) == 64, "NVMe command size");
_Static_assert(sizeof(struct nvme_completion) == 16, "NVMe completion size");

struct queue {
    void *entries;                  /* direct-map address */
    uint64_t phys;
    uint16_t size;
    uint16_t index;                 /* submission: tail; completion: head */
    uint8_t phase;                  /* completion: expected phase tag */
    volatile uint32_t *doorbell;
};

struct nvme {
    struct pci_device *pci;
    volatile uint8_t *regs;
    uint32_t doorbell_stride;
    uint32_t timeout_ms;
    struct queue admin_sq, admin_cq, io_sq, io_cq;
    uint16_t next_command_id;
    uint8_t *bounce;                /* physically contiguous transfer buffer */
    uint64_t bounce_phys;
    uint64_t *prp_list;
    uint64_t prp_list_phys;
    uint32_t max_blocks;            /* per command */
    bool interrupts;                /* MSI-X on: sleep instead of polling */
    volatile uint64_t interrupt_count;
    struct wait_queue completions;  /* threads waiting for a completion */
    struct mutex lock;              /* one command at a time (queues, bounce buffer) */
    uint64_t command_count, timeouts;
    uint64_t discarded;             /* blocks deallocated (TRIM) */
    struct block_device dev;
};

static uint32_t reg_read32(struct nvme *c, uint32_t off)
{
    return mmio_read32(c->regs + off);
}

static void reg_write32(struct nvme *c, uint32_t off, uint32_t v)
{
    mmio_write32(c->regs + off, v);
}

static uint64_t reg_read64(struct nvme *c, uint32_t off)
{
    return reg_read32(c, off) | (uint64_t)reg_read32(c, off + 4) << 32;
}

static void reg_write64(struct nvme *c, uint32_t off, uint64_t v)
{
    reg_write32(c, off, (uint32_t)v);
    reg_write32(c, off + 4, (uint32_t)(v >> 32));
}

/* Wait until (CSTS & mask) == want, up to the controller's timeout. */
static bool wait_status(struct nvme *c, uint32_t mask, uint32_t want)
{
    for (uint32_t ms = 0; ms <= c->timeout_ms; ms++) {
        if ((reg_read32(c, REG_CSTS) & mask) == want)
            return true;
        arch_delay_ms(1);
    }
    return false;
}

static bool queue_init(struct nvme *c, struct queue *q, uint16_t size, unsigned doorbell_index)
{
    q->phys = pmm_alloc_page();
    if (!q->phys)
        return false;
    q->entries = phys_to_virt(q->phys);
    memset(q->entries, 0, PMM_PAGE_SIZE);
    q->size = size;
    q->index = 0;
    q->phase = 1;
    q->doorbell = (volatile uint32_t *)(c->regs + REG_DOORBELLS +
                                        doorbell_index * c->doorbell_stride);
    return true;
}

/* Reset (disabled, then not ready), the admin queues given anew (their
 * memory cleared: tails, heads and phase from the start), enabled. The
 * boot's, and (37g-2) after a sleep. */
static const char *start_queues(struct nvme *c)
{
    reg_write32(c, REG_CC, reg_read32(c, REG_CC) & ~CC_ENABLE);
    if (!wait_status(c, CSTS_READY, 0))
        return "controller does not stop";
    struct queue *qs[4] = { &c->admin_sq, &c->admin_cq, &c->io_sq, &c->io_cq };
    for (int i = 0; i < 4; i++) {
        memset(qs[i]->entries, 0, PMM_PAGE_SIZE);
        qs[i]->index = 0;
        qs[i]->phase = 1;
    }
    uint16_t admin_size = c->admin_sq.size;
    reg_write32(c, REG_AQA, (uint32_t)(admin_size - 1) << 16 | (uint32_t)(admin_size - 1));
    reg_write64(c, REG_ASQ, c->admin_sq.phys);
    reg_write64(c, REG_ACQ, c->admin_cq.phys);
    reg_write32(c, REG_CC, CC_ENABLE | CC_IOSQES_64 | CC_IOCQES_16);
    if (!wait_status(c, CSTS_READY | CSTS_FATAL, CSTS_READY))
        return "controller does not become ready";
    return 0;
}

static uint16_t admin(struct nvme *c, struct nvme_command *cmd);

/* One I/O queue pair (ID 1); its completions raise interrupt 0 (MSI-X
 * entry 0) if interrupts are on. */
static const char *create_io_queues(struct nvme *c)
{
    uint16_t io_size = c->io_sq.size;
    struct nvme_command cmd = {0};
    cmd.opcode = ADMIN_CREATE_IO_CQ;
    cmd.prp1 = c->io_cq.phys;
    cmd.cdw10 = (uint32_t)(io_size - 1) << 16 | 1;
    cmd.cdw11 = 1 | (c->interrupts ? 2u : 0);       /* contiguous; interrupts on, vector 0 */
    if (admin(c, &cmd) != 0)
        return "creating the I/O completion queue failed";
    memset(&cmd, 0, sizeof(cmd));
    cmd.opcode = ADMIN_CREATE_IO_SQ;
    cmd.prp1 = c->io_sq.phys;
    cmd.cdw10 = (uint32_t)(io_size - 1) << 16 | 1;
    cmd.cdw11 = 1u << 16 | 1;                       /* completions go to CQ 1; contiguous */
    if (admin(c, &cmd) != 0)
        return "creating the I/O submission queue failed";
    return 0;
}

static void nvme_interrupt(void *context)
{
    struct nvme *c = context;
    c->interrupt_count++;           /* the waiting code checks the queue itself */
    wait_queue_wake_all(&c->completions);
}

/* Has the controller written the completion entry e (phase tag as expected)? */
static bool completed(volatile struct nvme_completion *e, uint8_t phase)
{
    return (e->status & 1) == phase;
}

/* Sleep until the entry is completed, woken by the controller's interrupt;
 * other threads run meanwhile. */
static bool sleep_until_completed(struct nvme *c, volatile struct nvme_completion *e,
                                  uint8_t phase)
{
    uint64_t deadline = timer_ticks() + (uint64_t)c->timeout_ms * TIMER_HZ / 1000 + 2;
    return wait_event_until(&c->completions, completed(e, phase), deadline);
}

/*
 * Submit one command and wait for its completion. Returns the status field
 * (0 = success) or STATUS_TIMEOUT.
 */
static uint16_t execute(struct nvme *c, struct queue *sq, struct queue *cq,
                        struct nvme_command *cmd, uint32_t *result)
{
    cmd->command_id = c->next_command_id++;
    memcpy((struct nvme_command *)sq->entries + sq->index, cmd, sizeof(*cmd));
    sq->index = (uint16_t)((sq->index + 1) % sq->size);
    arch_write_barrier();                          /* command in RAM before the doorbell */
    mmio_write32(sq->doorbell, sq->index);

    /* The controller flips the phase tag of each entry it writes, so a new
     * entry is one whose phase matches what we expect for this pass. */
    volatile struct nvme_completion *e = (struct nvme_completion *)cq->entries + cq->index;
    c->command_count++;
    if (c->interrupts) {
        if (!sleep_until_completed(c, e, cq->phase)) {
            c->timeouts++;
            return STATUS_TIMEOUT;
        }
    } else {
        uint32_t waited_ms = 0;
        for (uint32_t spins = 0; !completed(e, cq->phase); spins++) {
            if (spins < 100000) {
                arch_cpu_relax();
                continue;
            }
            if (waited_ms++ > c->timeout_ms) {
                c->timeouts++;
                return STATUS_TIMEOUT;
            }
            arch_delay_ms(1);
        }
    }
    arch_read_barrier();                           /* read the entry after its phase bit */

    uint16_t status = e->status >> 1;
    if (result)
        *result = e->result;
    if (e->command_id != cmd->command_id)
        status = STATUS_TIMEOUT;                    /* not ours: should never happen */
    cq->index = (uint16_t)((cq->index + 1) % cq->size);
    if (cq->index == 0)
        cq->phase ^= 1;
    mmio_write32(cq->doorbell, cq->index);
    return status;
}

static uint16_t admin(struct nvme *c, struct nvme_command *cmd)
{
    return execute(c, &c->admin_sq, &c->admin_cq, cmd, 0);
}

/* Copy a space-padded ASCII field and trim trailing spaces. */
static void copy_field(char *out, const uint8_t *in, int len)
{
    memcpy(out, in, (size_t)len);
    out[len] = 0;
    while (len > 0 && (out[len - 1] == ' ' || out[len - 1] == 0))
        out[--len] = 0;
}

/* Read or write up to max_blocks through the bounce buffer. */
static bool transfer(struct nvme *c, uint8_t opcode, uint64_t lba, uint32_t count)
{
    uint64_t bytes = (uint64_t)count * c->dev.block_size;
    uint64_t pages = (bytes + PMM_PAGE_SIZE - 1) / PMM_PAGE_SIZE;
    struct nvme_command cmd = {0};
    cmd.opcode = opcode;
    cmd.nsid = 1;
    cmd.prp1 = c->bounce_phys;
    /* Up to two pages fit in PRP1/PRP2; more need a list of page addresses. */
    if (pages == 2) {
        cmd.prp2 = c->bounce_phys + PMM_PAGE_SIZE;
    } else if (pages > 2) {
        for (uint64_t i = 1; i < pages; i++)
            c->prp_list[i - 1] = c->bounce_phys + i * PMM_PAGE_SIZE;
        cmd.prp2 = c->prp_list_phys;
    }
    cmd.cdw10 = (uint32_t)lba;
    cmd.cdw11 = (uint32_t)(lba >> 32);
    cmd.cdw12 = count - 1;                          /* number of blocks, 0-based */

    uint16_t status = execute(c, &c->io_sq, &c->io_cq, &cmd, 0);
    if (status != 0) {
        kprintf("  %s: %s of %u blocks at LBA %lu failed, status 0x%x\n", c->dev.name,
                opcode == IO_READ ? "read" : "write", count, lba, status);
        return false;
    }
    return true;
}

static bool nvme_read_locked(struct block_device *dev, uint64_t lba, uint32_t count, void *buffer)
{
    struct nvme *c = dev->driver_data;
    uint8_t *out = buffer;
    if (lba + count > dev->block_count || lba + count < lba)
        return false;
    while (count > 0) {
        uint32_t n = count < c->max_blocks ? count : c->max_blocks;
        if (!transfer(c, IO_READ, lba, n))
            return false;
        memcpy(out, c->bounce, (size_t)n * dev->block_size);
        out += (size_t)n * dev->block_size;
        lba += n;
        count -= n;
    }
    return true;
}

static bool nvme_write_locked(struct block_device *dev, uint64_t lba, uint32_t count, const void *buffer)
{
    struct nvme *c = dev->driver_data;
    const uint8_t *in = buffer;
    if (lba + count > dev->block_count || lba + count < lba)
        return false;
    while (count > 0) {
        uint32_t n = count < c->max_blocks ? count : c->max_blocks;
        memcpy(c->bounce, in, (size_t)n * dev->block_size);
        if (!transfer(c, IO_WRITE, lba, n))
            return false;
        in += (size_t)n * dev->block_size;
        lba += n;
        count -= n;
    }
    return true;
}

static bool nvme_flush_locked(struct block_device *dev);

static bool flush_now(struct nvme *c)
{
    return nvme_flush_locked(&c->dev);
}

static bool nvme_flush_locked(struct block_device *dev)
{
    struct nvme *c = dev->driver_data;
    struct nvme_command cmd = {0};
    cmd.opcode = IO_FLUSH;
    cmd.nsid = 1;
    uint16_t status = execute(c, &c->io_sq, &c->io_cq, &cmd, 0);
    if (status != 0)
        kprintf("  %s: flush failed, status 0x%x\n", dev->name, status);
    return status == 0;
}

static bool nvme_read(struct block_device *dev, uint64_t lba, uint32_t count, void *buffer)
{
    struct nvme *c = dev->driver_data;
    mutex_lock(&c->lock);
    bool ok = nvme_read_locked(dev, lba, count, buffer);
    mutex_unlock(&c->lock);
    return ok;
}

static bool nvme_write(struct block_device *dev, uint64_t lba, uint32_t count, const void *buffer)
{
    struct nvme *c = dev->driver_data;
    mutex_lock(&c->lock);
    bool ok = nvme_write_locked(dev, lba, count, buffer);
    mutex_unlock(&c->lock);
    return ok;
}

/* Deallocate count blocks from lba: one range per command, at most 2^32 - 1
 * blocks each. The range list lives in the bounce buffer. */
static bool nvme_discard(struct block_device *dev, uint64_t lba, uint64_t count)
{
    struct nvme *c = dev->driver_data;
    bool ok = true;
    mutex_lock(&c->lock);
    while (ok && count) {
        uint32_t n = count > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)count;
        memset(c->bounce, 0, 16);
        memcpy(c->bounce + 4, &n, 4);               /* length in blocks */
        memcpy(c->bounce + 8, &lba, 8);             /* starting block */
        struct nvme_command cmd = {0};
        cmd.opcode = IO_DATASET_MANAGEMENT;
        cmd.nsid = 1;
        cmd.prp1 = c->bounce_phys;
        cmd.cdw10 = 0;                              /* one range (0-based) */
        cmd.cdw11 = DSM_DEALLOCATE;
        uint16_t status = execute(c, &c->io_sq, &c->io_cq, &cmd, 0);
        if (status != 0) {
            kprintf("  %s: deallocate of %u blocks at LBA %lu failed, status 0x%x\n", dev->name,
                    n, lba, status);
            ok = false;
        } else {
            c->discarded += n;
        }
        lba += n;
        count -= n;
    }
    mutex_unlock(&c->lock);
    return ok;
}

static bool nvme_flush(struct block_device *dev)
{
    struct nvme *c = dev->driver_data;
    mutex_lock(&c->lock);
    bool ok = nvme_flush_locked(dev);
    mutex_unlock(&c->lock);
    return ok;
}

static struct block_device *fail(const char *why)
{
    kprintf("  NVMe:          %s\n", why);
    return 0;
}

/* (37g-2) Sleep: no command from here (the lock kept until resume), the
 * written blocks flushed, the controller shut down normally (as Linux's
 * nvme_disable_ctrl with shutdown); awake, started as at the boot. */
static bool flush_now(struct nvme *c);

static bool nvme_suspend(void *ctx)
{
    struct nvme *c = ctx;
    mutex_lock(&c->lock);
    flush_now(c);
    reg_write32(c, REG_CC, (reg_read32(c, REG_CC) & ~CC_SHN) | CC_SHN_NORMAL);
    if (!wait_status(c, CSTS_SHST, CSTS_SHST_DONE))
        kprintf("  %s: shutdown not reported done (CSTS 0x%x)\n", c->dev.name,
                reg_read32(c, REG_CSTS));
    return true;
}

static bool nvme_resume(void *ctx)
{
    struct nvme *c = ctx;
    const char *why = start_queues(c);
    if (!why)
        why = create_io_queues(c);
    mutex_unlock(&c->lock);
    if (why)
        kprintf("  %s: after the sleep: %s\n", c->dev.name, why);
    return !why;
}

struct block_device *nvme_init(struct pci_device *pci, int index)
{
    struct pci_bar *bar = &pci->bars[0];
    if (!bar->size || bar->is_io)
        return fail("controller has no register BAR");

    struct nvme *c = kzalloc(sizeof(*c));
    if (!c)
        return fail("out of memory");
    c->pci = pci;
    c->lock = (struct mutex)MUTEX_INIT("nvme");

    /* Let the controller respond to memory accesses and use DMA; no legacy
     * interrupts. */
    pci_write16(pci, PCI_COMMAND, pci_read16(pci, PCI_COMMAND) | PCI_COMMAND_MEMORY |
                                  PCI_COMMAND_BUS_MASTER | PCI_COMMAND_INTX_OFF);
    c->regs = paging_map_mmio(bar->address, bar->size);

    /* Interrupts: MSI-X table entry 0, used by the admin and the I/O
     * completion queue. Without it, the driver polls. */
    uint64_t msi_address = 0;
    uint32_t msi_data = 0;
    uint16_t msi_entries = 0;
    c->interrupts = pci_msix_enable(pci, nvme_interrupt, c, &msi_address, &msi_data,
                                    &msi_entries);

    uint64_t cap = reg_read64(c, REG_CAP);
    uint32_t max_entries = (uint32_t)(cap & 0xFFFF) + 1;
    c->doorbell_stride = 4u << ((cap >> 32) & 0xF);
    c->timeout_ms = (uint32_t)((cap >> 24) & 0xFF) * 500;
    if ((cap >> 48) & 0xF)
        return fail("controller does not support 4 KiB memory pages");
    if (!((cap >> 37) & 1))
        return fail("controller does not support the NVM command set");
    uint32_t vs = reg_read32(c, REG_VS);

    /* Admin queues, then enable. Queue sizes are limited by CAP.MQES. */
    uint16_t admin_size = (uint16_t)(max_entries < ADMIN_QUEUE_SIZE ? max_entries : ADMIN_QUEUE_SIZE);
    uint16_t io_size = (uint16_t)(max_entries < IO_QUEUE_SIZE ? max_entries : IO_QUEUE_SIZE);
    if (!queue_init(c, &c->admin_sq, admin_size, 0) || !queue_init(c, &c->admin_cq, admin_size, 1) ||
        !queue_init(c, &c->io_sq, io_size, 2) || !queue_init(c, &c->io_cq, io_size, 3))
        return fail("out of memory for queues");
    const char *why = start_queues(c);
    if (why)
        return fail(why);

    /* Identify the controller (CNS 1) and namespace 1 (CNS 0). */
    uint64_t id_phys = pmm_alloc_page();
    if (!id_phys)
        return fail("out of memory");
    uint8_t *id = phys_to_virt(id_phys);
    struct nvme_command cmd = {0};
    cmd.opcode = ADMIN_IDENTIFY;
    cmd.prp1 = id_phys;
    cmd.cdw10 = 1;
    if (admin(c, &cmd) != 0)
        return fail("identify controller failed");
    char serial[21], model[41], firmware[9];
    copy_field(serial, id + 4, 20);
    copy_field(model, id + 24, 40);
    copy_field(firmware, id + 64, 8);
    uint8_t mdts = id[77];                          /* max transfer: 2^mdts pages, 0 = no limit */
    uint32_t namespaces;
    memcpy(&namespaces, id + 516, 4);
    uint16_t oncs;                                  /* optional NVM commands */
    memcpy(&oncs, id + 520, 2);

    memset(&cmd, 0, sizeof(cmd));
    cmd.opcode = ADMIN_IDENTIFY;
    cmd.nsid = 1;
    cmd.prp1 = id_phys;
    cmd.cdw10 = 0;
    if (namespaces == 0 || admin(c, &cmd) != 0)
        return fail("no namespace 1");
    uint64_t blocks;
    memcpy(&blocks, id, 8);
    uint8_t format = id[26] & 0xF;                  /* the LBA format in use */
    uint32_t block_size = 1u << id[128 + 4 * format + 2];
    pmm_free_page(id_phys);
    if (blocks == 0 || block_size < 512 || block_size > PMM_PAGE_SIZE)
        return fail("unsupported namespace format");

    if ((why = create_io_queues(c)))
        return fail(why);

    /* Transfer buffer and PRP list. */
    c->bounce_phys = pmm_alloc_pages(BOUNCE_PAGES);
    c->prp_list_phys = pmm_alloc_page();
    if (!c->bounce_phys || !c->prp_list_phys)
        return fail("out of memory for buffers");
    c->bounce = phys_to_virt(c->bounce_phys);
    c->prp_list = phys_to_virt(c->prp_list_phys);
    uint64_t max_bytes = BOUNCE_PAGES * PMM_PAGE_SIZE;
    if (mdts && (PMM_PAGE_SIZE << mdts) < max_bytes)
        max_bytes = PMM_PAGE_SIZE << mdts;
    c->max_blocks = (uint32_t)(max_bytes / block_size);

    ksnprintf(c->dev.name, sizeof(c->dev.name), "nvme%dn1", index);
    c->dev.block_size = block_size;
    c->dev.block_count = blocks;
    c->dev.kind = "NVMe";
    ksnprintf(c->dev.model, sizeof(c->dev.model), "%s", model);
    c->dev.read = nvme_read;
    c->dev.write = nvme_write;
    c->dev.flush = nvme_flush;
    c->dev.discard = oncs & ONCS_DSM ? nvme_discard : 0;
    c->dev.driver_data = c;
    sleep_register(c->dev.name, nvme_suspend, nvme_resume, c);     /* (37g-2) */

    kprintf("  NVMe:          %02x:%02x.%u, version %u.%u, registers %p, doorbell stride %u\n",
            pci->bus, pci->device, pci->function, vs >> 16, (vs >> 8) & 0xFF,
            (void *)bar->address, c->doorbell_stride);
    kprintf("                 model '%s', serial '%s', firmware '%s'\n", model, serial, firmware);
    if (c->interrupts)
        kprintf("                 interrupts: MSI-X entry 0 of %u, message 0x%x to %p\n",
                msi_entries, msi_data, (void *)msi_address);
    else
        kprintf("                 interrupts: none (no MSI-X), polling\n");
    kprintf("                 %s: %lu blocks of %u bytes (%lu MiB), queues %u/%u entries, "
            "max transfer %lu KiB\n", c->dev.name, blocks, block_size,
            blocks * block_size / (1024 * 1024), admin_size, io_size, max_bytes / 1024);
    return &c->dev;
}

void nvme_report(struct block_device *dev)
{
    struct nvme *c = dev->driver_data;
    kprintf("  %s:       %lu commands, %lu interrupts, %lu timeouts\n", dev->name,
            c->command_count, c->interrupt_count, c->timeouts);
}
