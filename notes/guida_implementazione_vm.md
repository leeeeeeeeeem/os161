# Guida Architetturale: Sistema di Memoria Virtuale (VM) in OS/161

Questa guida approfondisce l'architettura delle strutture dati, le interazioni tra i componenti e la logica interna delle funzioni più complesse del sistema VM attuale.

---

## 1. Architettura delle Strutture Dati

Il sistema si basa sull'interazione di tre livelli di astrazione: lo spazio di indirizzamento, la tabella delle pagine e la mappa della memoria fisica.

### 1.1. Lo Spazio di Indirizzamento (`struct addrspace`)
Ogni processo ha il proprio `addrspace`, che funge da contenitore per:
- **`regions`**: Una lista collegata di `struct region`. Ogni regione definisce un segmento di memoria virtuale continuo (es. `.text` o `.data`).
    - *Logica di allineamento*: In `as_define_region`, gestiamo l'allineamento alle pagine arrotondando l'indirizzo base per difetto e aumentando la dimensione per coprire l'intera area richiesta.
- **`pagetable`**: Puntatore alla directory delle pagine (`struct pagedir`).
- **Stack**: Definito da `stack_base` (solitamente `0x80000000`) e una dimensione fissa di 16 pagine.

### 1.2. Tabella delle Pagine a Due Livelli
Abbiamo implementato una struttura gerarchica per risparmiare memoria (evitando di allocare tabelle per spazi virtuali non utilizzati):
1. **Directory (`pagedir`)**: Un array di 1024 puntatori a tabelle di secondo livello.
2. **Tabella di secondo livello (`pagetable`)**: Un array di 1024 voci, dove ogni voce contiene l'indirizzo fisico (`paddr_t`) di un frame.
- **Index Bitmasking**: Usiamo macro come `GET_DIR_INDEX` (bit 31-22) e `GET_PT_INDEX` (bit 21-12) per navigare la struttura partendo da un indirizzo virtuale.

### 1.3. La Coremap (`struct coremap_entry`)
È l'anagrafe della RAM fisica. Ogni elemento rappresenta un frame di 4KB:
- **Stati**: `FIXED` (kernel immutabile), `FREE` (disponibile), `IN_USE` (allocato a un processo).
- **`chunk_size`**: Memorizza quante pagine contigue sono state allocate insieme. Questo è fondamentale per la `free_kpages`, che riceve solo l'indirizzo iniziale e deve sapere quante pagine liberare.

---

## 2. Analisi della Logica Complessa

### 2.1. `vm_fault`: Il Vigile Urbano della Memoria
Questa funzione viene invocata dall'hardware durante un'eccezione. La sua logica segue passi critici:
1. **Ricerca Regione**: Scandisce la lista `as->regions`. Se l'indirizzo non è in una regione, controlla se cade nell'area dello stack. Se fallisce entrambi, restituisce `EFAULT` (segmentation fault).
2. **Controllo Permessi**: Utilizza uno `switch` sul tipo di errore (`READ`, `WRITE`, `READONLY`). Se un processo tenta di scrivere in una regione definita come `read-only`, il kernel lo intercetta qui.
3. **Aggiornamento TLB**: 
    - Usa `tlb_probe` per vedere se l'indirizzo è già nel TLB (magari è un errore di permessi su una voce esistente).
    - Se esiste, lo sovrascrive con `tlb_write`.
    - Se è un nuovo inserimento, usa `tlb_random` per rimpiazzare una voce a caso.
    - **Bit Dirty**: Impostiamo il bit `TLBLO_DIRTY` solo se la regione è scrivibile, permettendo all'hardware di sollevare un'eccezione se si tenta di scrivere su una pagina "pulita" (essenziale per futuri algoritmi di swap).

### 2.2. `pagetable_translate`: Allocazione Lazy
La funzione non si limita a tradurre, ma "crea" la memoria se manca:
- Se la tabella di secondo livello non esiste, la alloca al volo.
- Se la voce della tabella è `0`, chiama `alloc_kpages(1)` per ottenere un frame fisico, lo azzera con `bzero` (per sicurezza, evitando che un processo legga dati di un altro) e lo registra.
- Questo approccio garantisce che la memoria fisica venga consumata solo quando effettivamente toccata dal programma.

### 2.3. `coremap_alloc`: Ricerca di Spazio Contiguo
Implementa un algoritmo "First Fit":
- Scorre la coremap cercando `npages` consecutive nello stato `FREE`.
- Una volta trovate, marca la prima col valore di `chunk_size` e tutte le altre come `IN_USE`.
- Questo permette al kernel di allocare buffer contigui (necessari per alcune operazioni hardware o DMA).

---

## 3. Flusso di Esecuzione: Da Indirizzo Virtuale a Fisico

Ecco cosa succede quando un programma esegue `lw v0, 0(a0)`:

1. **CPU**: Cerca l'indirizzo virtuale contenuto in `a0` nel **TLB**.
2. **TLB Miss**: Se non c'è, la CPU salta al gestore delle eccezioni del kernel.
3. **Kernel**: Identifica il tipo di errore e chiama `vm_fault`.
4. **`vm_fault`**:
    - Trova la `region` associata (es. Segmento Dati).
    - Verifica che il processo abbia i permessi di lettura.
    - Chiama `pagetable_translate`.
5. **`pagetable_translate`**:
    - Naviga `pagedir` -> `pagetable`.
    - Se è la prima volta che si accede a quella pagina, alloca un frame fisico tramite la **Coremap**.
    - Restituisce l'indirizzo fisico.
6. **`vm_fault`**: Prepara la entry TLB (indirizzo fisico + bit di validità/dirty) e la scrive nell'hardware.
7. **CPU**: Ripristina l'esecuzione. Ora la `lw` trova l'indirizzo nel TLB e accede alla RAM fisica istantaneamente.

---

## 4. Integrazione tra i Componenti

Il successo del sistema dipende dalla coerenza tra queste strutture:
- Se `as_destroy` viene chiamato, deve pulire la `pagetable`.
- La `pagetable_destroy` deve chiamare `free_kpages` per ogni frame valido.
- `free_kpages` deve informare la `coremap` che quei frame sono di nuovo `FREE`.
- Se questa catena si rompe, si verificano **memory leak** che esaurirebbero la RAM in pochi secondi durante i test intensivi come `matmult`.
