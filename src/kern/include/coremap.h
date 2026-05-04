#ifndef COREMAP_H
#define COREMAP_H

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
};

void coremap_init(void);
vaddr_t coremap_alloc(unsigned npages);
void coremap_free(vaddr_t addr);

#endif
