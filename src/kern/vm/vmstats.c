#include <vmstats.h>
#include <types.h>
#include <spinlock.h>
#include <lib.h>

uint64_t tlb_faults_free = 0;
uint64_t tlb_faults_replace = 0;
uint64_t tlb_invalidations = 0;
uint64_t page_faults = 0;
uint64_t swap_reads = 0;       
uint64_t swap_writes = 0;

static struct spinlock stats_lock = SPINLOCK_INITIALIZER;
static bool recording = false;

void vm_start_recording_stats(void) {
	spinlock_acquire(&stats_lock);
	if (recording) {
		kprintf("VM statistics are already being recorded\n");
		spinlock_release(&stats_lock);
		return;
	}
	recording = true;
	spinlock_release(&stats_lock);
	kprintf("started recording VM statistics\n");
}

void vm_stop_recording_stats(void) {
	spinlock_acquire(&stats_lock);
	if (!recording) {
		kprintf("start recording statistics with vmstats --start before trying to stop recording them\n");
		spinlock_release(&stats_lock);
		return;
	}
	recording = false;
	spinlock_release(&stats_lock);
	kprintf("stopped recording VM statistics\n");
}

void vm_record_stat(int stat_type) {
	spinlock_acquire(&stats_lock);
	if (!recording) {
		spinlock_release(&stats_lock);
		return;
	}
	switch (stat_type) {
		case STAT_TLB_FAULT_FREE:
			tlb_faults_free++;
			break;
		case STAT_TLB_FAULT_REPLACE:
			tlb_faults_replace++;
			break;
		case STAT_TLB_INVALIDATION:
			tlb_invalidations++;
			break;
		case STAT_PAGE_FAULT:
			page_faults++;
			break;
		case STAT_SWAP_READ:
			swap_reads++;
			break;
		case STAT_SWAP_WRITE:
			swap_writes++;
			break;
	}
	spinlock_release(&stats_lock);
}

void vm_print_stats(void) {
	spinlock_acquire(&stats_lock);
	if (!recording) {
		kprintf("start recording statistics with vmstats --start before trying to print them\n");
		spinlock_release(&stats_lock);
		return;
	}
	kprintf("VM Statistics:\n");
	kprintf("\tTLB fault (free): %llu\n", tlb_faults_free);
	kprintf("\tTLB fault (replace): %llu\n", tlb_faults_replace);
	kprintf("\tTLB Invalidation: %llu\n", tlb_invalidations);
	kprintf("\tpage fault: %llu\n", page_faults);
	kprintf("\tswap read (page in): %llu\n", swap_reads);
	kprintf("\tswap write (page out): %llu\n", swap_writes);
	spinlock_release(&stats_lock);
}

void vm_reset_stats(void) {
	spinlock_acquire(&stats_lock);
	if (!recording) {
		kprintf("start recording statistics with vmstats --start before trying to reset them\n");
		spinlock_release(&stats_lock);
		return;
	}

	tlb_faults_free = 0;
	tlb_faults_replace = 0;
	tlb_invalidations = 0;
	page_faults = 0;
	swap_reads = 0;       
	swap_writes = 0;      
	spinlock_release(&stats_lock);
	kprintf("all VM statistics have been reset\n");
}
