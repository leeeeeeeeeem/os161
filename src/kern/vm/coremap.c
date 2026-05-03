#include "lib.h"
#include "types.h"
#include "vm.h"
#include "coremap.h"

coremap_entry_t* coremap;
bool coremap_ready = false;

void coremap_init(void){
	vaddr_t size = ram_getsize();

	unsigned long total_frames = size / PAGE_SIZE;
	unsigned long total_bytes = total_frames * sizeof(coremap_entry_t);
	unsigned long number_of_pages = DIVROUNDUP(total_bytes, PAGE_SIZE);

	paddr_t coremap_paddr = ram_stealmem(number_of_pages);
	coremap = (coremap_entry_t*) PADDR_TO_KVADDR(coremap_paddr);

	paddr_t boundary = coremap_paddr + (number_of_pages * PAGE_SIZE);

	for (int i; i < (int) total_frames; i++){
		paddr_t page_addr = i * PAGE_SIZE;
		if (page_addr < boundary){
			coremap[i].occupancy_state = FIXED;
			coremap[i].chunk_size = 0;
		}
		else {
			coremap[i].occupancy_state = FREE;
			coremap[i].chunk_size = 0;
		}
	}
}
