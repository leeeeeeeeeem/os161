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

### Interfaccia con il disco di Swap
Abbiamo implementato un meccansimo di swap per quando la memoria principale si riempe, il disco utilizzato per questo è `lhd1`. All'interno del file `swap.c` definiamo il vnode del disco, una bitmap che indica se ogni pagina fisica del disco è occupata o meno e un lock che garantisce la mutua esclusione per tutte le operazioni. Per interfacciarci con il disco di swap utilizziamo le seguenti funzioni:

- `swap_bootstrap`: Connette il disco chiamando `vfs_swapon`, ne ottiene la dimensione e inizializza la bitmap.

- `swap_alloc`: Trova e riserva uno slot libero nella bitmap impostandolo a 1 sotto la protezione di `swap_lock`.

- `swap_free`: Rilascia lo slot specificato nella bitmap impostandolo a 0.

- `swap_write`: Effettua la scrittura di una pagina dalla RAM al disco, detenendo il lock.

- `swap_read`: Effettua la lettura di una pagina dalla disco al RAM, sempre detenendo il lock.

### Page Table
Il cuore del nostro sistema di Memoria Virtuale sta nella Page Table, la quale è a 2 livelli e definita per ogni processo utente. Nell'address space di un processo c'è una page directory di primo livello, contenente 1024 puntatori a tabelle di secondo livello, inizialmente inizializzate a `NULL`. Al secondo livello ci sono le page table ognuna con 1024 entry di tipo `paddr_t`, mentre l'indice di ricerca è l'indirizzo virtuale utente. 
L'entry della page table può essere:

- **nulla**: in questo caso assume il valore di 0;
- **nella RAM**: in questo caso l'entry corrisponde all'indirizzo del frame fisico (un multiplo di 4096 e quindi con i 12 bit inferiori a 0) nei bit più alti e la flag `PTE_PRESENT = 0x1` nei bit più bassi; 
- **in swap**: in questo caso l'entry contiene nei 12 bit più alti il numero del blocco nel disco di swap nel quale è contenuta la pagina e la flag `PTE_SWAPPED = 0x2` nei bit più bassi.

Le funzioni definite per la Page Table sono:

- `pagetable_create`: Crea una page directory di primo livello, inizializzando tutte le 1024 entry a NULL.

- `pagetable_create_lv2`: Si occupa di allocare dinamicamente una page table di secondo livello, che viene fatto on-demand, ovvero solo quando il processo indirizza una pagina che deve essere salvata in questa tabella. Inizializza tutte le 1024 entry a 0.

- `pagetable_destroy`: Si occupa di dellocare la page table, ma prima aquisisce lo spinlock `mem_lock` (definito in `vm.c` e utilizzato per sincronizzare operazioni sulla VM). Successivamente controlla se ci sono frame appartenenti a questo processo che si trovano nello stato `FIXED` (stanno venendo swappate), nel caso ci siano si mette a dormire su `eviction_wchan` per evitare di deallocare pagine fisiche che stanno venendo scritte su disco. Al risveglio ripete la scanzione, una volta accertato che non ci siano più pagine fissate imposta `owner = NULL` a tutti i frame posseduti dal processo, dopo rilascia `mem_lock`. Poi scorre su ogni entry delle page table di secondo livello, deallocandole o in memoria con `free_kpages` oppure sul disco con `swap_free`. Infine libera le page table di secondo livello e poi la page directory.

- `pagetable_copy`: Effettua una copia in profondità della pagetable dell'address space padre all'address space figlio. Alloca page table di primo e secondo livello e per ogni entry alloca una nuova pagina in memoria, effettua una copia con `memcpy` e chiama `coremap_set_owner` con l'address space del figlio. Nel caso l'entry non sia in memoria la legge dal disco di swap prima di inserirla.

- `pagetable_get_entry`: Restituisce un puntatore a `paddr_t`, utilizzato per modificare entry nella page table quando una pagina viene spostata sul disco di swap ed è necessario impostare `PTE_SWAPPED` e scrivere lo swap slot.

#### `pagetable_translate`
Questa funzione è una delle più fondamentali per la nostra implenetazione, in quanto mette insieme le varie componenti di basso livello definite precedentemente, oltre al fatto che viene chiamata ogni volta che abbiamo un TLB miss. Si occupa di fare la "Page Table walk" e di effettuare traduzioni da indirizzi logici utente a indirizzi fisici, oltre a risolvere richieste di pagine on-demand.
Effettua le seguenti operazioni:

1. Prende in input un `vaddr_t` e da esso estrae indici per la page directory di primo livello e per la page table di secondo livello.  
2. Controlla se la page table di 2 livello esiste e nel caso negativo la alloca.
3. Preleva l'entry della pagina corrispondente all'indirizzo fornito, utilizzando indici di livello 1 e 2.
4. Leggendo l'entry ci possono essere 3 casistiche diverse:
    
    - L'entry è presente in RAM:

        5. In questo caso l'entry è diversa da 0 e `PTE_PRESENT` è settato, la funzione salta alla fine e restituisce l'indirizzo fisico registrato.

    - L'entry è uguale a 0:

        5. Il processo sta accedendo a questa pagina per la prima volta. Si alloca un frame della RAM, se è esaurita, questa chiamata porta all'eviction di altre pagine.
        6. Azzera la memoria fisica allocata chiamando `bzero`.
        7. Mappa l'indirizzo fisico combinandolo con la flag `PTE_PRESENT` e lo scrive nella pagetable.
        8. Associa il frame all'address space nella coremap chiamando `coremap_set_owner`.
        9. Restituisce l'indirizzo fisico appena allocato.

    - L'entry è presente sul disco (swap-in): 

        5. Significa che flag `PTE_SWAPPED` è settato, viene allocata una nuova pagina con `alloc_kpages`.
        6. Viene estratto l'indice dello slot di swap dai bit più alti
        7. Viene letta la pagina dal disco con `swap_read` e i dati vengono scritti nella pagina appena allocata.
        8. Viene liberato lo slot sulla partizione di swap bitmap con `swap_free`.
        9. Aggiorna l'entry impostando l'indirizzo del nuovo frame fisico e settando il flag `PTE_PRESENT`
        10. Registra l'addrspace nella coremap con `coremap_set_owner`.
        11. Restituisce il nuovo indirizzo fisico.

### Address space
