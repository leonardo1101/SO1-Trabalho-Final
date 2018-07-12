#include "param.h"
#include "types.h"
#include "defs.h"
#include "x86.h"
#include "memlayout.h"
#include "mmu.h"
#include "proc.h"
#include "elf.h"
#include "spinlock.h"

//=============================== COW ================================
#define TAMSHARETABLE PHYSTOP >> 12 //O maximo de memória que pode ser mapeada
// Tabela que informa a quantidade de processos que estão utilizando o mesmo espaço de memória
static int shareTable[TAMSHARETABLE]; // tabela que possui entrada para todas as paginas da memoria

// Estura utilizada para bloquear o código crítico de quando é necessário
// Realizar mudanças na shareTable
struct spinlock tablelock;

// Configura o tablelock e inicia a tabela de compartilhamento de paginas
void sharetableinit(void)
{
  initlock(&tablelock, "sharetable");
  int i;

  acquire(&tablelock);
  for(i=0; i< TAMSHARETABLE; i++){
    shareTable[i]=0;
  }
  release(&tablelock);

  cprintf("Inicializacao da ShareTable concluida\n");
}

int getCountPPN(uint pa){

  int index = (pa >> 12) & 0xFFFFF; // recupera o PPN do PA passado
  return shareTable[index]; // Retorna o numero de processos que estão compartilhando a mesma pagina de memoria fisica
}

void incCountPPN(uint pa){

  int index = (pa >> 12) & 0xFFFFF; // recupera o PPN do PA passado
  shareTable[index]++; // Incrementa o numero de processos que estão compartilhando a posiçao de memória
}

void decCountPPN(uint pa){

  int index = (pa >> 12) & 0xFFFFF; // recupera o PPN do PA passado
  shareTable[index]--; // Decrementa o numero de compartilhamento quando um dos processos deixa de utlizar a posicao de memoria
}
//===============================================================


extern char data[];  // defined by kernel.ld
pde_t *kpgdir;  // for use in scheduler()

// Set up CPU's kernel segment descriptors.
// Run once on entry on each CPU.
void seginit(void)
{
  struct cpu *c;

  // Map "logical" addresses to virtual addresses using identity map.
  // Cannot share a CODE descriptor for both kernel and user
  // because it would have to have DPL_USR, but the CPU forbids
  // an interrupt from CPL=0 to DPL=3.
  c = &cpus[cpunum()];
  c->gdt[SEG_KCODE] = SEG(STA_X|STA_R, 0, 0xffffffff, 0);
  c->gdt[SEG_KDATA] = SEG(STA_W, 0, 0xffffffff, 0);
  c->gdt[SEG_UCODE] = SEG(STA_X|STA_R, 0, 0xffffffff, DPL_USER);
  c->gdt[SEG_UDATA] = SEG(STA_W, 0, 0xffffffff, DPL_USER);

  // Map cpu and proc -- these are private per cpu.
  c->gdt[SEG_KCPU] = SEG(STA_W, &c->cpu, 8, 0);

  lgdt(c->gdt, sizeof(c->gdt));
  loadgs(SEG_KCPU << 3);

  // Initialize cpu-local storage.
  cpu = c;
  proc = 0;
}

// Return the address of the PTE in page table pgdir
// that corresponds to virtual address va.  If alloc!=0,
// create any required page table pages.
static pte_t * walkpgdir(pde_t *pgdir, const void *va, int alloc)
{
  pde_t *pde;
  pte_t *pgtab;

  pde = &pgdir[PDX(va)];
  if(*pde & PTE_P){
    pgtab = (pte_t*)P2V(PTE_ADDR(*pde));
  } else {
    if(!alloc || (pgtab = (pte_t*)kalloc()) == 0)
    return 0;
    // Make sure all those PTE_P bits are zero.
    memset(pgtab, 0, PGSIZE);
    // The permissions here are overly generous, but they can
    // be further restricted by the permissions in the page table
    // entries, if necessary.
    *pde = V2P(pgtab) | PTE_P | PTE_W | PTE_U;
  }
  return &pgtab[PTX(va)];
}

// Create PTEs for virtual addresses starting at va that refer to
// physical addresses starting at pa. va and size might not
// be page-aligned.
static int mappages(pde_t *pgdir, void *va, uint size, uint pa, int perm)
{
  char *a, *last;
  pte_t *pte;

  a = (char*)PGROUNDDOWN((uint)va);
  last = (char*)PGROUNDDOWN(((uint)va) + size - 1);
  for(;;){
    if((pte = walkpgdir(pgdir, a, 1)) == 0)
    return -1;
    if(*pte & PTE_P)
    panic("remap");
    *pte = pa | perm | PTE_P;
    if(a == last)
    break;
    a += PGSIZE;
    pa += PGSIZE;
  }
  return 0;
}

// There is one page table per process, plus one that's used when
// a CPU is not running any process (kpgdir). The kernel uses the
// current process's page table during system calls and interrupts;
// page protection bits prevent user code from using the kernel's
// mappings.
//
// setupkvm() and exec() set up every page table like this:
//
//   0..KERNBASE: user memory (text+data+stack+heap), mapped to
//                phys memory allocated by the kernel
//   KERNBASE..KERNBASE+EXTMEM: mapped to 0..EXTMEM (for I/O space)
//   KERNBASE+EXTMEM..data: mapped to EXTMEM..V2P(data)
//                for the kernel's instructions and r/o data
//   data..KERNBASE+PHYSTOP: mapped to V2P(data)..PHYSTOP,
//                                  rw data + free physical memory
//   0xfe000000..0: mapped direct (devices such as ioapic)
//
// The kernel allocates physical memory for its heap and for user memory
// between V2P(end) and the end of physical memory (PHYSTOP)
// (directly addressable from end..P2V(PHYSTOP)).

// This table defines the kernel's mappings, which are present in
// every process's page table.
static struct kmap {
  void *virt;
  uint phys_start;
  uint phys_end;
  int perm;
} kmap[] = {
  { (void*)KERNBASE, 0,             EXTMEM,    PTE_W}, // I/O space
  { (void*)KERNLINK, V2P(KERNLINK), V2P(data), 0},     // kern text+rodata
  { (void*)data,     V2P(data),     PHYSTOP,   PTE_W}, // kern data+memory
  { (void*)DEVSPACE, DEVSPACE,      0,         PTE_W}, // more devices
};

// Set up kernel part of a page table.
pde_t*
setupkvm(void)
{
  pde_t *pgdir;
  struct kmap *k;

  if((pgdir = (pde_t*)kalloc()) == 0)
  return 0;
  memset(pgdir, 0, PGSIZE);
  if (P2V(PHYSTOP) > (void*)DEVSPACE)
    panic("PHYSTOP too high");
  for(k = kmap; k < &kmap[NELEM(kmap)]; k++){
    if(mappages(pgdir, k->virt, k->phys_end - k->phys_start,
      (uint)k->phys_start, k->perm) < 0)
      return 0;
    }
    return pgdir;
  }

  // Allocate one page table for the machine for the kernel address
  // space for scheduler processes.
  void kvmalloc(void)
  {
    kpgdir = setupkvm();
    switchkvm();
  }

  // Switch h/w page table register to the kernel-only page table,
  // for when no process is running.
  void switchkvm(void)
  {
    lcr3(V2P(kpgdir));   // switch to the kernel page table
  }

  // Switch TSS and h/w page table to correspond to process p.
  void switchuvm(struct proc *p)
  {
    if(p == 0)
    panic("switchuvm: no process");
    if(p->kstack == 0)
    panic("switchuvm: no kstack");
    if(p->pgdir == 0)
    panic("switchuvm: no pgdir");

    pushcli();
    cpu->gdt[SEG_TSS] = SEG16(STS_T32A, &cpu->ts, sizeof(cpu->ts)-1, 0);
    cpu->gdt[SEG_TSS].s = 0;
    cpu->ts.ss0 = SEG_KDATA << 3;
    cpu->ts.esp0 = (uint)p->kstack + KSTACKSIZE;
    // setting IOPL=0 in eflags *and* iomb beyond the tss segment limit
    // forbids I/O instructions (e.g., inb and outb) from user space
    cpu->ts.iomb = (ushort) 0xFFFF;
    ltr(SEG_TSS << 3);
    lcr3(V2P(p->pgdir));  // switch to process's address space
    popcli();
  }

  // Load the initcode into address 0 of pgdir.
  // sz must be less than a page.
  void inituvm(pde_t *pgdir, char *init, uint sz)
  {
    char *mem;

    if(sz >= PGSIZE)
    panic("inituvm: more than a page");
    mem = kalloc();
    memset(mem, 0, PGSIZE);
    mappages(pgdir, 0, PGSIZE, V2P(mem), PTE_W|PTE_U);
    memmove(mem, init, sz);
  }

  // Load a program segment into pgdir.  addr must be page-aligned
  // and the pages from addr to addr+sz must already be mapped.
  int loaduvm(pde_t *pgdir, char *addr, struct inode *ip, uint offset, uint sz, uint perm)
  {
    uint i, pa, n;
    pte_t *pte;

    if((uint) addr % PGSIZE != 0)
    panic("loaduvm: addr must be page aligned");
    for(i = 0; i < sz; i += PGSIZE){
      if((pte = walkpgdir(pgdir, addr+i, 0)) == 0)
      panic("loaduvm: address should exist");
      pa = PTE_ADDR(*pte);

      perm = perm & PTE_W;
      if(perm){
        *pte |= perm;
      }
      else{
        *pte &= ~PTE_W;
      }
      if(sz - i < PGSIZE)
      n = sz - i;
      else
      n = PGSIZE;
      if(readi(ip, P2V(pa), offset+i, n) != n)
      return -1;
    }
    return 0;
  }

  // Allocate page tables and physical memory to grow process from oldsz to
  // newsz, which need not be page aligned.  Returns new size or 0 on error.
  int allocuvm(pde_t *pgdir, uint oldsz, uint newsz)
  {
    char *mem;
    uint a;

    if(newsz >= KERNBASE)
    return 0;
    if(newsz < oldsz)
    return oldsz;

    a = PGROUNDUP(oldsz);
    for(; a < newsz; a += PGSIZE){
      mem = kalloc();
      if(mem == 0){
        cprintf("allocuvm out of memory\n");
        deallocuvm(pgdir, newsz, oldsz);
        return 0;
      }
      memset(mem, 0, PGSIZE);
      if(mappages(pgdir, (char*)a, PGSIZE, V2P(mem), PTE_W|PTE_U) < 0){
        cprintf("allocuvm out of memory (2)\n");
        deallocuvm(pgdir, newsz, oldsz);
        kfree(mem);
        return 0;
      }
    }
    return newsz;
  }

  // Deallocate user pages to bring the process size from oldsz to
  // newsz.  oldsz and newsz need not be page-aligned, nor does newsz
  // need to be less than oldsz.  oldsz can be larger than the actual
  // process size.  Returns the new process size.
  int deallocuvm(pde_t *pgdir, uint oldsz, uint newsz)
  {
    pte_t *pte;
    uint a, pa;

    if(newsz >= oldsz)
    return oldsz;

    a = PGROUNDUP(newsz);
    for(; a  < oldsz; a += PGSIZE){
      pte = walkpgdir(pgdir, (char*)a, 0);
      if(!pte)
      a = PGADDR(PDX(a) + 1, 0, 0) - PGSIZE;
      else if((*pte & PTE_P) != 0){
        pa = PTE_ADDR(*pte);
        if(pa == 0)
        panic("kfree");
        char *v = P2V(pa);
        kfree(v);
        *pte = 0;
      }
    }
    return newsz;
  }

  // Free a page table and all the physical memory pages
  // in the user part.
  void freevm(pde_t *pgdir)
  {
    uint i;

    if(pgdir == 0)
    panic("freevm: no pgdir");
    deallocuvm(pgdir, KERNBASE, 0);
    for(i = 0; i < NPDENTRIES; i++){
      if(pgdir[i] & PTE_P){
        char * v = P2V(PTE_ADDR(pgdir[i]));
        kfree(v);
      }
    }
    kfree((char*)pgdir);
  }

  // Clear PTE_U on a page. Used to create an inaccessible
  // page beneath the user stack.
  void clearpteu(pde_t *pgdir, char *uva)
  {
    pte_t *pte;

    pte = walkpgdir(pgdir, uva, 0);
    if(pte == 0)
    panic("clearpteu");
    *pte &= ~PTE_U;
  }

  // Given a parent process's page table, create a copy
  // of it for a child.
  pde_t* copyuvm(pde_t *pgdir, uint sz)
  {
    pde_t *d;
    pte_t *pte;
    uint pa, i, flags;
    char *mem;

    if((d = setupkvm()) == 0)
    return 0;

    for(i = PGSIZE; i < sz; i += PGSIZE){
      if((pte = walkpgdir(pgdir, (void *) i, 0)) == 0)
      panic("copyuvm: pte should exist");
      if(!(*pte & PTE_P))
      panic("copyuvm: page not present");
      pa = PTE_ADDR(*pte);
      flags = PTE_FLAGS(*pte);
      if((mem = kalloc()) == 0)
      goto bad;
      memmove(mem, (char*)P2V(pa), PGSIZE);
      if(mappages(d, (void*)i, PGSIZE, V2P(mem), flags) < 0)
      goto bad;
    }
    return d;

    bad:
    freevm(d);
    return 0;
  }

  //PAGEBREAK!
  // Map user virtual address to kernel address.
  char*
  uva2ka(pde_t *pgdir, char *uva)
  {
    pte_t *pte;

    pte = walkpgdir(pgdir, uva, 0);
    if((*pte & PTE_P) == 0)
    return 0;
    if((*pte & PTE_U) == 0)
    return 0;
    return (char*)P2V(PTE_ADDR(*pte));
  }

  // Copy len bytes from p to user address va in page table pgdir.
  // Most useful when pgdir is not the current page table.
  // uva2ka ensures this only works for PTE_U pages.
  int
  copyout(pde_t *pgdir, uint va, void *p, uint len)
  {
    char *buf, *pa0;
    uint n, va0;

    buf = (char*)p;
    while(len > 0){
      va0 = (uint)PGROUNDDOWN(va);
      pa0 = uva2ka(pgdir, (char*)va0);
      if(pa0 == 0)
      return -1;
      n = PGSIZE - (va - va0);
      if(n > len)
      n = len;
      memmove(pa0 + (va - va0), buf, n);
      len -= n;
      buf += n;
      va = va0 + PGSIZE;
    }
    return 0;
  }

  //PAGEBREAK!
  // Blank page.
  //PAGEBREAK!
  // Blank page.
  //PAGEBREAK!
  // Blank page.

//=========================== COW ===========================

pde_t* share_cow(pde_t *pgdir, uint sz)
{
  pde_t *d;
  pte_t *pte;
  uint pa, i, flags;

  if((d = setupkvm()) == 0)
    return 0;

  acquire(&tablelock);
  for(i = PGSIZE; i < sz; i += PGSIZE){
    if((pte = walkpgdir(pgdir, (void *) i, 0)) == 0)
      panic("copyuvm: pte should exist");
    if(!(*pte & PTE_P))
      panic("copyuvm: page not present");
    *pte &= ~PTE_W; // torna read-only (desabilita a escrita)
    *pte |= PTE_SHARE; // Indica que a página é compartilhada
    pa = PTE_ADDR(*pte);
    flags = PTE_FLAGS(*pte);

    // instead of create new pages, remap the pages for cow child
    if(mappages(d, (void*)i, PGSIZE, pa, flags) < 0)
      goto bad;

    if(getCountPPN(pa) == 0){
      incCountPPN(pa);
      incCountPPN(pa);
    }
    else{
      incCountPPN(pa);
    }

    // cprintf("pid: %d index: %p count: %d\n", proc->pid, pa, getCountPPN(pa));
  }
  release(&tablelock);

  lcr3(V2P(proc->pgdir)); // atualiza o TLB

  return d;

bad:
  freevm(d);
  return 0;
}

void handle_pgflt(void){
  // Recupera o endereço virtual onde ocorreu o pagefault (armazenado no registrador cr2)
  uint addr = rcr2();
  // Se o endereço que tentou ser acessado foi o 0 - avisar que foi um
  // null pointer Exception
  if (addr == 0) {
    cprintf("Segmentation Fault - Null Pointer Dereference\n");
    kill(proc->pid);
  }
  // Se o processo possui paginas compartilhadas, realiza a cópia da memoria
  // que causou o pagefault (por ser read only)
  else{
    // Recupera o Page Table Entry do endereço acima para o processo atual
    pte_t* pte = walkpgdir(proc->pgdir, (void *) addr, 0);

    if(PTE_FLAGS(*pte)&PTE_SHARE){
        copyuvm_cow(addr);
        cprintf("Page Fault: cowfork \n");
    }
    else{
      cprintf("Segmentation Fault - Writing to Read-only Memory\n");
      kill(proc->pid);
    }
  }
}

int copyuvm_cow(uint addr)
{
  uint pa;
  pte_t *pte;
  char *mem;

  // Recupera o Page Table Entry do endereço acima para o processo atual
  pte = walkpgdir(proc->pgdir, (void *) addr, 0);
  pa = PTE_ADDR(*pte);

  acquire(&tablelock);
  // Se pagina está sendo compartilhada
  if (getCountPPN(pa) > 1) {
    if((mem = kalloc()) == 0) // aloca uma nova página de memoria
      goto bad;
    memmove(mem, (char*)P2V(pa), PGSIZE);
    *pte &= 0xFFF; // pega todas as flags de pte
    *pte &= ~PTE_SHARE; // retira a flag de compartilhamento
    *pte |= V2P(mem) | PTE_W; // insere a permissão de escrita na nova pagina de memória
    decCountPPN(pa); // Decrementa a quantidade de processos que estão compartilhando a mesma memória
  }
  // Se há apenas um processo usando a pagina, basta dar permissão para escrita e retira a flag de compartilhamento
  else {
    *pte |= PTE_W;
    *pte &= ~PTE_SHARE;
  }

  release(&tablelock);

  lcr3(V2P(proc->pgdir)); // flush the TLB

  return 1;

bad:
  return 0;
}
// Desaloca a memoria virtual apontada por pgdir
int deallocuvm_cow(pde_t *pgdir, uint oldsz, uint newsz)
{
  pte_t *pte;
  uint a, pa;

  if(newsz >= oldsz)
    return oldsz;

  a = PGROUNDUP(newsz);
  // Será realizada mudanças na shareTable, é necessário bloquea-la, para que não haja problema de concorrência e a torne inválida
  acquire(&tablelock);
  for(; a < oldsz; a += PGSIZE){
    pte = walkpgdir(pgdir, (char*)a, 0);
    if(!pte)
      a += (NPTENTRIES - 1) * PGSIZE;
    else if((*pte & PTE_P) != 0){
      pa = PTE_ADDR(*pte);
      if(pa == 0)
        panic("kfree");
      // Se a memoria está sendo compartilhada, decrementa a quantidade de processos que estão compartilhando
      // Pois está sendo desalocada do processo atual
      if (getCountPPN(pa) > 1) {
        decCountPPN(pa);
      }
      // se a memoria não está sendo compartilhada com nenhum outro processo
      // pode ser liberada completamente
      else {
        char *v = P2V(pa);
        kfree(v);
        decCountPPN(pa);
      }
      // Faz o ponteiro para page table entry apontar para null
      *pte = 0;
    }
  }
  release(&tablelock);
  return newsz;
}
// Libera a memoria virtual de um processo que utilizou o share_cow
void freevm_cow(pde_t *pgdir)
{
  uint i;

  if(pgdir == 0)
    panic("freevm: no pgdir");
  // Desaloca as page tabels
  deallocuvm_cow(pgdir, KERNBASE, 0);

  // desaloca os page directories
  for(i = 0; i < NPDENTRIES; i++){
    if(pgdir[i] & PTE_P){
      char *v = P2V(PTE_ADDR(pgdir[i]));
      kfree(v);
    }
  }
  kfree((char*)pgdir);
}
// ==========================================================================
