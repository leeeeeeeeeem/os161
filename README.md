# Virtual Memory Implementation on OS161

## User Program Execution
To run the VM tests, it is necessary to be able to run user programs in addition to kernel tests; to do this, we modified the `runprogram.c` file, containing the function of the same name. We implemented support for passing command-line arguments (`argc` and `argv`) from the kernel to the user program before transitioning to user mode. To do this, the function:

- defines the user address space stack by calling `as_define_stack` and saves a pointer to its first address;

- temporarily allocates a pointer array in the kernel (with `kmalloc`) to store the addresses of the strings representing the parameters;

- copies strings from the kernel to the user stack using `copyoutstr`, decrementing the stack pointer each time, and saves the virtual addresses into the previously defined array `argv_ptrs`;

- aligns the stack pointer to 8 bytes;

- copies the `argv_ptrs` array onto the user stack using the `copyout` function, providing the user process with an accessible array of strings;

- frees `argv_ptrs` from kernel memory, performs a further alignment of the stack, and adds 16 bytes of space for the stack frame;

- calls `enter_new_process`, passing the number of arguments, the stack pointer, and the program's entry point.

## System Calls and Process Management
We identified the need to implement additional support for process control so that the menu process could wait for user process termination to correctly compute and return test execution times. Furthermore, it was necessary to test our VM implementation concurrently to verify its operation using tests such as `forktest` and `parallelvm`.

### Process Table and New Fields in `struct proc`
First of all, we modified `proc.c` and `proc.h` to improve process management. We modified the `struct proc` data structure, adding variables to signal whether the process has terminated and whether the parent has called wait, the exit code, a semaphore for synchronization, a pointer to the parent process, and finally the PID, which is a unique process identifier.
Inside `proc.c`, we added a Process Table: a global data structure that stores all currently active processes via an array of `struct proc` indexed by PID and protected by a spinlock, a flag storing whether the table is active or not, and a field containing the last assigned PID. The `proc_bootstrap` function was modified to initialize this data structure. Helper functions were then defined to:

- insert a new process into the Process Table and assign its PID (`proc_init_waitpid`);
- remove a process from the Process Table and deallocate it permanently (`proc_end_waitpid`);
- wait for the termination of a child process via its PID and read its exit code (`proc_wait`);
- retrieve a process given the PID and vice versa (`proc_search_pid` and `proc_getpid`).

### System Calls
We built upon the newly defined architecture and implemented several necessary syscalls:

- `sys_getpid`: Returns the current process identifier simply by looking at the `p_pid` field of its struct.

- `sys_fork`: Duplicates the current process by creating an identical child process with a separate address space, allocating a new trapframe, creating the child process, copying the address space with `as_copy`, calling `thread_fork` to start execution of the child process, and returning the child's PID to the parent.

- `sys_exit`: Terminates execution of a process, deallocating its address space and updating its state and exit code.

- `sys_waitpid`: Suspends the calling process until the child process with the specified PID returns, utilizing the logic defined in `proc_wait`.

- `sys_sbrk`: Modifies the size of the calling user process's heap area; this is logical memory that will be physically allocated only when needed (demand paging).

## Virtual Memory Architecture
The implemented virtual memory system entirely replaces dumbvm by managing on-demand allocation, page replacement (eviction), and the swap partition.

### Coremap
The coremap tracks the status of all available physical memory (RAM) frames by defining an array indexed by frame number. The array entries are of type `struct coremap_entry`, with the following fields:

- `occupancy_state`: The state of that frame, an enum that can be `FREE` if the frame is free, `IN_USE` if allocated, and `FIXED` if allocated and cannot be deallocated. We use the latter for frames allocated by the kernel at bootstrap time (thus also containing the coremap itself) and to lock frames that are being moved to the swap disk to avoid concurrency issues.
- `chunk_size`: Allocation size in number of pages/frames, different from 1 only for memory allocated by the kernel.
- `owner`: The address space that allocated that frame, equal to `NULL` if allocated by the kernel.
- `vaddr`: The user virtual address of that page, also relevant only if it is not a kernel frame.
- `counter`: FIFO counter for the page replacement algorithm, different from 0 only for user frames, as kernel-allocated frames are not considered for replacement.

A wait channel `eviction_wchan` is also defined, introduced to handle waits and prevent race conditions between blocking I/O operations (writing/reading on swap disk) and the destruction of page tables during process termination. Its task is to temporarily suspend threads that want to deallocate a process's memory as long as there are active disk I/O operations for that process.

The functions defined for the coremap are:

- `coremap_init`: Initializes the coremap at the start of VM bootstrap, calculating its total size based on RAM size and allocating space for the coremap. Pages occupied by the kernel and by the coremap itself are set to the `FIXED` state. The others are marked as FREE. The `eviction_wchan` wait channel is also created.

- `coremap_alloc`: Searches for and allocates a contiguous sequence of `npages`; if there are no free frames and the request is for a single page, it triggers the replacement mechanism by calling `coremap_evict_one` and then retries; returns a kernel virtual address.

- `coremap_free`: Receives a kernel virtual address and deallocates it, setting the page(s) to `FREE`.

- `coremap_set_owner`: Associates a frame with the owning address space, registering the associated user virtual address and storing the incremental FIFO counter value.

- `coremap_evict_one`: Selects a user frame with the smallest FIFO counter, temporarily sets its state to `FIXED`, and performs swap-out to disk. Subsequently, it invalidates the entry in the TLB if present, sets the flag in the process's page table indicating that the page is not in RAM, frees the frame by setting it to `FREE`, and notifies waiting threads on `eviction_wchan`.

### Interface with the Swap Disk
We implemented a swap mechanism for when main memory fills up; the disk used for this is `lhd1`. Inside the `swap.c` file, we define the disk vnode, a bitmap indicating whether each physical disk page is occupied or not, and a lock ensuring mutual exclusion for all operations. To interface with the swap disk, we use the following functions:

- `swap_bootstrap`: Connects the disk by calling `vfs_swapon`, obtains its size, and initializes the bitmap.

- `swap_alloc`: Finds and reserves a free slot in the bitmap, setting it to 1 under the protection of `swap_lock`.

- `swap_free`: Releases the specified slot in the bitmap, setting it to 0.

- `swap_write`: Performs the writing of a page from RAM to disk while holding the lock.

- `swap_read`: Performs the reading of a page from disk to RAM, also while holding the lock.

### Page Table
The core of our Virtual Memory system lies in the Page Table, which is 2-level and defined for each user process. In a process's address space, there is a first-level page directory containing 1024 pointers to second-level tables, initially initialized to `NULL`. At the second level, there are page tables, each with 1024 entries of type `paddr_t`, while the search index is the user virtual address. 
A page table entry can be:

- **null**: in this case, it assumes the value of 0;
- **in RAM**: in this case, the entry corresponds to the physical frame address (a multiple of 4096 and thus with the lower 12 bits set to 0) in the higher bits and the flag `PTE_PRESENT = 0x1` in the lower bits; 
- **in swap**: in this case, the entry contains in the highest 12 bits the block number on the swap disk where the page is contained and the flag `PTE_SWAPPED = 0x2` in the lower bits.

The functions defined for the Page Table are:

- `pagetable_create`: Creates a first-level page directory, initializing all 1024 entries to NULL.

- `pagetable_create_lv2`: Dynamically allocates a second-level page table, which is done on-demand—that is, only when the process addresses a page that needs to be stored in this table. Initializes all 1024 entries to 0.

- `pagetable_destroy`: Deallocates the page table, but first acquires the `mem_lock` spinlock (defined in `vm.c` and used to synchronize operations on the VM). Next, it checks if there are frames belonging to this process that are in the `FIXED` state (currently being swapped); if so, it goes to sleep on `eviction_wchan` to avoid deallocating physical pages that are being written to disk. Upon waking up, it repeats the scan; once assured that there are no more fixed pages, it sets `owner = NULL` for all frames owned by the process, then releases `mem_lock`. It then iterates over every entry in the second-level page tables, deallocating them either in memory with `free_kpages` or on disk with `swap_free`. Finally, it frees the second-level page tables and then the page directory.

- `pagetable_copy`: Performs a deep copy of the parent address space's page table to the child address space. Allocates first and second-level page tables and, for each entry, allocates a new page in memory, performs a copy using `memcpy`, and calls `coremap_set_owner` with the child's address space. If the entry is not in memory, it reads it from the swap disk before inserting it.

- `pagetable_get_entry`: Returns a pointer to `paddr_t`, used to modify entries in the page table when a page is moved to the swap disk and it is necessary to set `PTE_SWAPPED` and write the swap slot.

#### `pagetable_translate`
This function is one of the most fundamental to our implementation, as it brings together the lower-level components defined previously, besides being called every time a TLB miss occurs. It handles the "Page Table walk" and translates user logical addresses to physical addresses, in addition to resolving on-demand page requests.
It performs the following operations:

1. Takes a `vaddr_t` as input and extracts from it indices for the first-level page directory and the second-level page table.  
2. Checks if the second-level page table exists and, if not, allocates it.
3. Retrieves the page entry corresponding to the provided address using level 1 and level 2 indices.
4. Upon reading the entry, 3 different cases can occur:
    - The entry is present in RAM: <br>
        5. In this case, the entry is non-zero and `PTE_PRESENT` is set; the function jumps to the end and returns the recorded physical address. <br>
    - The entry is equal to 0: <br>
        5. The process is accessing this page for the first time. A RAM frame is allocated; if exhausted, this call leads to the eviction of other pages. <br>
        6. Zeroes out the allocated physical memory by calling `bzero`. <br>
        7. Maps the physical address by combining it with the `PTE_PRESENT` flag and writes it to the page table. <br>
        8. Associates the frame with the address space in the coremap by calling `coremap_set_owner`. <br>
        9. Returns the newly allocated physical address. <br>
    - The entry is present on disk (swap-in): <br>
        5. Means that the `PTE_SWAPPED` flag is set; a new page is allocated using `alloc_kpages`. <br>
        6. The swap slot index is extracted from the highest bits. <br>
        7. The page is read from disk with `swap_read` and data is written into the newly allocated page. <br>
        8. The slot on the swap partition bitmap is freed with `swap_free`. <br>
        9. Updates the entry by setting the new physical frame address and setting the `PTE_PRESENT` flag. <br>
        10. Registers the address space in the coremap with `coremap_set_owner`. <br>
        11. Returns the new physical address.

### Address Space
Each user process possesses its own address space; we implemented it by adding the following fields inside `struct addrspace`:

- `regions`: Pointer to the head of the linked list of `struct region`. This allows us to manage an indefinite number of regions, overcoming the limitation of dumbvm. `struct region` contains the following fields:
    - `vaddr`: The starting virtual address of the region.
    - `npages`: The size of the region expressed as a number of pages.
    - `readable`, `writeable`, `executable`: Boolean flags defining read, write, and execute permissions for the region.
    - `writeable_backup`: used to temporarily disable write protection on memory regions during the executable loading phase, subsequently restoring correct permissions prior to program execution.
- `stack_base`: The base address of the user stack.
- `stack_npages`: The maximum allowed size for the stack (set to 16 pages).
- `pagetable`: Pointer to the first-level page directory.
- `heap_start`: The starting virtual address of the heap.
- `heap_end`: The current upper limit of the heap (which expands dynamically via calls to `sys_sbrk`).
- `is_copying`: A boolean synchronization flag. When `true` (during an `as_copy` operation), `coremap_evict_one` will not select any page belonging to this address space as an eviction victim.
 
The defined functions are:

- `as_create`: Allocates and initializes a new address space, setting the region list to `NULL`, allocating the page table, and initializing the other fields.

- `as_destroy`: Deallocates an address space, freeing first the regions in the linked list, then the page table, and finally the `struct addrspace` itself.

- `as_copy`: Creates an identical copy of the address space during fork. Allocates a new address space and sets the `is_copying` flag to `true` on both parent and child. It then copies the heap and stack boundaries and duplicates the page table by calling `pagetable_copy`. Next, it iterates through the parent's region list and allocates each region in the child's list. Finally, it resets `is_copying = false` and returns the new address space.

- `as_activate`: Loads and activates the address space of the current process; basically handles flushing the TLB during context switches between two different address spaces. To ensure that the whole TLB is not invalidated when merely switching context temporarily without changing address spaces (the primary scenario being context switching for swap disk I/O), we introduce the global variable `active_as`. This function performs a complete TLB flush only if the address space to be activated is different from `active_as`, which is updated at the end of the function. All of this is done with interrupts disabled.

- `as_deactivate`: Deactivates the current process's address space and sets the global variable `active_as = NULL`, also with interrupts disabled.

- `as_define_region`: Defines and registers a new logical region (e.g., code or data) within the address space, allocating a new `struct region` and appending it to the `regions` linked list; it also updates the heap location, placing it right after the limit of the newly created region.

- `as_prepare_load`: Makes all regions temporarily writeable to prepare for loading an ELF file. When a new process is invoked, its regions (such as .text, which contains executable code and is non-writeable) are defined along with their permissions using `as_define_region`. Subsequently, the kernel needs to read the various regions of the ELF file and write them to a page in memory; to do this, it requires write permissions on all regions. To solve this, this function saves original permissions in `writeable_backup` and sets all regions to `writeable`; following this, `load_elf` will be called, which can then write to all regions.

- `as_complete_load`: After `load_elf` has been called, original permissions must be restored before executing the program; this function handles that by writing the value of `writeable_backup` back to `writeable` for each region.

- `as_define_stack`: Configures the bounds of the user stack; in our implementation, the stack starts at `0x80000000` and has a size of 16 pages.

### Central VM Manager
The `vm.c` file brings together all the sub-parts defined above; it takes care of entirely replacing `dumbvm.c` and thus defines all memory-related functions called by the rest of the kernel.
First of all, it defines a couple of global variables necessary for system operation:

- `vm_ready`: a boolean storing whether the VM is usable or not; in the early phases of kernel bootstrap, the coremap is not yet allocated, so the VM system is not yet active;
- `mem_lock`: a spinlock used to serialize all operations on the coremap and protect calls to `wchan_sleep` and `wchan_wakeall` on `eviction_wchan`.

The defined functions are:

- `vm_bootstrap`: Called during kernel initialization, calls `coremap_init` to define the coremap and `swap_init` to initialize the swap partition. Finally, sets `vm_ready` to `true` to enable VM usage.

- `alloc_kpages`: Allocates a contiguous block of `npages` for kernel use; if the VM is not ready, it uses `ram_stealmem`, otherwise it calls `coremap_alloc`; returns the virtual address of the first allocated page.

- `free_kpages`: Frees memory previously allocated by `alloc_kpages` by calling `coremap_free`.

#### `vm_fault`
This function is the central VM manager; it handles resolving all exceptions triggered by memory accesses. It is called whenever a TLB miss occurs—that is, when a thread wants to write to a virtual address whose page is not present in the TLB—and it is also called if a thread attempts to write to a virtual address marked as read-only in the TLB. It resolves these exceptions by updating the TLB or returning an error.
It takes an integer `faulttype`, which stores the fault type, and `faultaddress`, which is the `vaddr_t` that triggered the exception, as inputs. It performs the following operations:

1. Retrieves the current process's address space and returns `EFAULT` if it is `NULL`.
2. Checks that `faultaddress` is not 0 and that `faulttype` is a valid value, returning `EFAULT` otherwise.
3. Iterates through the linked list of address space regions to check if `faultaddress` is contained within one of them; if so, checks whether the permission types defined by the region and the operation described by `faulttype` are compatible; if not, returns `EFAULT`, otherwise jumps to step 6.
4. Checks if the address falls within the heap or stack; if so, jumps to step 6.
5. If it reaches here, it means the address is neither in the stack, nor in the heap, nor in any region defined by the addrspace, and is therefore an illegal access; returns `EFAULT`.
6. Calls `pagetable_translate`, which returns the `paddr` where the virtual address resides.
7. Disables interrupts and constructs the value to insert into the TLB (virtual page address in the upper 32 bits and frame address in the lower 32 bits).
8. Uses `tlb_probe` to check if a TLB entry already exists for the virtual page that generated the fault (as in the case of a fault where the page was present but non-writeable).
9. Otherwise, checks if the TLB contains an empty or invalid slot; if so, writes the entry using `tlb_write`.
10. Otherwise, replaces a valid entry in the TLB using `tlb_random`.
11. Finally, re-enables interrupts and returns 0 to indicate that the operation succeeded.

## Statistics and Benchmarks

### Statistics Collection
To test and evaluate the performance of the completed system, we introduced a statistics counting mechanism inside the `vmstats.c` file and its relative header. We have 6 global variables acting as counters for:

- TLB fault (free), i.e., resolved by finding a free slot;
- TLB fault (replace), resolved by replacing a pre-existing slot;
- TLB Invalidations;
- Page Faults;
- Reads from the Swap disk (also called page in);
- Writes to the Swap disk (also called page out).

We defined functions to start statistics recording, stop recording, reset counters, and print statistics. Finally, we defined a function that increments counters based on the type of statistic to record (`vm_record_stat`); calls to this function were appropriately inserted throughout the various files relating to our implementation.

### `vmstats` Command
To provide an interface for this mechanism, we added the `vmstats` command to `menu.c`, which calls the functions defined in `vmstats.c`. The command is invoked by typing `vmstats <flag>` in the menu, where `<flag>` can be:

- `--start`: starts recording VM statistics;
- `--stop`: ends statistics recording;
- `--reset`: resets all statistics counters;
- `--print`: prints collected statistics;

### Test Results
We ran all user-side tests in the `testbin/` directory related to VM operation; the results are reported in the following table.

| Test | RAM size | Time (s) | TLB fault (free) | TLB fault (replace) | TLB Invalidation | Page fault | Swap read (page in) | Swap write (page out) |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| ctest | 512K | 12248.748 | 92867 | 32645 | 92804 | 124986 | 124726 | 124912 |
| ctest | 1M | 11602.368 | 9658 | 116309 | 9595 | 123935 | 123675 | 123735 |
| ctest | 2M | 13.022 | 64 | 120321 | 1 | 260 | 0 | 0 |
| forktest | 512K | 3.795 | 221 | 0 | 220 | 28 | 34 | 34 |
| forktest | 1M | 0.844 | 212 | 0 | 72 | 5 | 0 | 0 |
| forktest | 2M | 0.848 | 214 | 0 | 74 | 5 | 0 | 0 |
| huge | 512K | 292.087 | 2896 | 706 | 2833 | 3582 | 3067 | 3511 |
| huge | 1M | 270.589 | 359 | 3238 | 296 | 3423 | 2908 | 3225 |
| huge | 2M | 231.972 | 170 | 3398 | 107 | 3155 | 2640 | 2702 |
| matmult | 512K | 58.763 | 631 | 193 | 568 | 813 | 430 | 740 |
| matmult | 1M | 46.859 | 106 | 695 | 43 | 772 | 389 | 572 |
| matmult | 2M | 0.686 | 64 | 732 | 1 | 383 | 0 | 0 |
| malloctest | 512K | 0.625 | 7 | 0 | 1 | 7 | 0 | 0 |
| malloctest | 1M | 0.655 | 7 | 0 | 1 | 7 | 0 | 0 |
| malloctest | 2M | 0.627 | 7 | 0 | 1 | 7 | 0 | 0 |
| palin | 512K | 15.321 | 5 | 0 | 1 | 5 | 0 | 0 |
| palin | 1M | 15.321 | 5 | 0 | 1 | 5 | 0 | 0 |
| palin | 2M | 15.295 | 5 | 0 | 1 | 5 | 0 | 0 |
| parallelVM* | 512K | 523.375 | 16858 | 0 | 32098 | 5242 | 5070 | 5211 |
| parallelVM | 1M | 349.415 | 18806 | 0 | 21821 | 3494 | 3156 | 3325 |
| parallelVM | 2M | 12.830 | 8615 | 0 | 3095 | 346 | 5 | 16 |
| sort | 512K | 160.582 | 1354 | 561 | 1291 | 1756 | 1463 | 1683 |
| sort | 1M | 57.310 | 206 | 1728 | 143 | 814 | 521 | 614 |
| sort | 2M | 3.538 | 64 | 1845 | 1 | 293 | 0 | 0 |

*parallelVM with a 512K configuration manages to fork only 13/24 processes and the reported values reflect this; this phenomenon is explained in more detail below.

### Performance Analysis

#### Thrashing for ctest and huge
The ctest test demonstrates the vulnerability of the FIFO replacement algorithm. This test creates a 1MB array and subsequently accesses the array in a strided manner, accessing elements with a distance between their indices equal to `stride`, which defaults to 477. Each stride equals 477 * 4 bytes = 1908 bytes (about half a page); this completely removes the principle of spatial locality.
If the available RAM is less than 1MB, as in the 512KB configuration or even the 1MB configuration since part of the RAM is occupied by the kernel, the following occurs:

- The entire array cannot reside completely in RAM; parts of it will be allocated on the swap disk.
- At each stride, the program very frequently accesses a different page that is not in RAM. 
- This leads to continuous page faults; the kernel must free a physical frame (performing a swap-out) in order to load a page present on disk (swap-in).
- With a FIFO replacement algorithm, the page requested at each iteration ends up being precisely the most recently evicted one.
- Thus, the thrashing phenomenon occurs—that is, the kernel spends all its time performing I/O on the swap disk instead of executing code, bringing the execution time to over 3 hours.

In contrast, for the 2MB RAM configuration, the entire array fits in memory, so the swap disk is not used and thrashing does not occur, drastically reducing execution time.

The huge test shows a similar situation, but since the total array size is 8MB, 2MB of physical memory is still not enough to contain all the data, leading to a similar execution time in all three configurations as they all go into thrashing. Total time is nonetheless smaller because this test performs fewer accesses than ctest.

#### Spatial Locality in sort
The sort test sorts a 576KB array using a temporary array of the same size, totaling over 1MB. We observe that:

- With 2MB of RAM, all data can be saved in memory; this is reflected in swap disk reads and writes being equal to 0.
- Reducing RAM to 1MB, an increase in swap reads and writes is observed. The quicksort algorithm recursively divides the array into partitions. As soon as the size of the sub-partitions becomes smaller than the available physical RAM, sorting can occur without further page faults, reducing the number of I/O operations. This is a case where the spatial locality assumption holds.
- Reducing memory to 512KB, the threshold at which partitions reside in RAM lowers, forcing the system to use swap more frequently and thus tripling execution time.

#### Kernel Memory Limits in parallelVM
The parallelVM test creates 24 parallel child processes, each performing matrix multiplications. It is observed that:

- In the 512KB RAM configuration, 11 sub-processes fail with an out-of-memory error. This is because each process requires allocation of a page for the kernel stack; being kernel memory, this page cannot be replaced and written to the swap disk. Therefore, if physical memory is insufficient, the coremap cannot allocate further frames and forces fork to return an error.
- With 1MB and 2MB of RAM instead, physical space is sufficient for all required kernel memory, allowing us to verify that our virtual memory works even in a concurrent environment. For 1MB of RAM, it is necessary to rely more heavily on the swap partition to save the matrices of the various processes, whereas for 2MB almost everything is contained in RAM, leading to a negligible number of swap accesses.
