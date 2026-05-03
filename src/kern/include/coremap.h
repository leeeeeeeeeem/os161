#ifndef COREMAP_H
#define COREMAP_H

#include "vm.h"

typedef enum {
	FREE,
	IN_USE,
	FIXED
} occupancy_state;

typedef struct {
	occupancy_state occupancy_state;
	uint8_t chunk_size;
} coremap_entry_t ;

void coremap_init(void);

#endif
