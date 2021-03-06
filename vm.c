#include "param.h"
#include "types.h"
#include "defs.h"
#include "mips.h"
#include "memlayout.h"
#include "mmu.h"
#include "proc.h"
#include "elf.h"

extern char data[];  // defined by kernel.ld
pde_t *kpgdir;  // for use in scheduler()
pde_t *curpgdir; // current page directory

// Return the address of the PTE in page table pgdir
// that corresponds to virtual address va.  If alloc!=0,
// create any required page table pages.
static pte_t *
walkpgdir(pde_t *pgdir, char asid, const void *va, int alloc, int check_asid)
{
  pde_t *pde;
  pte_t *pgtab;

  pde = &pgdir[PDX(va)];
  if(!*pde)
    goto alloc;
  pgtab = (pte_t*)PDE_ADDR(*pde);
  if(check_asid && !ASID_MATCH(asid, *pde, pgtab[PTX(va)]))
    goto alloc;
  return &pgtab[PTX(va)];

alloc:
  if(!alloc || (pgtab = (pte_t*)kalloc()) == 0)
    return 0;
  // Make sure all those ELO_V bits are zero.
  memset(pgtab, 0, PGSIZE);
  *pde = (ulong)pgtab | asid;
  return &pgtab[PTX(va)];
}

// Create PTEs for virtual addresses starting at va that refer to
// physical addresses starting at pa. va and size might not
// be page-aligned.
static int
mappages(pde_t *pgdir, char asid, void *va, ulong size, ulong pa, int perm)
{
  char *a, *last;
  pte_t *pte;
  ulong elo;
  
  a = (char*)PGROUNDDOWN((ulong)va);
  last = (char*)PGROUNDDOWN(((ulong)va) + size - 1);
  for(;;){
    if((pte = walkpgdir(pgdir, asid, a, 1, 1)) == 0)
      return -1;
    if(PTE_ELO(*pte, ELX(a)) & ELO_V)
      panic("remap");
    elo = (pa >> 6) | perm | ELO_V;
    *pte = ELX(va)? PTE(PTE_ELO(*pte, 0), elo) : PTE(elo, PTE_ELO(*pte, 1));
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
  ulong phys_start;
  ulong phys_end;
  int perm;
} kmap[] = {
 { (void*)KERNBASE, 0,             EXTMEM,    ELO_G | ELO_D}, // I/O space
 { (void*)KERNLINK, V2P(KERNLINK), V2P(data), 0},     // kern text+rodata
 { (void*)data,     V2P(data),     PHYSTOP,   ELO_G | ELO_D}, // kern data+memory
 { (void*)DEVSPACE, DEVSPACE,      0,         ELO_G | ELO_D}, // more devices
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
  if (p2v(PHYSTOP) > (void*)DEVSPACE)
    panic("PHYSTOP too high");
  for(k = kmap; k < &kmap[NELEM(kmap)]; k++)
    if(mappages(pgdir, 0, k->virt, k->phys_end - k->phys_start, 
                (ulong)k->phys_start, k->perm) < 0)
      return 0;
  return pgdir;
}

// Allocate one page table for the machine for the kernel address
// space for scheduler processes.
void
kvmalloc(void)
{
  kpgdir = setupkvm();
  switchkvm();
}

// Switch h/w page table register to the kernel-only page table,
// for when no process is running.
void
switchkvm(void)
{
  curpgdir = kpgdir;   // switch to the kernel page table. No need to translate into physical address because kpgdir is at kseg0.
}

// Switch TSS and h/w page table to correspond to process p.
void
switchuvm(struct proc *p)
{
  pte_t entry_pte;

  pushcli();
  if(p->pgdir == 0)
    panic("switchuvm: no pgdir");
  // register va zero to TLB and switch to new address space
  entry_pte = *(pte_t*)PDE_ADDR(p->pgdir[PDX(0x00000000)]);
  tlbwi(0x00000000 | p->asid, entry_pte);
  curpgdir = p->pgdir;
  popcli();
  return;
}

// Load the initcode into address 0 of pgdir.
// sz must be less than a page.
void
inituvm(pde_t *pgdir, char asid, char *init, ulong sz)
{
  char *mem;
  
  if(sz >= PGSIZE)
    panic("inituvm: more than a page");
  mem = kalloc();
  memset(mem, 0, PGSIZE);
  mappages(pgdir, asid, 0, PGSIZE, v2p(mem), ELO_D);
  memmove(mem, init, sz);
}

// Load a program segment into pgdir.  addr must be page-aligned
// and the pages from addr to addr+sz must already be mapped.
int
loaduvm(pde_t *pgdir, char *addr, struct inode *ip, ulong offset, ulong sz)
{
  ulong i, pa, n;
  pte_t *pte;

  if((ulong) addr % PGSIZE != 0)
    panic("loaduvm: addr must be page aligned");
  for(i = 0; i < sz; i += PGSIZE){
    if((pte = walkpgdir(pgdir, 0, addr+i, 0, 0)) == 0)
      panic("loaduvm: address should exist");
    pa = ELO_ADDR(PTE_ELO(*pte, ELX(addr+i)));
    if(sz - i < PGSIZE)
      n = sz - i;
    else
      n = PGSIZE;
    if(readi(ip, p2v(pa), offset+i, n) != n)
      return -1;
  }
  return 0;
}

// Allocate page tables and physical memory to grow process from oldsz to
// newsz, which need not be page aligned.  Returns new size or 0 on error.
int
allocuvm(pde_t *pgdir, char asid, ulong oldsz, ulong newsz)
{
  char *mem;
  ulong a;

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
    mappages(pgdir, asid, (char*)a, PGSIZE, v2p(mem), ELO_D);
  }
  return newsz;
}

// Deallocate user pages to bring the process size from oldsz to
// newsz.  oldsz and newsz need not be page-aligned, nor does newsz
// need to be less than oldsz.  oldsz can be larger than the actual
// process size.  Returns the new process size.
int
deallocuvm(pde_t *pgdir, ulong oldsz, ulong newsz)
{
  pte_t *pte;
  ulong a, pa;

  if(newsz >= oldsz)
    return oldsz;

  a = PGROUNDUP(newsz);
  for(; a  < oldsz; a += PGSIZE){
    pte = walkpgdir(pgdir, 0, (char*)a, 0, 0);
    if(!pte)
      a += (NPTENTRIES - 1) * PGSIZE;
    else if((PTE_ELO(*pte, ELX(a)) & ELO_V) != 0){
      pa = ELO_ADDR(PTE_ELO(*pte, ELX(a)));
      if(pa == 0)
        panic("kfree");
      char *v = p2v(pa);
      kfree(v);
      *pte = 0;
    }
  }
  return newsz;
}

// Free a page table and all the physical memory pages
// in the user part.
void
freevm(pde_t *pgdir)
{
  ulong i;

  if(pgdir == 0)
    panic("freevm: no pgdir");
  deallocuvm(pgdir, KERNBASE, 0);
  for(i = 0; i < NPDENTRIES; i++){
    if(pgdir[i]){
      char *v = (char*)PDE_ADDR(pgdir[i]);
      kfree(v);
    }
  }
  kfree((char*)pgdir);
}

// Clear PTE_U on a page. Used to create an inaccessible
// page beneath the user stack.
// MIPS does not support memory access control by pages, so this does nothing.
void
clearpteu(pde_t *pgdir, char *uva)
{
}

// Given a parent process's page table, create a copy
// of it for a child.
pde_t*
copyuvm(pde_t *pgdir, char asid, ulong sz)
{
  pde_t *d;
  pte_t *pte;
  ulong pa, i, flags;
  char *mem;

  if((d = setupkvm()) == 0)
    return 0;
  for(i = 0; i < sz; i += PGSIZE){
    if((pte = walkpgdir(pgdir, asid, (void *) i, 0, 0)) == 0)
      panic("copyuvm: pte should exist");
    if(!(PTE_ELO(*pte, ELX(i)) & ELO_V))
      panic("copyuvm: page not present");
    pa = ELO_ADDR(PTE_ELO(*pte, ELX(i)));
    flags = ELO_FLAGS(PTE_ELO(*pte, ELX(i)));
    if((mem = kalloc()) == 0)
      goto bad;
    memmove(mem, (char*)p2v(pa), PGSIZE);
    if(mappages(d, asid, (void*)i, PGSIZE, v2p(mem), flags) < 0)
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

  pte = walkpgdir(pgdir, 0, uva, 0, 0);
  if((PTE_ELO(*pte, ELX(uva)) & ELO_V) == 0)
    return 0;
  if(ELO_ADDR(PTE_ELO(*pte, ELX(uva))) >= KSEG0)
    return 0;
  return (char*)p2v(ELO_ADDR(PTE_ELO(*pte, ELX(uva))));
}

// Copy len bytes from p to user address va in page table pgdir.
// Most useful when pgdir is not the current page table.
// uva2ka ensures this only works for PTE_U pages.
int
copyout(pde_t *pgdir, ulong va, void *p, ulong len)
{
  char *buf, *pa0;
  ulong n, va0;

  buf = (char*)p;
  while(len > 0){
    va0 = (ulong)PGROUNDDOWN(va);
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

