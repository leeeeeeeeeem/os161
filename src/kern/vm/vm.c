#include "vm.h"
#include "coremap.h"
#include "types.h"
#include "spinlock.h"

bool vm_ready = false;
static struct spinlock mem_lock = SPINLOCK_INITIALIZER;

void vm_bootstrap(void){
	coremap_init();
	vm_ready = true;
}

