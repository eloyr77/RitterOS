/*
 * (67b) The driver interface, version DRIVER_API_MAJOR.DRIVER_API_MINOR:
 * what a kernel module (a driver in drivers/, kernel/module.h) may use.
 * A driver includes only this header and its device class's
 * (driver_net.h, ...): it is compiled with drivers/include/ as its only
 * include folder, so nothing else of the kernel is in reach. The kernel
 * exports exactly the functions declared here (scripts/kexports.c makes
 * the loader's table from these headers and refuses a module that uses
 * anything else), and drivers/abi/driver-api.N.txt records the interface
 * (scripts/abicheck.py): adding is compatible (the minor version goes
 * up), removing or changing anything needs a new major version. The
 * loader refuses a module built for another major version, or a newer
 * minor one, than the kernel's. drivers/CLAUDE.md is the guide.
 *
 * The types come first: the kernel uses these same definitions. The
 * functions follow, for drivers only (the kernel has some of them inline:
 * marked DRIVER_WRAPPED, they reach a copy of the kernel's, kernel/driver.c).
 * Everything runs in the kernel, with its rules: no unbounded waits on
 * hardware, no sleeping, mutexes or the heap in an interrupt handler or
 * with a spinlock held, device registers only through mmio_*.
 */
#ifndef RITTEROS_DRIVER_H
#define RITTEROS_DRIVER_H

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define DRIVER_API_MAJOR 6
#define DRIVER_API_MINOR 1

/* --- types ---------------------------------------------------------------- */

/* An interrupt's handler (interrupt context: no sleeping, no mutex, no heap). */
typedef void (*irq_handler_fn)(void *context);

struct thread;
struct poll_entry;

/* A spinlock: interrupts off while held, a ticket lock between cores. All
 * zero is a free lock; give it a name (the lock checker and `cpus`). */
struct spinlock {
    volatile uint32_t next;         /* the ticket the next taker draws */
    volatile uint32_t serving;      /* the ticket whose turn it is */
    volatile int holder;            /* the holding core's index + 1; 0: free */
    const char *name;
    int check_class;                /* (the lock checker's) */
};

#define SPINLOCK_INIT(n) { 0, 0, 0, (n), 0 }

/* Sleeping threads; zero is an empty queue. (2.0) Each queue has its own
 * lock, the kernel's ("wait queue", named at its first use). */
struct wait_queue {
    struct thread *head;            /* sleeping threads, in arrival order */
    bool pollable;                  /* (the kernel's: poll()) */
    struct poll_entry *watchers;    /* (the kernel's: poll()) */
    struct spinlock lock;           /* (the kernel's) */
};

/* A mutex: sleeps while taken, recursive for its owner. Never in an
 * interrupt handler or with a spinlock held. */
struct mutex {
    struct thread *owner;
    unsigned depth;                 /* times the owner has locked it */
    struct wait_queue waiters;
    const char *name;
    struct spinlock lock;           /* guards owner and depth */
};

#define MUTEX_INIT(n) { 0, 0, { 0 }, (n), SPINLOCK_INIT(n) }

/* A PCI device (the kernel's: drivers get pointers, never make one). */
struct pci_bar {
    uint64_t address;               /* physical address (memory) or port (I/O); 0 if unused */
    uint64_t size;
    bool is_io;
    bool is_64bit;
    bool prefetchable;
};

struct pci_device_kernel;           /* (the kernel's own part: kernel/pci.h) */

struct pci_device {
    uint8_t bus, device, function;
    uint16_t vendor_id, device_id;
    uint8_t class_code, subclass, prog_if, revision;
    struct pci_bar bars[6];
    struct pci_device_kernel *kernel;
};

#define PCI_COMMAND             0x04
#define PCI_COMMAND_IO          0x0001  /* respond to I/O port accesses */
#define PCI_COMMAND_MEMORY      0x0002  /* respond to memory accesses */
#define PCI_COMMAND_BUS_MASTER  0x0004  /* may access memory itself (DMA) */
#define PCI_COMMAND_INTX_OFF    0x0400  /* no legacy interrupt pin signals */
#define PCI_CAP_MSI             0x05    /* (1.1) capability IDs (pci_find_capability) */
#define PCI_CAP_MSIX            0x11

#define TIMER_HZ 100                    /* timer ticks a second (timer_ticks) */

/* (1.4) An ACPI table's header (the ACPI specification's): the firmware's
 * tables (acpi_find_table) start with it. */
struct acpi_header {
    char signature[4];
    uint32_t length;                    /* whole table, header included */
    uint8_t revision;
    uint8_t checksum;                   /* all bytes of the table sum to 0 */
    char oem_id[6];
    char oem_table_id[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
} __attribute__((packed));
#define PAGE_SIZE 4096ULL                /* a page (pmm_alloc_page) */

/* A module's declaration, at its end (the loader reads its `.module`
 * section): the PCI IDs it drives (MODULE_ANY: any device of that vendor;
 * (1.5) MODULE_CLASS: any device of that class and subclass, whoever made
 * it) and the function the kernel calls for each matching device, true
 * when it took the device. */
#define MODULE_MAGIC 0x32444F4DU        /* "MOD2": this layout of module_info */
#define MODULE_ANY 0xFFFF
#define MODULE_BY_CLASS 0xFFFE          /* (1.5) as vendor: device is class << 8 | subclass */
#define MODULE_CLASS(class, subclass) { MODULE_BY_CLASS, (uint16_t)((class) << 8 | (subclass)) }

struct module_pci {
    uint16_t vendor, device;
};

struct module_info {
    uint32_t magic;
    uint16_t api_major, api_minor;      /* the driver interface it was built for */
    char name[16];
    bool (*start)(struct pci_device *device);
    /* (5.0) Before the machine sleeps (ACPI S3), each device it took, the
     * last taken first: its work stopped and anything the device must keep
     * written to memory (in S3 only memory keeps its contents, and the
     * device's PCI configuration, which the kernel restores); false if it
     * cannot (the sleep is called off: resume is called for the devices
     * suspended before). After waking, in the order taken: the device as
     * before (its registers, rings and interrupts set up again). Both 0:
     * a machine with this module's devices does not sleep. Threads' calls,
     * every other core running. */
    bool (*suspend)(struct pci_device *device);
    bool (*resume)(struct pci_device *device);
    const struct module_pci *pci;
    uint32_t pci_count;
};

#define MODULE(name, start, suspend, resume, pci_table)                                   \
    __attribute__((section(".module"), used)) static const struct module_info              \
        module_info_ = { MODULE_MAGIC, DRIVER_API_MAJOR, DRIVER_API_MINOR, #name, start,   \
                         suspend, resume, pci_table,                                       \
                         sizeof(pci_table) / sizeof((pci_table)[0]) };

/* --- functions (drivers only) ---------------------------------------------- */
#ifndef RITTEROS_KERNEL
#define DRIVER_WRAPPED

/* The kernel's log and console (printf's formats; %lu for uint64_t). */
void kprintf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
int ksnprintf(char *buf, size_t size, const char *fmt, ...) __attribute__((format(printf, 3, 4)));

void *memcpy(void *dst, const void *src, size_t n);
void *memset(void *dst, int value, size_t n);
int memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);
int strcmp(const char *a, const char *b);

/* The kernel's heap (0 without memory; never in an interrupt handler). */
void *kmalloc(size_t size);
void *kzalloc(size_t size);
void kfree(void *ptr);

/* Physical pages of 4 KiB (0 without memory): DMA buffers. A run of
 * pages is contiguous; _below keeps it under limit (a 32-bit device). */
uint64_t pmm_alloc_page(void);
uint64_t pmm_alloc_pages(size_t count);
uint64_t pmm_alloc_pages_below(size_t count, uint64_t limit);
void pmm_free_page(uint64_t addr);
void pmm_free_pages(uint64_t addr, size_t count);

/* Physical memory through the kernel's direct map, and back (also for a
 * module's own data, though that is not contiguous: DMA from pages). */
DRIVER_WRAPPED void *phys_to_virt(uint64_t phys);
DRIVER_WRAPPED uint64_t virt_to_phys(const void *virt);

/* A device's registers (a BAR's address), uncached; or memory made
 * write-combining (a framebuffer). */
volatile void *paging_map_mmio(uint64_t phys, uint64_t size);
bool paging_write_combining(uint64_t phys, uint64_t size);

/* Device registers: only through these (simple loads and stores, which a
 * hypervisor can emulate). The barriers order memory against a device:
 * write before a doorbell, read after its completion flag. */
DRIVER_WRAPPED uint8_t mmio_read8(const volatile void *address);
DRIVER_WRAPPED uint16_t mmio_read16(const volatile void *address);
DRIVER_WRAPPED uint32_t mmio_read32(const volatile void *address);
DRIVER_WRAPPED uint64_t mmio_read64(const volatile void *address);
DRIVER_WRAPPED void mmio_write8(volatile void *address, uint8_t value);
DRIVER_WRAPPED void mmio_write16(volatile void *address, uint16_t value);
DRIVER_WRAPPED void mmio_write32(volatile void *address, uint32_t value);
DRIVER_WRAPPED void mmio_write64(volatile void *address, uint64_t value);
DRIVER_WRAPPED void arch_write_barrier(void);
DRIVER_WRAPPED void arch_read_barrier(void);
DRIVER_WRAPPED void arch_cpu_relax(void);

/* PCI configuration space, capabilities (their offset, 0: none), DMA on
 * (memory and bus master, and the bridges above), PCIe power saving off
 * (each side's earlier state, -1: none), message interrupts (MSI-X entry
 * 0 or MSI) to handler(context): false if the device or machine cannot. */
uint8_t pci_read8(const struct pci_device *dev, uint16_t offset);
uint16_t pci_read16(const struct pci_device *dev, uint16_t offset);
uint32_t pci_read32(const struct pci_device *dev, uint16_t offset);
void pci_write16(const struct pci_device *dev, uint16_t offset, uint16_t value);
void pci_write32(const struct pci_device *dev, uint16_t offset, uint32_t value);
uint8_t pci_find_capability(const struct pci_device *dev, uint8_t id);
void pci_enable_dma(struct pci_device *dev);
void pci_aspm_off(struct pci_device *dev, int *was_dev, int *was_port);
bool pci_msi_enable(struct pci_device *dev, irq_handler_fn handler, void *context,
                    uint64_t *address, uint32_t *data);
bool pci_msix_enable(struct pci_device *dev, irq_handler_fn handler, void *context,
                     uint64_t *address, uint32_t *data, uint16_t *entries);

/* Time: the processor's counter (arch_counter_hz counts a second), the
 * timer's ticks (TIMER_HZ a second), sleeping (a thread, not an interrupt
 * handler), a function on every tick (interrupt context: be quick). */
uint64_t arch_counter(void);
uint64_t arch_counter_hz(void);
uint64_t timer_ticks(void);
void timer_sleep_ms(uint32_t ms);
bool timer_add_tick_handler(void (*fn)(void *context), void *context);

/* (1.2) A file from /system/firmware/ (name: "WIFI_RAM_CODE_MT7961_1.bin"),
 * whole, in the heap (free it with kfree), its size in *size; 0 if it is not
 * there or memory is short. A thread's call: it reads the disk. */
void *firmware_load(const char *name, uint64_t *size);

/* A kernel thread running fn(arg) (a driver's receive loop, its watcher). */
struct thread *thread_create(const char *name, void (*fn)(void *arg), void *arg);

/* (1.4) Is this thread asked to stop (its program is ending)? A wait on a
 * program's behalf gives up then. */
bool thread_stop_requested(void);

/* (1.4) The firmware's ACPI table with this signature ("VFCT", ...), or 0:
 * what the firmware tells about the machine, to be asked before assuming. */
const struct acpi_header *acpi_find_table(const char *signature);

/* (2.1) The firmware's method named method ("ATIF") under d's ACPI node
 * (the device whose _ADR, down the bridges, is d's place), called with
 * function and a buffer of in_length bytes (in 0: the integer 0), as an
 * interface's entry point such as AMD's ATIF takes them. The buffer it
 * returns goes into out (at most out_size bytes): its length, -1 when
 * there is no such method, -2 when it failed or gave no buffer. A
 * thread's call (the firmware's code runs under the interpreter's mutex). */
int acpi_pci_method(const struct pci_device *d, const char *method, uint64_t function,
                    const void *in, uint32_t in_length, void *out, uint32_t out_size);

/* Locks (struct spinlock, struct mutex above). spin_lock returns the
 * interrupt state spin_unlock puts back. */
DRIVER_WRAPPED uint64_t spin_lock(struct spinlock *lock);
DRIVER_WRAPPED void spin_unlock(struct spinlock *lock, uint64_t state);
void mutex_lock(struct mutex *m);
bool mutex_try_lock(struct mutex *m);
void mutex_unlock(struct mutex *m);

/* Waiting: a thread sleeps on a queue until its condition is true; the
 * code that makes it true (an interrupt handler too) wakes the queue.
 * Use the wait_event macros; their steps are below. */
DRIVER_WRAPPED uint64_t arch_irq_save(void);
DRIVER_WRAPPED void arch_irq_restore(uint64_t state);
void wait_prepare(struct wait_queue *q, uint64_t deadline);
void wait_cancel(void);
bool wait_commit(uint64_t deadline);
void wait_queue_wake_all(struct wait_queue *q);
void wait_queue_wake_one(struct wait_queue *q);

/* Sleep on q until cond is true. */
#define wait_event(q, cond) do {                                            \
        uint64_t state_ = arch_irq_save();                                  \
        while (!(cond)) {                                                   \
            wait_prepare((q), 0);                                           \
            if (cond) {                                                     \
                wait_cancel();                                              \
                break;                                                      \
            }                                                               \
            wait_commit(0);                                                 \
        }                                                                   \
        arch_irq_restore(state_);                                           \
    } while (0)

/* The same until a timer tick (timer_ticks() + ms * TIMER_HZ / 1000);
 * true if cond became true. */
#define wait_event_until(q, cond, deadline) ({                              \
        uint64_t state_ = arch_irq_save();                                  \
        bool ok_ = true;                                                    \
        while (!(cond)) {                                                   \
            wait_prepare((q), (deadline));                                  \
            if (cond) {                                                     \
                wait_cancel();                                              \
                break;                                                      \
            }                                                               \
            if (!wait_commit(deadline)) {                                   \
                ok_ = (cond);                                               \
                break;                                                      \
            }                                                               \
        }                                                                   \
        arch_irq_restore(state_);                                           \
        ok_;                                                                \
    })

#endif /* RITTEROS_KERNEL */
#endif
