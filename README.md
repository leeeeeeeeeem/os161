# Implementazione Virtual Memory su OS161

- Corso: Programmazione di Sistema
- Docente: Sarah Azimi
- Progetto: OS161 B
- Studenti: Greco Eleonora s354761, Jafrancesco Chiara s355023, Lemerle Stefano s353334

## Esecuzione programmi utente
Per poter eseguire i test sulla VM, è necessario poter eseguire i programmi utente oltre ai test kernel, per fare questo abbiamo modificato il file `runprogram.c`, contenente la funzione omonima. Abbiamo implementato un supporto per la gestione del passaggio di argomenti dal command line (`argc` e `argv`) al programma utente da parte del kernel, prima del passaggio a user mode. Per fare questo la funzione:

- definisce lo stack dell'address space utente chiamando `as_define_stack` e ne salva un puntatore al primo indirizzo;

- alloca temporaneamente nel kernel (con `kmalloc`) un array di puntatori per poter memorizzare gli indirizzi delle strighe che rappresentano i parametri;

- copia le strighe dal kernel allo stack utente usando `copyoutstr`, decrementando ogni volta lo stack pointer, inoltre salva gli indirizzi virtuali nell'array definito prima `argv_ptrs`;

- allinea lo stack pointer a 8 byte;

- copia l'array `argv_ptrs` sullo stack utente, usando la funzione `copyout`, in modo da fornire al processo utente un array di stringhe a cui può accedere;

- libera `argv_ptrs` dalla memoria kernel, effettua un ulteriore allineamento dello stack e aggiunge 16 byte di spazio per lo stack frame;

- chiama `enter_new_process` passando il numero di argomenti, lo stack pointer e l'entry point del programma.

## System call e gestione dei processi
Abbiamo riscontrato il bisogno di implementare un supporto aggiuntivo per il controllo dei processi, in modo tale che il processo del menu potesse attendere la terminazione del processo utente, per poter calcolare e ritornare correttamente il tempo di esecuzione dei test. Inoltre era necessario testare la nostra implementazione della VM in maniera concorrente per verificarne il funzionamento, con test come `forktest` e `parallelvm`.

### Process Table e nuovi campi in `struct proc`
Prima di tutto abbiamo modificato `proc.c` e `proc.h`, per migliorare la gestione dei processi. Abbiamo modificato la struttura dati dei processi `struct proc`, aggiungendo variabili per segnalare se il processo ha terminato e se il padre ha chiamato wait, l'exit code, un semaforo per la sincronizzazione, un puntatore al processo padre e infine il PID, ovvero un identifictore univoco del processo.
All'interno di `proc.c` abbiamo aggiunto una Process Table: una struttura dati globale che memorizza tutti i processi attivi in quel momento, tramite un array di `struct proc` indicizzato per PID e protetto da uno spinlock, un flag che memorizza se la tabella è attiva o meno e un campo contenente l'ultimo PID assegnato. La funzione `proc_bootstrap` è stata modificata per inizializzare questa struttura dati. Sono state poi definite funzioni di supporto per:

- inserire un nuovo processo dentro la Process Table e assegnargli il suo PID (`proc_init_waitpid`);
- rimuovere un processo dalla Process Table e deallocarlo definitivamente (`proc_end_waitpid`);
- attendere la terminazione di un processo figlio tramite il suo PID e leggerne l'exit code (`proc_wait`);
- prelevare un processo dato il PID e viceversa (`proc_search_pid` e `proc_getpid`).

### System calls
Ci siamo basati sull'architettura appena defita e abbiamo implementato alcune syscall necessarie:

- `sys_getpid`: Ritorna l'identificatore del processo attuale, guardando semplicemente il campo `p_pid` della sua struct.

- `sys_fork`: Duplica il processo corrente creando un processo figlio identico con spazio di indirizzamento separato, allocando un nuovo trapframe, creando il processo figlio, copiando l'address space con `as_copy`, chiamando `thread_fork` per avviare l'esecuzione del process o figlio e restituendo il PID del figlio al padre.

- `sys_exit`: Termina l'esecuzione di un processo, deallocandone l'address space, aggiornandone lo stato e l'exit code.

- `sys_waitpid`: Sospende il processo chiamante finchè il processo figlio di cui è stato specificato il PID non ritorna, utilizzando la logica definita in `proc_wait`.

- `sys_sbrk`: Modifica la dimensione dell'area heap del processo utente chiamante, si tratta di memoria logica che verrà allocata fisicamente soltanto quando necessaria (demand paging).

## Architettura della Virtual Memory
Il sistema di memoria virtuale implementato sostituisce interamente dumbvm gestendo l'allocazione on-demand, il rimpiazzamento delle pagine (eviction) e la partizione di swap.

### Coremap
La coremap tiene traccia dello stato di tutti i frame di memoria fisica (RAM) disponibili, definendo un array indicizzato per frame number. Le entry dell'array sono di tipo `struct coremap_entry`, con i seguenti campi:

- `occupancy_state`: Lo stato di quel frame, un enum che può essere `FREE` se il frame è libero, `IN_USE` se è allocato e `FIXED` se è allocato e non può essere dealloacato. Usiamo quest'ultimo per i frame allocati dal kernel al momento del bootstrap (quindi contenenti anche la coremap stessa) e per bloccare i frame che stanno venendo spostati sul disco di swap, per evitare problemi di concorrenza.
- `chunk_size`: Dimensione dell'allocazione in numero di pagine/frame, diverso da 1 solamente per la memoria allocata dal kernel.
- `owner`: L'addrspace che ha allocato quel frame, uguale a `NULL` se allocato dal kernel.
- `vaddr`: L'indirizzo virtuale utente di quella pagina, anche questo rilevante solamente se non è un frame kernel.
- `counter`: Contatore FIFO per l'algoritmo di rimpiazzamento delle pagine, diverso da 0 solamente per frame utente, in quanto quelli allocati dal kernel non sono considerati per il rimpiazzamento.

Viene anche definito una wait channel `eviction_wchan`, introdotto per gestire le attese ed evitare race condition tra le operazioni bloccanti di I/O (scrittura/lettura su disco di swap) e la distruzione delle pagetable in fase di terminazione dei processi. Ha il compito di sospendere temporaneamente i thread che vogliono deallocare la memoria di un processo finché ci sono operazioni di I/O su disco attive per quel processo.

Le funzioni definite per la coremap sono:

- `coremap_init`: Inizializza la coremap all'inizio del bootstrap della VM, calcolandone la dimensione totale in base alla dimensione della RAM, allocando lo spazio per la coremap. Le pagine occupate dal kernel e dalla coremap stessa vengono impostate nello stato `FIXED`. Le altre vengono marcate come FREE. Viene creato anche il canale di attesa `eviction_wchan`

- `coremap_alloc`: Cerca e alloca una sequenza di `npages` contigua, se non ci sono frame liberi e la richiesta è per una pagina singola attiva il meccanismo di rimpiazzamento chiamando `coremap_evict_one` e poi riprova, ritorna un indirizzo virtuale kernel.

- `coremap_free`: Riceve un indirizzo virtuale kernel e lo dealloca impostanto la/e pagina/e a `FREE`.

- `coremap_set_owner`: Associa un frame all'address space proprietario, registrando l'indirizzo virtuale utente associato e memorizzando il valore incrementale del contatore FIFO.

- `coremap_evict_one`: Sceglie un frame utente con il contatore FIFO minore, imposta temporaneamente il suo stato a `FIXED` ed effettua lo swap-out su disco. Successivamente invalida l'entry nel TLB se presente, imposta il flag per dire che la pagina non è in RAM sulla pagetable del processo, libera il frame impostandolo a `FREE` e notifica i thread in attesa su `eviction_wchan`.
