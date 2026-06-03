/*
 * Copyright (c) 2013
 *	The President and Fellows of Harvard College.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE UNIVERSITY AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE UNIVERSITY OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

/*
 * Process support.
 *
 * There is (intentionally) not much here; you will need to add stuff
 * and maybe change around what's already present.
 *
 * p_lock is intended to be held when manipulating the pointers in the
 * proc structure, not while doing any significant work with the
 * things they point to. Rearrange this (and/or change it to be a
 * regular lock) as needed.
 *
 * Unless you're implementing multithreaded user processes, the only
 * process that will have more than one thread is the kernel process.
 */

#include "opt-WAITPID.h"

#include <types.h>
#include <spl.h>
#include <proc.h>
#include <current.h>
#include <addrspace.h>
#include <vnode.h>
#include <synch.h>
#include <thread.h>

/*
 * The process for the kernel; this holds all the kernel-only threads.
 */
struct proc *kproc;

#if OPT_WAITPID

/*
 * Numero massimo di processi gestiti dalla tabella.
 *
 * EXTRA RISPETTO AL PDF:
 * Il PDF usa MAX_PROC ma non mostra dove definirlo.
 */
#define MAX_PROC 128

/*
 * Per semplicità uso il semaforo per waitpid.
 *
 * EXTRA RISPETTO AL PDF:
 * Il PDF parla sia di semaforo sia di CV+lock.
 * Qui scegliamo semaforo perché è sufficiente per il tuo problema
 * e richiede meno modifiche.
 */
#define USE_SEMAPHORE_FOR_WAITPID 1

/*
 * Tabella globale dei processi:
 * indice = pid
 * valore = puntatore alla struct proc
 */
static struct _processTable {
	int active;
	struct proc *proc[MAX_PROC + 1]; /* pid 0 non usato */
	int last_i;
	struct spinlock lk;
} processTable;

#endif

/*##############FUNZIONI AUSILIARIE WAIT_PID##########################*/

#if OPT_WAITPID

/*
 * Cerca un processo tramite pid.
 * Serve a sys_waitpid(pid, ...).
 */
struct proc *
proc_search_pid(pid_t pid)
{
	struct proc *p;

	if (pid <= 0 || pid > MAX_PROC) {
		return NULL;
	}

	spinlock_acquire(&processTable.lk);
	p = processTable.proc[pid];
	spinlock_release(&processTable.lk);

	if (p == NULL) {
		return NULL;
	}

	KASSERT(p->p_pid == pid);

	return p;
}

/*
 * Restituisce il pid di una proc.
 *
 * EXTRA RISPETTO AL PDF:
 * Il PDF dice di usare proc->p_pid o sys_getpid(proc).
 * Questa funzione evita di accedere direttamente al campo p_pid
 * da altri file.
 */
pid_t
proc_getpid(struct proc *proc)
{
	pid_t pid;

	KASSERT(proc != NULL);

	spinlock_acquire(&proc->p_lock);
	pid = proc->p_pid;
	spinlock_release(&proc->p_lock);

	return pid;
}

/*
 * Inizializza i campi necessari per waitpid:
 * - assegna un pid
 * - registra la proc nella tabella
 * - inizializza stato di uscita
 * - crea il semaforo su cui il padre aspetterà
 */
static
void
proc_init_waitpid(struct proc *proc, const char *name)
{
	int i;
	int count;

	KASSERT(proc != NULL);

	spinlock_acquire(&processTable.lk);

	proc->p_pid = 0;

	i = processTable.last_i + 1;
	if (i > MAX_PROC) {
		i = 1;
	}

	/*
	 * EXTRA RISPETTO AL PDF:
	 * Nel PDF il ciclo è potenzialmente problematico perché pid 0 non è usato.
	 * Qui uso count per fare al massimo MAX_PROC tentativi.
	 */
	for (count = 0; count < MAX_PROC; count++) {
		if (processTable.proc[i] == NULL) {
			processTable.proc[i] = proc;
			processTable.last_i = i;
			proc->p_pid = i;
			break;
		}

		i++;
		if (i > MAX_PROC) {
			i = 1;
		}
	}

	spinlock_release(&processTable.lk);

	if (proc->p_pid == 0) {
		panic("too many processes: process table is full\n");
	}

	/*
	 * Nel tuo proc.c esistono già p_exitcode e p_exited,
	 * quindi NON aggiungo p_status come nel PDF.
	 */
	proc->p_exitcode = 0;
	proc->p_exited = false;

	/*
	 * EXTRA RISPETTO AL PDF:
	 * Evita doppia wait sulla stessa proc.
	 */
	proc->p_waited = false;
	proc->p_parent = NULL;

#if USE_SEMAPHORE_FOR_WAITPID
	proc->p_sem = sem_create(name, 0);
	if (proc->p_sem == NULL) {
		panic("proc_init_waitpid: sem_create failed\n");
	}
#endif
}

/*
 * Fine gestione waitpid:
 * - rimuove la proc dalla tabella globale
 * - distrugge il semaforo
 */
static
void
proc_end_waitpid(struct proc *proc)
{
	int pid;

	KASSERT(proc != NULL);

	pid = proc->p_pid;

	KASSERT(pid > 0 && pid <= MAX_PROC);

	spinlock_acquire(&processTable.lk);
	KASSERT(processTable.proc[pid] == proc);
	processTable.proc[pid] = NULL;
	spinlock_release(&processTable.lk);

#if USE_SEMAPHORE_FOR_WAITPID
	sem_destroy(proc->p_sem);
	proc->p_sem = NULL;
#endif
}

#endif


/*########################GESTIONE PROCESSO##########################
 * Create a proc structure.
 */

static
struct proc *
proc_create(const char *name)
{
	struct proc *proc;

	proc = kmalloc(sizeof(*proc));
	if (proc == NULL) {
		return NULL;
	}
	proc->p_name = kstrdup(name);
	if (proc->p_name == NULL) {
		kfree(proc);
		return NULL;
	}

	proc->p_numthreads = 0;
	spinlock_init(&proc->p_lock);
	proc->p_exitcode = 0;
	proc->p_exited = false;

	/* VM fields */
	proc->p_addrspace = NULL;

	/* VFS fields */
	proc->p_cwd = NULL;

	#if OPT_WAITPID
		proc_init_waitpid(proc, name);
	#endif

	return proc;
}

/*
 * Destroy a proc structure.
 *
 * Note: nothing currently calls this. Your wait/exit code will
 * probably want to do so.
 */
void
proc_destroy(struct proc *proc)
{
	/*
	 * You probably want to destroy and null out much of the
	 * process (particularly the address space) at exit time if
	 * your wait/exit design calls for the process structure to
	 * hang around beyond process exit. Some wait/exit designs
	 * do, some don't.
	 */

	KASSERT(proc != NULL);
	KASSERT(proc != kproc);

	/*
	 * We don't take p_lock in here because we must have the only
	 * reference to this structure. (Otherwise it would be
	 * incorrect to destroy it.)
	 */

	/* VFS fields */
	if (proc->p_cwd) {
		VOP_DECREF(proc->p_cwd);
		proc->p_cwd = NULL;
	}

	/* VM fields */
	if (proc->p_addrspace) {
		/*
		 * If p is the current process, remove it safely from
		 * p_addrspace before destroying it. This makes sure
		 * we don't try to activate the address space while
		 * it's being destroyed.
		 *
		 * Also explicitly deactivate, because setting the
		 * address space to NULL won't necessarily do that.
		 *
		 * (When the address space is NULL, it means the
		 * process is kernel-only; in that case it is normally
		 * ok if the MMU and MMU- related data structures
		 * still refer to the address space of the last
		 * process that had one. Then you save work if that
		 * process is the next one to run, which isn't
		 * uncommon. However, here we're going to destroy the
		 * address space, so we need to make sure that nothing
		 * in the VM system still refers to it.)
		 *
		 * The call to as_deactivate() must come after we
		 * clear the address space, or a timer interrupt might
		 * reactivate the old address space again behind our
		 * back.
		 *
		 * If p is not the current process, still remove it
		 * from p_addrspace before destroying it as a
		 * precaution. Note that if p is not the current
		 * process, in order to be here p must either have
		 * never run (e.g. cleaning up after fork failed) or
		 * have finished running and exited. It is quite
		 * incorrect to destroy the proc structure of some
		 * random other process while it's still running...
		 */
		struct addrspace *as;

		if (proc == curproc) {
			as = proc_setas(NULL);
			as_deactivate();
		}
		else {
			as = proc->p_addrspace;
			proc->p_addrspace = NULL;
		}
		as_destroy(as);
	}

	KASSERT(proc->p_numthreads == 0);

	#if OPT_WAITPID
		proc_end_waitpid(proc);
	#endif

	spinlock_cleanup(&proc->p_lock);

	kfree(proc->p_name);
	kfree(proc);
}

/*
 * Create the process structure for the kernel.
 */

void
proc_bootstrap(void)
{
#if OPT_WAITPID
	int i;

	processTable.active = 1;
	processTable.last_i = 0;

	for (i = 0; i <= MAX_PROC; i++) {
		processTable.proc[i] = NULL;
	}

	spinlock_init(&processTable.lk);
#endif

	kproc = proc_create("[kernel]");
	if (kproc == NULL) {
		panic("proc_create for kproc failed\n");
	}
}
/*
 * Create a fresh proc for use by runprogram.
 *
 * It will have no address space and will inherit the current
 * process's (that is, the kernel menu's) current directory.
 */
struct proc *
proc_create_runprogram(const char *name)
{
	struct proc *newproc;

	newproc = proc_create(name);
	if (newproc == NULL) {
		return NULL;
	}

	/* VM fields */

	newproc->p_addrspace = NULL;

	/* VFS fields */

	/*
	 * Lock the current process to copy its current directory.
	 * (We don't need to lock the new process, though, as we have
	 * the only reference to it.)
	 */
	spinlock_acquire(&curproc->p_lock);
	if (curproc->p_cwd != NULL) {
		VOP_INCREF(curproc->p_cwd);
		newproc->p_cwd = curproc->p_cwd;
	}
	spinlock_release(&curproc->p_lock);

	return newproc;
}

/*
 * Add a thread to a process. Either the thread or the process might
 * or might not be current.
 *
 * Turn off interrupts on the local cpu while changing t_proc, in
 * case it's current, to protect against the as_activate call in
 * the timer interrupt context switch, and any other implicit uses
 * of "curproc".
 */
int
proc_addthread(struct proc *proc, struct thread *t)
{
	int spl;

	KASSERT(t->t_proc == NULL);

	spinlock_acquire(&proc->p_lock);
	proc->p_numthreads++;
	spinlock_release(&proc->p_lock);

	spl = splhigh();
	t->t_proc = proc;
	splx(spl);

	return 0;
}

/*
 * Remove a thread from its process. Either the thread or the process
 * might or might not be current.
 *
 * Turn off interrupts on the local cpu while changing t_proc, in
 * case it's current, to protect against the as_activate call in
 * the timer interrupt context switch, and any other implicit uses
 * of "curproc".
 */
void
proc_remthread(struct thread *t)
{
	struct proc *proc;
	int spl;

	proc = t->t_proc;
	KASSERT(proc != NULL);

	spinlock_acquire(&proc->p_lock);
	KASSERT(proc->p_numthreads > 0);
	proc->p_numthreads--;
	spinlock_release(&proc->p_lock);

	spl = splhigh();
	t->t_proc = NULL;
	splx(spl);
}

#if OPT_WAITPID

/*
 * Attende la terminazione del processo figlio.
 *
 * Questa è la funzione usata dal kernel/menu:
 *
 *     exit_code = proc_wait(proc);
 *
 * Quando il figlio chiama sys__exit(), fa V(proc->p_sem),
 * quindi questa P si sblocca.
 */
int
proc_wait(struct proc *proc)
{
	int return_status;

	KASSERT(proc != NULL);
	KASSERT(proc != kproc);

#if USE_SEMAPHORE_FOR_WAITPID
	P(proc->p_sem);
#endif

	spinlock_acquire(&proc->p_lock);
	KASSERT(proc->p_exited == true);
	return_status = proc->p_exitcode;
	spinlock_release(&proc->p_lock);

	proc_destroy(proc);

	return return_status;
}

#endif
/*
 * Fetch the address space of (the current) process.
 *
 * Caution: address spaces aren't refcounted. If you implement
 * multithreaded processes, make sure to set up a refcount scheme or
 * some other method to make this safe. Otherwise the returned address
 * space might disappear under you.
 */
struct addrspace *
proc_getas(void)
{
	struct addrspace *as;
	struct proc *proc = curproc;

	if (proc == NULL) {
		return NULL;
	}

	spinlock_acquire(&proc->p_lock);
	as = proc->p_addrspace;
	spinlock_release(&proc->p_lock);
	return as;
}

/*
 * Change the address space of (the current) process. Return the old
 * one for later restoration or disposal.
 */
struct addrspace *
proc_setas(struct addrspace *newas)
{
	struct addrspace *oldas;
	struct proc *proc = curproc;

	KASSERT(proc != NULL);

	spinlock_acquire(&proc->p_lock);
	oldas = proc->p_addrspace;
	proc->p_addrspace = newas;
	spinlock_release(&proc->p_lock);
	return oldas;
}
