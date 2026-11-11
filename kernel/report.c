#include "report.h"
#include "acpi.h"
#include "cpus.h"
#include "devices.h"
#include "gpu.h"
#include "kprintf.h"
#include "mmap.h"
#include "module.h"
#include "net.h"
#include "pci.h"
#include "process.h"
#include "sched.h"
#include "swap.h"
#include "syscalls.h"
#include "usb.h"

bool report_print(int which)
{
    switch (which) {
    case REPORT_PROCESSES:
        process_print_all();
        return true;
    case REPORT_THREADS:
        sched_print_threads();
        return true;
    case REPORT_PCI:
        kprintf("%d PCI devices:\n", pci_device_count());
        for (int i = 0; i < pci_device_count(); i++)
            pci_print_device(pci_device_at(i));
        return true;
    case REPORT_USB:
        usb_print_devices();
        return true;
    case REPORT_DISKS:
        devices_print();
        return true;
    case REPORT_NET:
        net_print();
        return true;
    case REPORT_MEMORY:
        mmap_print_cache();
        swap_print();                       /* (35a) */
        return true;
    case REPORT_CPUS:
        cpus_print();
        return true;
    case REPORT_DISPLAYS:
        gpu_print();
        return true;
    case REPORT_DRIVERS:
        modules_print();
        return true;
    case REPORT_POWER:
        acpi_power_print();                 /* (37a) */
        return true;
    case REPORT_DISPLAY_SLEEP:
        gpu_print_sleep();                  /* (37g-5c-2) */
        return true;
    default:
        return false;
    }
}

long report_text(int which, char *buf, size_t size)
{
    struct kprintf_capture capture = { buf, size, 0 };
    kprintf_capture(&capture);
    bool ok = report_print(which);
    kprintf_capture(0);
    return ok ? (long)capture.used : -1;
}
