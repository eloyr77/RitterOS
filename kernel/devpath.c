#include "devpath.h"
#include "kprintf.h"
#include "string.h"

/* Node types and subtypes (UEFI spec 2.10, chapter 10.3). */
#define TYPE_HARDWARE   0x01
#define TYPE_ACPI       0x02
#define TYPE_MESSAGING  0x03
#define TYPE_MEDIA      0x04
#define TYPE_END        0x7F

#define HW_PCI          0x01
#define ACPI_ACPI       0x01
#define MSG_USB         0x05
#define MSG_SATA        0x12
#define MSG_NVME        0x17
#define MEDIA_HARDDRIVE 0x01

#define PNP_PCI_ROOT    0x0A0341D0   /* EISA ID "PNP0A03" */
#define PNP_PCIE_ROOT   0x0A0841D0   /* EISA ID "PNP0A08" */

static uint32_t u32_at(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint64_t u64_at(const uint8_t *p) { uint64_t v; memcpy(&v, p, 8); return v; }

void guid_to_text(const uint8_t g[16], char out[37])
{
    /* The first three fields are stored little-endian, the rest as bytes. */
    ksnprintf(out, 37, "%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X",
              u32_at(g), (unsigned)(g[4] | g[5] << 8), (unsigned)(g[6] | g[7] << 8),
              g[8], g[9], g[10], g[11], g[12], g[13], g[14], g[15]);
}

/* Visit the nodes; stops at the end node or a malformed length. */
#define FOR_EACH_NODE(path, size, node, len)                                    \
    for (size_t off_ = 0, len = 0;                                              \
         off_ + 4 <= (size) &&                                                  \
         (node = (path) + off_, len = (size_t)(node[2] | node[3] << 8)) >= 4 && \
         off_ + len <= (size) && node[0] != TYPE_END;                           \
         off_ += len)

void devpath_to_text(const uint8_t *path, size_t size, char *out, size_t out_size)
{
    size_t n = 0;
    const uint8_t *node;
    out[0] = 0;

    FOR_EACH_NODE(path, size, node, len) {
        uint8_t type = node[0], sub = node[1];
        char part[128];

        if (type == TYPE_ACPI && sub == ACPI_ACPI && len >= 12) {
            uint32_t hid = u32_at(node + 4), uid = u32_at(node + 8);
            if (hid == PNP_PCI_ROOT || hid == PNP_PCIE_ROOT)
                ksnprintf(part, sizeof(part), "%s(0x%x)",
                          hid == PNP_PCI_ROOT ? "PciRoot" : "PcieRoot", uid);
            else
                ksnprintf(part, sizeof(part), "Acpi(0x%x,0x%x)", hid, uid);
        } else if (type == TYPE_HARDWARE && sub == HW_PCI && len >= 6) {
            ksnprintf(part, sizeof(part), "Pci(0x%x,0x%x)", node[5], node[4]);  /* device, function */
        } else if (type == TYPE_MESSAGING && sub == MSG_NVME && len >= 16) {
            const uint8_t *e = node + 8;
            ksnprintf(part, sizeof(part), "NVMe(0x%x,%02X-%02X-%02X-%02X-%02X-%02X-%02X-%02X)",
                      u32_at(node + 4), e[0], e[1], e[2], e[3], e[4], e[5], e[6], e[7]);
        } else if (type == TYPE_MESSAGING && sub == MSG_SATA && len >= 10) {
            ksnprintf(part, sizeof(part), "Sata(0x%x,0x%x,0x%x)",
                      (unsigned)(node[4] | node[5] << 8), (unsigned)(node[6] | node[7] << 8),
                      (unsigned)(node[8] | node[9] << 8));
        } else if (type == TYPE_MESSAGING && sub == MSG_USB && len >= 6) {
            ksnprintf(part, sizeof(part), "USB(0x%x,0x%x)", node[4], node[5]);
        } else if (type == TYPE_MEDIA && sub == MEDIA_HARDDRIVE && len >= 42) {
            char guid[37];
            if (node[41] == 2) {
                guid_to_text(node + 24, guid);
                ksnprintf(part, sizeof(part), "HD(%u,GPT,%s,0x%lx,0x%lx)", u32_at(node + 4),
                          guid, u64_at(node + 8), u64_at(node + 16));
            } else {
                ksnprintf(part, sizeof(part), "HD(%u,MBR,0x%08X,0x%lx,0x%lx)", u32_at(node + 4),
                          u32_at(node + 24), u64_at(node + 8), u64_at(node + 16));
            }
        } else {
            ksnprintf(part, sizeof(part), "Path(%u,%u)", type, sub);
        }

        n += (size_t)ksnprintf(out + n, n < out_size ? out_size - n : 0, "%s%s",
                               n ? "/" : "", part);
        if (n >= out_size)
            return;
    }
}

bool devpath_partition(const uint8_t *path, size_t size, struct devpath_partition *p)
{
    const uint8_t *node;
    FOR_EACH_NODE(path, size, node, len) {
        if (node[0] == TYPE_MEDIA && node[1] == MEDIA_HARDDRIVE && len >= 42) {
            p->number = u32_at(node + 4);
            p->start_lba = u64_at(node + 8);
            p->sector_count = u64_at(node + 16);
            memcpy(p->guid, node + 24, 16);
            p->is_gpt = node[41] == 2;
            return true;
        }
    }
    return false;
}

int devpath_pci_nodes(const uint8_t *path, size_t size, uint8_t *devices, uint8_t *functions,
                      int max)
{
    const uint8_t *node;
    int n = 0;
    FOR_EACH_NODE(path, size, node, len) {
        if (node[0] == TYPE_HARDWARE && node[1] == HW_PCI && len >= 6 && n < max) {
            functions[n] = node[4];
            devices[n] = node[5];
            n++;
        }
    }
    return n;
}
