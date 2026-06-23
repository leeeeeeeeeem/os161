#ifndef COREMAP_H
#define COREMAP_H

#include "addrspace.h"
#include "types.h"
#include "lib.h"

enum occupancy_state {
	FREE,
	IN_USE,
	FIXED
};

struct coremap_entry {
	enum occupancy_state occupancy_state;
	uint8_t chunk_size;
	struct addrspace* owner;
	vaddr_t vaddr;
	uint64_t counter;
};

void coremap_init(void);
vaddr_t coremap_alloc(unsigned npages);
void coremap_free(vaddr_t addr);
void coremap_set_owner(uint32_t pframe, struct addrspace *as, vaddr_t vaddr);
paddr_t coremap_evict_one(void);

#endif
