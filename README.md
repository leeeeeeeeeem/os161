# Implementazione Virtual Memory su OS161

- Corso: Programmazione di Sistema
- Docente: Sarah Azimi
- Progetto: OS161 B
- Studenti: G42 - Greco Eleonora s354761, Jafrancesco Chiara s355023, Lemerle Stefano s353334

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
    - L'entry è presente in RAM: <br>
        5. In questo caso l'entry è diversa da 0 e `PTE_PRESENT` è settato, la funzione salta alla fine e restituisce l'indirizzo fisico registrato. <br>
    - L'entry è uguale a 0: <br>
        5. Il processo sta accedendo a questa pagina per la prima volta. Si alloca un frame della RAM, se è esaurita, questa chiamata porta all'eviction di altre pagine. <br>
        6. Azzera la memoria fisica allocata chiamando `bzero`. <br>
        7. Mappa l'indirizzo fisico combinandolo con la flag `PTE_PRESENT` e lo scrive nella pagetable. <br>
        8. Associa il frame all'address space nella coremap chiamando `coremap_set_owner`. <br>
        9. Restituisce l'indirizzo fisico appena allocato. <br>
    - L'entry è presente sul disco (swap-in): <br>
        5. Significa che flag `PTE_SWAPPED` è settato, viene allocata una nuova pagina con `alloc_kpages`. <br>
        6. Viene estratto l'indice dello slot di swap dai bit più alti. <br>
        7. Viene letta la pagina dal disco con `swap_read` e i dati vengono scritti nella pagina appena allocata. <br>
        8. Viene liberato lo slot sulla partizione di swap bitmap con `swap_free`. <br>
        9. Aggiorna l'entry impostando l'indirizzo del nuovo frame fisico e settando il flag `PTE_PRESENT` <br>
        10. Registra l'addrspace nella coremap con `coremap_set_owner`. <br>
        11. Restituisce il nuovo indirizzo fisico.

### Address space
Ogni processo utente possiete un suo address space, noi lo abbiamo implementato inserendo i seguenti campi all'interno di `struct addrspace`:

- `regions` : Puntatore alla testa della lista concatenata di `struct region`. Questo ci permette di gestire un numero
  indefinito di regioni, superando il limite di dumbvm. `struct region` contiene i seguenti campi:
    - `vaddr`: L'indirizzo virtuale di partenza della regione.
    - `npages`: La dimensione della regione espressa in numero di pagine.
    - `readable`,  `writeable`, `executable`: Flag booleani che definiscono i permessi di lettura, scrittura ed esecuzione per la regione.
    - `writeable_backup`: serve per disabilitare temporaneamente la protezione in scrittura delle regioni di memoria durante la fase di caricamento dell'eseguibile, ripristinando poi i permessi corretti prima dell'esecuzione del programma.
- `stack_base`: L'indirizzo di base dello stack utente.
- `stack_npages`: La dimensione massima consentita per lo stack (impostata a 16 pagine).
- `pagetable`: Puntatore alla page directory di primo livello.
- `heap_start`: L'indirizzo virtuale di partenza dell'heap.
- `heap_end`: Il limite superiore corrente dell'heap (che si estende dinamicamente con chiamate a `sys_sbrk`).
- `is_copying`: Un flag booleano di sincronizzazione. Quando è `true` (durante la un operazione di `as_copy`), `coremap_evict_one` non selezionerà come vittima di eviction nessun pagina appartenente a questo address space.
 
Le funzioni definite sono:

- `as_create`: Alloca e inizializza un nuovo address space, impostando la lista delle regioni a `NULL`, allocando la page table e inizializzando gli altri campi.

- `as_destroy`: Dealloca un address space, liberando prima le regioni nella linked list, poi la page table e infine la `struct addrspace` in se.

- `as_copy`: Crea una copia identica dell'address space durante la fork. Alloca un nuovo address space, imposta il flag `is_copying` a `true` su sia padre che figlio. Poi copia i limiti dell'heap e dello stack e duplica la pagetable chiamando `pagetable_copy`. Poi scorre la lista delle regioni del padre e alloca ogni regione nella lista del figlio. Infine rimette `is_copying = false` e ritorna il nuovo address space.

- `as_activate`: Carica e attiva l'address space del processo attuale, di base si occupa di fare un flush del TLB durante il cambio di contesto tra due address space diversi. Per fare in modo che non si invalidi tutto il TLB se stiamo solo cambiando contesto temporaneamente ma non cambiando address space (la casistica principale in cui succede questo è il context switch per I/O sul disco di swap), introduciamo la variabile globale `active_as`. Questa funzione esegue il flush completo del TLB solamente se l'address space che si vuole attivare è diverso da `activa_as`, il quale viene aggiornato alla fine della funzione. Tutto questo viene fatto con le interruzioni disabilitate.

- `as_deactivate`: Disattiva l'address space del processo attuale e imposta la variabile globale `active_as = NULL`, sempre con interrupt disabilitate.

- `as_define_region`: Definisce e registra una nuova regione logica (es. codice o dati) all'interno dell'address space, allocando un nuovo `struct region` e aggiungendolo in coda alla linked list `regions`, aggiorna anche la posizione dell'heap mettendola subito dopo il limite della regione appena creata.

- `as_prepare_load`: Rende tutte le regioni temporaneamente scrivibili, per preparare il caricamento di un file ELF. Quando un nuovo processo viene chiamato, le sue regioni (tra cui per esempio .text che contiene il codice eseguibile e non è scrivibile) vengono definite insieme ai loro permessi con `as_define_region`. Successivamente il kernel dovrà leggere le varie regioni del file ELF e scriverle su una pagina in memoria, per fare questo necessita di permessi di scrittura su tutte le regioni. Per risolvere, questa funzione salva in `writeable_backup` i permessi originali e imposta tutte le regioni come `writeable`, dopo di questa verrà chiamata `load_elf`, che potrà quindi scrivere su tutte le regioni.

- `as_complete_load`: Dopo che è stato chiamata `load_elf`, è necessario ripristinare i permessi originali prima di eseguire il programma, quest funzione si occupa di fare quello, riscrivendo su `writeable` il valore di `writeable_backup` per ogni regione.

- `as_define_stack`: Configura i limiti dello stack utente, nella nostra implementazione lo stack inizio da `0x80000000` e ha come dimensione 16 pagine.

### Gestore centrale della VM
Il file `vm.c` unisce tutte le sottoparti appena definite, si occupa di sostituire completamente `dumbvm.c` e quindi definisce tutte le funzioni relative alla memoria che vengonono chiamate dal resto del kernel.
Prima di tutto definisce un paio di variabili globali necessari per il funzionamento del sistema:

- `vm_ready`: un booleano che memorizza se la VM è utilizzabile o meno, nelle prime fasi del bootstrap del kernel la coremap non è ancora allocata e quindi il sistema di VM non è ancora attivo;
- `mem_lock`: uno spinlock che serve a serializzare tutte le operazioni sulla coremap e a proteggere le chiamate a `wchan_sleep` e `wchan_wakeall` su `eviction_wchan`.

Le funzioni definite sono:

- `vm_bootstrap`: Chiamata durante l'inizializzazione del kernel, chiama `coremap_init` per definire la coremap e `swap_init` per inizializzare la partizione di swap. Infine imposta `vm_ready` a `true` per abilitare l'utilizzo della VM.

- `alloc_kpages`: Alloca un blocco continuo di `npages` per l'uso del kernel, se la VM non è pronta usa `ram_stealmem` e altrimenti chiama `coremap_alloc`, restituisce l'indirizzo virtuale della prima pagina allocata.

- `free_kpages`: Libera la memoria precedentemente allocata da `alloc_kpages`, chiamando `coremap_free`.

#### `vm_fault`
Questa funzione è il gestore centrale della VM, si occupa di risolvere tutte le eccezioni scatenate dagli accessi in memoria. Viene chiamata ogni volta che si ha un TLB miss, ovvero quando un thread vuole scrivere su un'indirizzo virtuale la cui pagina non è presente nel TLB, viene anche chiamata se un thread cerca di scrivere a un indirizzo virtuale marcato come un indirizzo di sola scrittura nel TLB. Si occupa di risolvere queste eccezioni aggiornando il TLB o restituendo un errore. 
Prende in input `faulttype` di tipo intero, che memorizza il tipo di fault e `faultaddress` che è il `vaddr_t` che ha scatenato l'eccezione. Svolge le seguenti operazioni:

1. Preleva l'address space del processo corrente e ritorna `EFAULT` se è `NULL`.
2. Controlla che `faultaddress` sia diverso da 0 e che `faulttype` sia un valore valido, altrimenti ritorna `EFAULT`.
3. Scorre la linked list delle regioni dell'address space per controllare se `faultaddress` è contenuto in uno di esse, se si controlla se il tipo di permessi definiti dalla regione e l'operazione descritta da `faulttype` sono compatibili, se non lo sono ritorna `EFAULT`, altrimenti salta a 6.
4. Controlla se l'indirizzo rientra nell'heap e nello stack, se si salta a 6.
5. Se arriva quì significa che l'indirizzo non è ne nello stack, ne nell'heap ne in nessuna regione definita dall'addrspace e quindi è un accesso illegale, restituisce `EFAULT`.
6. Chiama `pagetable_translate`, che ritorna il `paddr` dove è presente l'indirizzo virtuale.
7. Disabilita le interrupt e costruisce il valore da inserire nel TLB (indirizzo di pagina virtuale nei primi 32 bit e indirizzo di frame nei 32 bit più bassi).
8. Cerca con `tlb_probe` se nel TLB esiste già una voce per la pagina virtuale che ha generato il fault (come nel caso del fault per cui la pagina era presente ma non scrivibile).
9. Altrimenti cerca se il TLB contiene uno slot vuoto o invalido, se si ci scrive l'entry con `tlb_write`.
10. Altrimenti sostituisce un entry valido nel TLB con `tlb_random`.
11. Infine riabilita gli interrupt e ritorna 0 per dire che l'operazione è andata a buon fine.

## Statistiche e benchmark

### Raccoglimento statistiche
Per testare e valutare le prestazioni del sistema finito abbiamo introdotto un meccanismo di conteggio delle statistiche, all'interno del file `vmstats.c` e il relativo header. Abbiamo 6 variabili globali che fungono da contatori per:

- TLB fault (free), ovver risolto trovando uno slot libero;
- TLB fault (replace), risolto rimpiazzando uno slot preesistente;
- Invalidazioni del TLB;
- Page Fault;
- Letture dal disco di Swap (anche detto page in);
- Scritture sul disco di Swap (anche detto page out).

Abbiamo definito funzioni per iniziare la registrazione delle statistiche, per fermare la registrazione, per resettare i contatori e per stampare le statistiche. Infine ovviamente abbiamo definito una funzione che aumenta il contatore, in base al tipo di statistica che si vuole registrare (`vm_record_stat`, le chiamate a questa funzione sono state inserite opportunamente all'interno dei vari file relativi alla nostra implementazione. 

### Comando `vmstats`
Per fornire un'interfaccia a questo meccanismo, abbiamo introdotto a `menu.c` il comando `vmstats`, il quale chiama le funzioni definite nel file `vmstats.c`. Il comando viene invocato scrivendo `vmstats <flag>` sul menu, dove `<flag>` può essere:

- `--start`: inizia la registrazione delle statistiche sulla VM;
- `--stop`: termina la registrazione delle statistiche;
- `--reset`: resetta tutti i contatori delle statistiche;
- `--print`: stampa le statistiche raccolte;

### Risultati dei test
Abbiamo eseguito tutti i test lato utente nella cartella `testbin/` relativi al funzionamento della VM, i risultati sono riportati nella seguente tabella.

| Test | RAM size | Tempo (s) | TLB fault (free) | TLB fault (replace) | TLB Invalidation | Page fault | Swap read (page in) | Swap write (page out) |
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
| parallelVM | 512K | 523.375 | 16858 | 0 | 32098 | 5242 | 5070 | 5211 |
| parallelVM | 1M | 349.415 | 18806 | 0 | 21821 | 3494 | 3156 | 3325 |
| parallelVM | 2M | 12.830 | 8615 | 0 | 3095 | 346 | 5 | 16 |
| sort | 512K | 160.582 | 1354 | 561 | 1291 | 1756 | 1463 | 1683 |
| sort | 1M | 57.310 | 206 | 1728 | 143 | 814 | 521 | 614 |
| sort | 2M | 3.538 | 64 | 1845 | 1 | 293 | 0 | 0 |

### Analisi e Valutazione delle Prestazioni

#### Fenomeno del Thrashing e test strided (ctest e huge)
Il test ctest evidenza la vulnerabilità dell'algoritmo di rimpiazzamento FIFO globale in presenza di accessi ciclici a un'area di memoria più grande della RAM fisica disponibile.
- Con 2MB di RAM, l'intero array da 1MB e il codice del processo risiedono in memoria contemporaneamente. Non si verificano operazioni di swap e l'esecuzione richiede soltanto 13 secondi.
- Con 1MB e 512KB di RAM, lo spazio fisico è inferiore alla dimensione dell'array più il kernel. L'accesso strided ciclico del test comporta che ad ogni iterazione la pagina virtuale richiesta sia proprio quella sfrattata più di recente. Si innesca una condizione di thrashing continuo in cui quasi ogni accesso in memoria solleva un page fault che richiede la scrittura del frame vittima su swap e la lettura del frame richiesto dal disco di swap. Il tempo complessivo di esecuzione sale a oltre 11,000 secondi.
Il test huge mostra una situazione analoga, ma poiché la dimensione totale dell'array è di 8MB, la memoria fisica di 2MB non è comunque sufficiente per contenere i dati del processo, portando ad un tempo di esecuzione simile in tutte e tre le configurazioni a causa del thrashing inevitabile indotto dalla dimensione dei dati.

#### Località spaziale in Quicksort (sort)
Il test sort ordina un array da 576KB appoggiandosi ad un array temporaneo delle stesse dimensioni (per un totale di 1.125MB).
- Con 2MB di RAM l'intero set di dati risiede stabilmente in memoria (0 operazioni di swap).
- Riducendo la memoria fisica a 1MB, si osserva un aumento contenuto delle letture e scritture da swap (521 letture, 614 scritture). L'algoritmo quicksort opera dividendo ricorsivamente l'array in partizioni. Non appena la dimensione delle sotto-partizioni scende al di sotto della RAM fisica disponibile al processo, l'ordinamento in loco avviene senza generare ulteriori page fault, ordinando localmente e riducendo l'overhead di I/O rispetto ad una scansione lineare globale.
- Riducendo ulteriormente la memoria a 512KB, la soglia a cui le partizioni risiedono completamente in RAM si abbassa drasticamente, obbligando il sistema a ricorrere molto più frequentemente allo swap (1,463 letture) e triplicando il tempo di esecuzione (da 57 a 160 secondi).

#### Allocazione stack e limiti di memoria fisica (parallelVM e forktest)
Il test parallelVM crea 24 processi figli paralleli, ciascuno dei quali esegue moltiplicazioni di matrici.
- Nella configurazione a 512KB, 11 sottoprocessi falliscono con errore di out-of-memory. Ciascun processo richiede l'allocazione di una pagina non scambiabile per lo stack del kernel. Sotto forte pressione di memoria, la coremap esaurisce i frame fisici allocabili forzando il fallimento della chiamata fork.
- Con 1MB e 2MB di RAM, lo spazio fisico è sufficiente per ospitare i blocchi di controllo e i descrittori di tutti i processi. Con 2MB, il sistema sperimenta un numero irrilevante di swap-out (16 scritture, 5 letture) e completa il calcolo in soli 12.8 secondi.
- Il test forktest sotto 512KB presenta un numero di swap read (34) maggiore dei page fault (28). Questo accade perché `pagetable_copy` legge dallo swap del padre e popola la pagina del figlio chiamando direttamente `swap_read`, evitando il sollevamento dell'eccezione hardware gestita da `vm_fault` che incrementerebbe il contatore di page fault.

