#include <types.h>
#include <kern/errno.h>
#include <kern/reboot.h>
#include <kern/unistd.h>
#include <lib.h>
#include <uio.h>
#include <clock.h>
#include <mainbus.h>
#include <synch.h>
#include <thread.h>
#include <proc.h>
#include <vfs.h>
#include <sfs.h>
#include <syscall.h>
#include <test.h>

int sys_write(int fd, userptr_t buf_ptr, size_t size){
	char* buf = (char*) buf_ptr;

	if (fd != STDOUT_FILENO && fd != STDERR_FILENO)
		kprintf("Error, can only write to stdout or stderr");

	for (int i = 0; i < (int) size; i++){
		putch(buf[i]);
	}

	return (int) size;
}


int sys_read(int fd, userptr_t buf_ptr, size_t size){
	char* buf = (char*) buf_ptr;

	if (fd != STDOUT_FILENO && fd != STDERR_FILENO)
		kprintf("Error, can only read from stdout or stderr");

	for (int i = 0; i < (int) size; i++){
		buf[i] = getch();
		if (buf[i] == -1)
			return i;
	}

	return (int) size;
}
