#ifndef PAGETABLE_H
#define PAGETABLE_H

#include "types.h"
#include "lib.h"

#define PT_SIZE 1024

#define GET_DIR_INDEX(addr) ((addr >> 22) & 0x000003FF)
#define GET_PT_INDEX(addr) ((addr >> 12) & 0x000003FF)

struct pagetable {
	paddr_t entries[PT_SIZE];
}; 

struct pagedir {
	struct pagetable* tables[PT_SIZE];
};

struct pagedir* pagetable_create(void);
struct pagetable* pagetable_create_lv2(void);
void pagetable_destroy(struct pagedir* pt);
paddr_t pagetable_translate(struct pagedir* pt, vaddr_t vaddr);

#endif
