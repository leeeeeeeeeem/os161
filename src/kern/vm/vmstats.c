#include <vmstats.h>
#include <types.h>
#include <spinlock.h>
#include <lib.h>

uint64_t tlb_faults = 0;
uint64_t tlb_faults_free = 0;
uint64_t tlb_faults_replace = 0;
uint64_t tlb_invalidations = 0;
uint64_t page_faults = 0;
uint64_t swap_reads = 0;       
uint64_t swap_writes = 0;      
uint64_t page_replacements = 0;

static struct spinlock stats_lock = SPINLOCK_INITIALIZER;
static bool recording = false;

void vm_start_recording_stats(void) {
	spinlock_acquire(&stats_lock);
	recording = true;
	spinlock_release(&stats_lock);
}

void vm_stop_recording_stats(void) {
	spinlock_acquire(&stats_lock);
	recording = false;
	spinlock_release(&stats_lock);
}

void vm_record_stat(int stat_type) {
	spinlock_acquire(&stats_lock);
	if (!recording) {
		spinlock_release(&stats_lock);
		return;
	}
	switch (stat_type) {
		case STAT_TLB_FAULT:
			tlb_faults++;
			break;
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
		case STAT_PAGE_REPLACEMENT:
			page_replacements++;
			break;
	}
	spinlock_release(&stats_lock);
}

void vm_print_stats(void) {
	spinlock_acquire(&stats_lock);
	kprintf("VM Statistics:\n");
	kprintf("\tTLB faults (total): %llu\n", tlb_faults);
	kprintf("\tTLB faults (free): %llu\n", tlb_faults_free);
	kprintf("\tTLB faults (replace): %llu\n", tlb_faults_replace);
	kprintf("\tTLB Invalidations: %llu\n", tlb_invalidations);
	kprintf("\tPage faults: %llu\n", page_faults);
	kprintf("\tSwap reads: %llu\n", swap_reads);
	kprintf("\tSwap writes: %llu\n", swap_writes);
	kprintf("\tPage replacements: %llu\n", page_replacements);
	spinlock_release(&stats_lock);
}

void vm_reset_stats(void) {
	spinlock_acquire(&stats_lock);
	tlb_faults = 0;
	tlb_faults_free = 0;
	tlb_faults_replace = 0;
	tlb_invalidations = 0;
	page_faults = 0;
	swap_reads = 0;       
	swap_writes = 0;      
	page_replacements = 0;
	spinlock_release(&stats_lock);
}
