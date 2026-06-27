#ifndef PAGETABLE_H
#define PAGETABLE_H

#include "types.h"
#include "lib.h"

#define PT_SIZE 1024

#define GET_DIR_INDEX(addr) ((addr >> 22) & 0x000003FF)
#define GET_PT_INDEX(addr) ((addr >> 12) & 0x000003FF)

#define PTE_PRESENT 0x1
#define PTE_SWAPPED 0x2

struct addrspace;

// 2 livello
struct pagetable {
	paddr_t entries[PT_SIZE];
}; 

// 1 livello
struct pagedir {
	struct pagetable* tables[PT_SIZE];
};

struct pagedir* pagetable_create(void);
struct pagetable* pagetable_create_lv2(void);
void pagetable_destroy(struct pagedir* pt);
paddr_t pagetable_translate(struct addrspace* as, vaddr_t vaddr);
struct pagedir* pagetable_copy(struct addrspace *old, struct addrspace *newas);
paddr_t *pagetable_get_entry(struct pagedir *pt, vaddr_t vaddr);

#endif
