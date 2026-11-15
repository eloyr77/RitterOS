/*
 * sys/sysinfo.h (43a-3): sysinfo: the time since the start (seconds),
 * the memory (totalram, freeram in units of mem_unit bytes; no swap or
 * load figures: 0); get_nprocs_conf/get_nprocs: the cores the firmware
 * lists, and those started; get_phys_pages/get_avphys_pages: memory and
 * free memory in pages.
 */
#ifndef RITTEROS_SYS_SYSINFO_H
#define RITTEROS_SYS_SYSINFO_H

#ifdef __cplusplus
extern "C" {
#endif

struct sysinfo {
    long uptime;
    unsigned long loads[3];
    unsigned long totalram, freeram, sharedram, bufferram, totalswap, freeswap;
    unsigned short procs, pad;
    unsigned long totalhigh, freehigh;
    unsigned int mem_unit;
    char reserved[256];
};
int sysinfo(struct sysinfo *info);
int get_nprocs_conf(void);
int get_nprocs(void);
long get_phys_pages(void);
long get_avphys_pages(void);

#ifdef __cplusplus
}
#endif

#endif
