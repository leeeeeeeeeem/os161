#include "types.h"
#include "lib.h"
#include "vm.h"
#include "coremap.h"

struct coremap_entry* coremap;
uint32_t total_frames;


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


vaddr_t coremap_alloc(unsigned npages){
	if (npages > 255) // chunk size defined as uint8
		return 0; 

	uint32_t first_page = 0;
	uint8_t pages_found = 0;
	for (uint32_t i = 0; i < total_frames; i++){
		switch (coremap[i].occupancy_state) {
			case FIXED:
				pages_found = 0; // this should be useless but here as a safeguard
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
	}
}
