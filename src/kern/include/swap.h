#ifndef SWAP_H
#define SWAP_H

#include <types.h>

void swap_bootstrap(void);
int swap_alloc(unsigned *ret_slot);
void swap_free(unsigned slot);
int swap_write(vaddr_t kvaddr, unsigned slot);
int swap_read(vaddr_t kvaddr, unsigned slot);

#endif
