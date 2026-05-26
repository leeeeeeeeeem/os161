# Guida: Cosa Implementare per Eseguire i Programmi Utente e Testare la VM

Attualmente l'implementazione della tua Virtual Memory è solida nella gestione delle tabelle delle pagine e nella traduzione degli indirizzi, ma **i programmi utente non riusciranno a funzionare correttamente** (o crasheranno il sistema) a causa di una serie di problematiche architetturali e limitazioni non gestite al di fuori della logica puramente legata alla memoria.

Ecco un'analisi dettagliata e una lista delle cose **che devi implementare** per farli girare in modo sicuro ed efficiente.

---

## 1. Risolvere il Bug Critico dello Stack del Kernel (`sys_write` / `sys_read`)

**Il Problema:**
Nei file `syscalls.c`, utilizzi VLA (Variable Length Arrays) come:
```c
char kernel_buf[size]; // sys_read
char kernel_buf[size + 1]; // sys_write
```
Questo alloca la memoria dinamicamente sullo stack del thread del kernel. Il problema è che in OS/161 **lo stack del kernel è strettamente limitato a una singola pagina (4096 byte)**. Se un programma utente tenta di scrivere un buffer di 5000 byte tramite una singola operazione `write()`, questa allocazione causerà immediatamente uno **Stack Overflow** del kernel, portando la macchina a un Kernel Panic, corrompendo la struttura del thread.

**La Soluzione:**
- Rimuovi i VLA. 
- Alloca la memoria usando `kmalloc(size + 1)`, effettuando il controllo su eventuali errori di memoria terminando l'operazione in caso non vi sia spazio.
- **Alternativa migliore (Chunking):** Implementa un buffer fisso piccolo sullo stack (es. `char buf[128]`) e copia i dati dall'utente 128 byte alla volta usando un ciclo, invocando ripetutamente `copyin` e `putch`.

---

## 2. Invalida il TLB dopo aver caricato l'eseguibile (ELF)

**Il Problema:**
Quando un ELF viene caricato (`load_elf`), OS/161 chiama prima `as_prepare_load()`, che imposta forzatamente tutti i permessi delle regioni su scrivibili (`writeable = 1`) temporaneamente. Nel mentre, `load_segment` copia i dati nei segmenti innescando le chiamate a `vm_fault()`. Queste verranno scritte nel TLB hardware con il flag **`TLBLO_DIRTY`** (che su MIPS significa Write-Enabled).
Alla fine viene chiamato `as_complete_load()`, che ripristina correttamente i permessi in sola lettura (per il segmento di testo). **Tuttavia, il TLB contiene ancora le entry "sporche/scrivibili".** Di conseguenza, se un programma tenta accidentalmente di sovrascrivere il proprio codice, il TLB glielo permetterà senza causare un page fault.

**La Soluzione:**
Aggiungi in `as_complete_load()` (o subito dopo aver caricato l'eseguibile) una chiamata ad `as_activate()`. Questa funzione si occuperà di fare il flush (invalidazione) completo del TLB forzando la ricarica con i permessi corretti alla prima esecuzione del codice utente.

---

## 3. Gestire gli Argomenti (`argc` e `argv`) in `runprogram`

**Il Problema:**
La funzione `runprogram()` (in `src/kern/syscall/runprogram.c`) attualmente salta al livello utente tramite la chiamata:
```c
enter_new_process(0, NULL, NULL, stackptr, entrypoint);
```
Passando sempre `argc = 0` e `argv = NULL`, programmi che richiedono argomenti da terminale andranno in segment fault non appena proveranno a dereferenziare `argv[1]`.

**La Soluzione:**
1. Rimodella `runprogram()` (e parallelamente implementa la syscall `execv()`) affinché riceva una lista di parametri.
2. Calcola la lunghezza totale di tutte le stringhe di argomenti, più i puntatori (`char*`).
3. Muovi l'indirizzo dello `stackptr` verso il basso per fare spazio a questi dati.
4. Usa `copyoutstr` per copiare le stringhe fisicamente nel nuovo spazio dello stack utente, e posiziona i rispettivi indirizzi nell'array `argv` sempre sullo stack utente.
5. Allinea i puntatori della memoria a 4 byte prima di passarli a `enter_new_process`.

---

## 4. Implementare la Regione di Heap e la Syscall `sbrk`

**Il Problema:**
La struttura `addrspace` ora mappa solo Segmento di Testo, Dati e Stack. Non esiste una definizione per la regione Heap. I programmi più intensivi che testano pesantemente la VM (es. `matmult` o `sort`) fanno largo uso di allocazioni dinamiche tramite `malloc()`. Questo richiederà nativamente la syscall `sys_sbrk`, non ancora implementata, altrimenti la VM considererà lo spazio oltre il segmento dati invalido bloccando il programma (EFAULT).

**La Soluzione:**
1. Nella `struct addrspace`, definisci un `heap_start` (uguale alla fine dell'ultimo segmento BSS) ed un `heap_end` (che inizialmente è uguale ad `heap_start`). Entrambi allineati alla pagina.
2. In `vm_fault`, aggiungi il controllo: se l'indirizzo rientra tra `heap_start` e `heap_end`, dev'essere trattato come regione valida ed essere tradotto (è sempre writeable).
3. Implementa `sys_sbrk(intptr_t amount)` che riceve la quantità di byte di cui allargare l'heap. Modifica `heap_end`, verifica che non vada in collisione con lo stack e ritorna il vecchio indirizzo `heap_end`.

---

## 5. Terminazione Morbida al Page Fault (Invece del Panic)

**Il Problema:**
Attualmente, se un processo cerca di accedere a un'area di memoria invalida, `vm_fault` ritorna `EFAULT` o panic. Se il sistema hardware riceve un `EFAULT` da un exception handler di memoria in kernel space, causerà inesorabilmente un **"Fatal Exception"** bloccando completamente il sistema OS/161.

**La Soluzione:**
Non devi far crashare il kernel se è un programma utente a fare una scemenza. In `vm_fault` o nel general trap (es. in `mips_trap`), quando rilevi un errore fatale di indirizzamento da parte di un processo utente (es. scrivere in read-only o fuori dalle region), il comportamento corretto è chiamare internamente qualcosa di simile a `sys_exit(-1)`. In questo modo distruggi solo il processo problematico e rilasci la memoria, ma tieni il kernel stabile e gli altri programmi in esecuzione intatti.

---

## 6. Sincronizzazione dell'esecuzione (`waitpid`)

**Il Problema:**
Attualmente quando lanci dal menu usando `p /testbin/palin`, la funzione `common_prog` crea un nuovo processo e ritorna immediatamente. Questo causa:
- Output sovrapposto (il menu torna chiedendoti comandi mentre il programma sta ancora stampando).
- Nessuno pulisce la struttura base `proc` se non lo fai te (il memory leak del proc causerà esaurimento ram alla lunga).

**La Soluzione:**
Implementa la syscall `waitpid`. Dopo aver istanziato il thread via `thread_fork`, il thread chiamante (il menu del kernel) deve chiamare una funzione che dorme finché il figlio (il programma appena spawnato) non ha terminato l'esecuzione, ad esempio sfruttando un semaforo, cv, o lock implementato appositamente per le gerarchie di processi. Solo a quel punto devi chiamare la deallocazione di `proc`.
