#include "swap.h"
#include "types.h"
#include "spinlock.h"
#include <vmstats.h>
#include "lib.h"
#include "vm.h"
#include "coremap.h"
#include "pagetable.h"
#include "spl.h"
#include "mips/tlb.h"
#include "wchan.h"

struct coremap_entry* coremap;
uint32_t total_frames;
struct wchan *eviction_wchan = NULL;

extern struct spinlock mem_lock;

uint64_t fifo_alloc_counter = 0;

void coremap_init(void){
	vaddr_t size = ram_getsize();

	total_frames = size / PAGE_SIZE;
	unsigned long total_bytes = total_frames * sizeof(struct coremap_entry);
	unsigned long number_of_pages = DIVROUNDUP(total_bytes, PAGE_SIZE);

	paddr_t coremap_paddr = ram_stealmem(number_of_pages);
	coremap = (struct coremap_entry*) PADDR_TO_KVADDR(coremap_paddr);

	paddr_t boundary = ram_getfirstfree();

	for (uint32_t i = 0; i < total_frames; i++){
		paddr_t page_addr = i * PAGE_SIZE;
		coremap[i].owner = NULL;
		coremap[i].vaddr = 0;
		coremap[i].counter = 0;
		if (page_addr < boundary){
			coremap[i].occupancy_state = FIXED;
			coremap[i].chunk_size = 0;
		}
		else {
			coremap[i].occupancy_state = FREE;
			coremap[i].chunk_size = 0;
		}
	}

	eviction_wchan = wchan_create("eviction");
	KASSERT(eviction_wchan != NULL);
}

vaddr_t coremap_alloc(unsigned npages){
	if (npages > 255)
		return 0; 

	uint32_t first_page = 0;
	uint8_t pages_found = 0;

retry:
	pages_found = 0;
	for (uint32_t i = 0; i < total_frames; i++){
		switch (coremap[i].occupancy_state) {
			case FIXED:
				pages_found = 0; //dovrebbe essere inutile ma qui per sicurezza
				break;
			case FREE: 
				if (pages_found == 0)
					first_page = i;

				pages_found++;

				if (pages_found == npages)
					goto allocate;

				break;
			case IN_USE:
				pages_found = 0;
				break;
		}
	}

	if (npages == 1) {
		paddr_t evicted_paddr = coremap_evict_one();
		if (evicted_paddr != 0) {
			goto retry;
		}
	}

	return 0;

allocate:
	coremap[first_page].chunk_size = npages;

	for (uint32_t i = first_page; i < first_page + npages; i++){
		coremap[i].occupancy_state = IN_USE;
	}

	return (vaddr_t) PADDR_TO_KVADDR(first_page * PAGE_SIZE);
}


void coremap_free(vaddr_t addr){
	uint32_t paddr = KVADDR_TO_PADDR(addr);
	uint32_t first_page = paddr / PAGE_SIZE;

	KASSERT(first_page < total_frames);
	uint8_t chunk_size = coremap[first_page].chunk_size;
	KASSERT(chunk_size != 0);
	uint32_t last_page = first_page + chunk_size;
	KASSERT(last_page <= total_frames);

	for (uint32_t i = first_page; i < last_page; i++){
		KASSERT(coremap[i].occupancy_state == IN_USE);

		coremap[i].occupancy_state = FREE;
		coremap[i].chunk_size = 0;
		coremap[i].owner = NULL;
		coremap[i].vaddr = 0;
		coremap[i].counter = 0;
	}
}

void coremap_set_owner(uint32_t pframe, struct addrspace *as, vaddr_t vaddr) {
	spinlock_acquire(&mem_lock);
	KASSERT(pframe < total_frames);
	KASSERT(coremap[pframe].occupancy_state == IN_USE);
	coremap[pframe].owner = as;
	coremap[pframe].vaddr = vaddr & PAGE_FRAME;
	coremap[pframe].counter = fifo_alloc_counter++;
	spinlock_release(&mem_lock);
}

paddr_t coremap_evict_one(void) {
	uint64_t min_counter = 0xFFFFFFFFFFFFFFFF;
	uint32_t selected_frame = 0;
	bool found = false;
	for (uint32_t i = 0; i < total_frames; i++) {
		if (coremap[i].occupancy_state == IN_USE && coremap[i].owner != NULL && !coremap[i].owner->is_copying) {
			if (coremap[i].counter < min_counter) {
				min_counter = coremap[i].counter;
				selected_frame = i;
				found = true;
			}
		}
	}
	if (!found) return 0;

	coremap[selected_frame].occupancy_state = FIXED;

	vaddr_t vaddr = coremap[selected_frame].vaddr;
	struct pagedir* pt = coremap[selected_frame].owner->pagetable;

	spinlock_release(&mem_lock);

	unsigned int swap_slot;

	int err = swap_alloc(&swap_slot);
	if (err) {
		spinlock_acquire(&mem_lock);
		coremap[selected_frame].occupancy_state = IN_USE;
		wchan_wakeall(eviction_wchan, &mem_lock);
		return 0;
	}

	err = swap_write(PADDR_TO_KVADDR(selected_frame << 12), swap_slot);
	if (err) {
		swap_free(swap_slot);
		spinlock_acquire(&mem_lock);
		coremap[selected_frame].occupancy_state = IN_USE;
		wchan_wakeall(eviction_wchan, &mem_lock);
		return 0;
	}

	spinlock_acquire(&mem_lock);

	paddr_t* pt_entry = pagetable_get_entry(pt, vaddr);
	*pt_entry = (swap_slot << 12) | PTE_SWAPPED;

	int spl = splhigh();
	int index = tlb_probe(vaddr & PAGE_FRAME, 0);
	if (index >= 0) {
		tlb_write(TLBHI_INVALID(index), TLBLO_INVALID(), index);
		vm_record_stat(STAT_TLB_INVALIDATION);
	}
	splx(spl);

	coremap[selected_frame].occupancy_state = FREE;
	coremap[selected_frame].owner = NULL;
	coremap[selected_frame].vaddr = 0;
	coremap[selected_frame].counter = 0;
	coremap[selected_frame].chunk_size = 0;

	wchan_wakeall(eviction_wchan, &mem_lock);
	return selected_frame << 12;
}
