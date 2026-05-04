#ifndef PAGETABLE_H
#define PAGETABLE_H

#include "types.h"
#include "lib.h"

#define PT_SIZE 1024

struct page_table {
	paddr_t entries[PT_SIZE];
}; 

struct pagedir {
	struct page_table* tables[PT_SIZE];
};

#endif
