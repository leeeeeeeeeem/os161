#ifndef VMSTATS_H
#define VMSTATS_H

#include <types.h>

#define STAT_TLB_FAULT_FREE      1
#define STAT_TLB_FAULT_REPLACE   2
#define STAT_TLB_INVALIDATION    3
#define STAT_PAGE_FAULT          4
#define STAT_SWAP_READ           5
#define STAT_SWAP_WRITE          6

void vm_start_recording_stats(void);
void vm_stop_recording_stats(void);
void vm_record_stat(int stat_type);
void vm_print_stats(void);
void vm_reset_stats(void);

#endif 
