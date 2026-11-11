/*
 * ACPI (Advanced Configuration and Power Interface) tables: the firmware's
 * description of the machine. The RSDP points to the XSDT (or the older
 * RSDT), a list of tables identified by 4-character signatures, e.g.
 *   FACP  fixed hardware: power management registers (power-off, reset)
 *   APIC  "MADT": CPU cores and interrupt controllers
 *   MCFG  where PCI Express configuration space is mapped
 *   HPET  high precision event timer
 */
#ifndef RITTEROS_ACPI_H
#define RITTEROS_ACPI_H

#include <stdbool.h>
#include <stdint.h>
#include "driver.h"

/* Common header of every ACPI table. */
/* struct acpi_header (driver.h, 67f: the drivers' too): signature,
 * length (header included), revision, checksum, OEM and creator. */

/* Generic address structure: where a register is (I/O port or memory). */
struct acpi_gas {
    uint8_t space;             /* 0: memory, 1: I/O port */
    uint8_t bit_width, bit_offset, access_size;
    uint64_t address;
} __attribute__((packed));

#define ACPI_SPACE_MEMORY 0
#define ACPI_SPACE_IO     1

/* The FADT (signature "FACP"), as far as RitterOS uses it. Fields past the
 * table's length (older versions) must be treated as 0. */
struct acpi_fadt {
    struct acpi_header header;
    uint32_t firmware_ctrl;
    uint32_t dsdt;             /* 32-bit address of the DSDT */
    uint8_t reserved;
    uint8_t preferred_pm_profile;
    uint16_t sci_interrupt;
    uint32_t smi_command;      /* port for switching to ACPI mode */
    uint8_t acpi_enable, acpi_disable, s4bios_request, pstate_control;
    uint32_t pm1a_event_block, pm1b_event_block;
    uint32_t pm1a_control_block, pm1b_control_block;   /* I/O ports: sleep states */
    uint32_t pm2_control_block, pm_timer_block, gpe0_block, gpe1_block;
    uint8_t pm1_event_length, pm1_control_length, pm2_control_length, pm_timer_length;
    uint8_t gpe0_length, gpe1_length, gpe1_base, cstate_control;
    uint16_t c2_latency, c3_latency, flush_size, flush_stride;
    uint8_t duty_offset, duty_width, day_alarm, month_alarm, century;
    uint16_t iapc_boot_arch;
    uint8_t reserved2;
    uint32_t flags;
    struct acpi_gas reset_register;
    uint8_t reset_value;
    uint16_t arm_boot_arch;    /* bit 0: PSCI implemented, bit 1: call it with HVC */
    uint8_t minor_version;
    uint64_t x_firmware_ctrl;
    uint64_t x_dsdt;           /* 64-bit address of the DSDT (preferred) */
} __attribute__((packed));

#define FADT_RESET_REGISTER  (1u << 10)    /* flags: reset_register is valid */
#define FADT_HARDWARE_REDUCED (1u << 20)   /* flags: no PM1 registers (e.g. ARM) */
#define FADT_LOW_POWER_S0    (1u << 21)    /* (37a) flags: low-power S0 idle is better than S3 */
#define FADT_PROC_C1         (1u << 2)     /* C1 on every processor */
#define FADT_P_LVL2_UP       (1u << 3)     /* C2 works with several processors */
#define FADT_PWR_BUTTON      (1u << 4)     /* the power button is a device (else fixed) */
#define FADT_SLP_BUTTON      (1u << 5)     /* the sleep button is a device, or none */
#define FADT_ARM_PSCI        (1u << 0)
#define FADT_ARM_PSCI_HVC    (1u << 1)

/* The FADT, or 0. */
const struct acpi_fadt *acpi_fadt(void);

/* (37a) What the firmware offers for power management, read without running
 * AML: the FADT's flags and registers, the sleep states \_S0_-\_S5_, the
 * FACS, the power devices by ID, where power methods are defined (power's
 * report, REPORT_POWER). */
void acpi_power_print(void);

/* (37b-1) Table number i as the firmware gave it: the DSDT, the FACS, then
 * the XSDT's tables in order; 0 past the last. Its length is the header's
 * (the FACS's too: its first 8 bytes are a signature and a length). */
const struct acpi_header *acpi_table_at(int i);

/* (37b-2) The firmware's AML (the DSDT, then every SSDT) run into the
 * interpreter's namespace (kernel/aml.c); (37b-4) its operation regions
 * reach the machine (memory but the kernel's RAM, from info's map; I/O
 * ports; PCI configuration), then the boot's _INI and _REG run as Linux
 * runs them. Logged in two lines. After pci_init. */
struct boot_info;
void acpi_aml_load(const struct boot_info *info);

/* (37b-2) Every object of the namespace as a line (power names), given to
 * out with ctx, the interpreter held meanwhile. */
void acpi_aml_names(void (*out)(const char *line, void *ctx), void *ctx);

/* (37b-3) The object at path ("\\_SB.PCI0._CRS", from the root) evaluated
 * (a method run without arguments, a field read) and its value given to
 * out as lines (power eval): 0, -1 when there is no such object, -2 when
 * the evaluation failed (the reason in the log). */
int acpi_aml_eval(const char *path, void (*out)(const char *line, void *ctx), void *ctx);

/* (37c-2b) For the events (kernel/acpi_events.c), each under the
 * interpreter's mutex: for GPEs 0..count-1 their method's kind ('L' a
 * level-triggered \_GPE._Lxx, 'E' an edge's _Exx, 0 none) and whether a
 * present device's _PRW names it (for waking only); one's method run
 * (false: it failed or is not there); the GED devices' (ACPI0013)
 * interrupts and one's _EVT(gsi) run; the power button devices' paths
 * (PNP0C0C, whose Notify(0x80) is a press: acpi_events_button). */
struct aml_node;
struct acpi_ged_line {
    struct aml_node *device;
    uint32_t gsi;
    bool level, active_low;
};
void acpi_aml_gpes(uint8_t *kind, bool *wake, unsigned count);
bool acpi_aml_gpe_run(unsigned gpe, char kind);
int acpi_aml_ged_lines(struct acpi_ged_line *out, int max);
bool acpi_aml_ged_run(struct aml_node *device, uint32_t gsi);
void acpi_aml_buttons(char *paths, size_t size);

/* (37c-2c) The embedded controller's waiting events run (ec_run_queries:
 * how many); the first lid's _LID and the first AC adapter's _PSR (1 open
 * or plugged in, 0 not, -1 none or not read), its path into path. */
int acpi_aml_ec_queries(void);
int acpi_aml_lid(char *path, size_t size);
int acpi_aml_ac(char *path, size_t size);

/* (37d) The processors' sleep states from their _CST (C1-C3; the register
 * as its generic register descriptor gives it: space 0x7F "functional
 * fixed hardware" (bit_offset 2: MWAIT with the address as its hint, 1:
 * HLT), 1 an I/O port read; latency in us): the first processor's, up to
 * max, how many; *processors how many have one, *alike whether all the
 * same. */
struct acpi_cstate {
    uint8_t type, space, bit_width, bit_offset, access;
    uint64_t address;
    uint32_t latency_us, power_mw;
};
int acpi_aml_cstates(struct acpi_cstate *out, int max, int *processors, bool *alike);

/* The firmware's ACPI Control Structure (ACPI 6.5 5.2.10), found through
 * the FADT: the waking vector for sleep states S1-S3 lives here. */
struct acpi_facs {
    char signature[4];                  /* "FACS" */
    uint32_t length;
    uint32_t hardware_signature;        /* changes when the hardware does (S4) */
    uint32_t waking_vector;             /* real-mode address the firmware jumps to */
    uint32_t global_lock;
    uint32_t flags;                     /* bit 0 S4BIOS, bit 1 64-bit wake supported */
    uint64_t x_waking_vector;
    uint8_t version;
} __attribute__((packed));

/* (37g-1) Sleep state x's values for PM1a and PM1b (\_Sx_ run, else found
 * in the tables' bytes), the FACS (the waking vector's place; 0 if none),
 * and \_PTS or \_WAK run with the state (1 ran, 0 none, -1 failed). */
bool acpi_sleep_type(int x, uint8_t *a, uint8_t *b);
struct acpi_facs *acpi_facs(void);
int acpi_aml_sleep_method(const char *path, uint64_t state);

/* (37d-2) Those states to the idle thread (arch_idle_setup); logs them. */
void acpi_idle_start(void);

/* (37e-2a) A driver's call of the method named method under d's ACPI node
 * (driver.h). */
int acpi_pci_method(const struct pci_device *d, const char *method, uint64_t function,
                    const void *in, uint32_t in_length, void *out, uint32_t out_size);

/* The sleep type values for state S5 ("soft off") from the DSDT's \_S5_
 * object, for PM1a and PM1b. A tiny pattern match instead of an AML
 * interpreter: _S5_ is a package of constants in practice. */
bool acpi_s5_sleep_type(uint8_t *pm1a, uint8_t *pm1b);

/* Validate the RSDP and the table list at the physical address rsdp_address
 * and log the tables. Returns the number of valid tables, or 0 if ACPI is
 * unusable. */
int acpi_init(uint64_t rsdp_address);

/* The first table with this signature (e.g. "APIC"), or 0. The pointer is a
 * direct-map address. */
const struct acpi_header *acpi_find_table(const char *signature);

/* The device IDs in the DSDT and SSDTs that name touchpads and their buses
 * (HID over I2C, ELAN, Synaptics, the AMD and Intel I2C controllers), one
 * line on the console if there are any: what a touchpad driver would need
 * (15r). Only strings are looked for; the AML is not run. */
void acpi_report_input_devices(void);

/* A Device in the DSDT or an SSDT, read without running AML (15s): its last
 * name segment and its body. What a driver needs from it is looked for as
 * written there: resource descriptors in buffers (Memory32Fixed,
 * I2cSerialBus, GpioInt) and packages of constants. A value the firmware
 * computes in a method (a patched address) is not seen; drivers confirm
 * what they find on the device itself. */
struct acpi_device {
    char name[5];                   /* "I2CD", "TPAD" */
    const uint8_t *start, *end;     /* its body (children included) */
};

/* The devices whose body names id ("PNP0C50", "AMDI0010"; a PNP id also as
 * an EisaId), innermost: up to max into out; how many. */
int acpi_devices_with(const char *id, struct acpi_device *out, int max);
/* The device named seg (the last segment of a path like \_SB.I2CD). */
bool acpi_device_named(const char *seg, struct acpi_device *out);
/* Its first Memory32Fixed range. */
bool acpi_device_memory(const struct acpi_device *d, uint64_t *base, uint32_t *length);
/* Its first I2cSerialBus: the controller's last name segment, the address
 * (0: patched at run time) and the speed in Hz. */
bool acpi_device_i2c(const struct acpi_device *d, char bus[5], uint16_t *address, uint32_t *speed);
/* Its first Extended Interrupt: the GSI, and whether level-triggered and
 * active low. */
bool acpi_device_interrupt(const struct acpi_device *d, uint32_t *gsi, bool *level, bool *active_low);
/* Its first GpioInt: the controller's last name segment and the pin. */
bool acpi_device_gpio_int(const struct acpi_device *d, char controller[5], uint16_t *pin);
/* HID over I2C: the (address, descriptor register) pairs in its packages
 * of the form { id, address, register, "HID", "PNP0C50" } (a firmware
 * choosing among models), up to max; how many. */
int acpi_device_i2c_hid_pairs(const struct acpi_device *d, uint16_t *address, uint16_t *reg,
                              int max);

#endif
