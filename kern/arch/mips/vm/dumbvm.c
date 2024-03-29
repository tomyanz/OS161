/*
 * Copyright (c) 2000, 2001, 2002, 2003, 2004, 2005, 2008, 2009
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

#include <types.h>
#include <kern/errno.h>
#include <lib.h>
#include <spl.h>
#include <spinlock.h>
#include <proc.h>
#include <current.h>
#include <mips/tlb.h>
#include <addrspace.h>
#include <vm.h>

/*
 * Dumb MIPS-only "VM system" that is intended to only be just barely
 * enough to struggle off the ground.
 */

/* under dumbvm, always have 48k of user stack */
#define DUMBVM_STACKPAGES    12

/*
 * Wrap rma_stealmem in a spinlock.
 */
static struct spinlock stealmem_lock = SPINLOCK_INITIALIZER;

int *coremap_count;
paddr_t *coremap_location;
int num_pages;
paddr_t firstpaddr, lastpaddr;
bool bootstrapped = false;
int start_page;

void
vm_bootstrap(void)
{
  KASSERT(!bootstrapped);
  spinlock_acquire(&stealmem_lock);

  ram_getsize(&firstpaddr, &lastpaddr);
  num_pages = (lastpaddr - firstpaddr) / PAGE_SIZE;

  //kprintf("firstpaddr: 0x%x, lastpaddr: 0x%x\n", firstpaddr, lastpaddr);
  //kprintf("Number of pages is: %d\n", num_pages);

  coremap_count = (int *)PADDR_TO_KVADDR(firstpaddr);
  coremap_location = (paddr_t *) coremap_count + num_pages;

  int coremap_size = num_pages * sizeof(int *) * 2;
  start_page = ROUNDUP(coremap_size, PAGE_SIZE) / PAGE_SIZE;

  //kprintf("coremap_count: %x, coremap_location: 0x%x\n", (vaddr_t) coremap_count, (paddr_t) coremap_location);
  coremap_count[0] = start_page;
  coremap_location[0] = (vaddr_t)coremap_count;

  for (int i = start_page; i < num_pages; i++) {
    coremap_count[i] = 0; // initialize coremap to be available
    coremap_location[i] = firstpaddr + i*PAGE_SIZE;
  }
  //kprintf("last coremap_location address: 0x%x\n", (int)&coremap_location[num_pages-1]);
  //kprintf("page %d location: 0x%x\n", start_page, coremap_location[start_page]);
  //kprintf("page 2 location: 0x%x\n", coremap_location[2]);
  //kprintf("last page location: 0x%x\n", coremap_location[num_pages-1]);

  bootstrapped = true;

  spinlock_release(&stealmem_lock);
}

static
paddr_t
getppages(int npages)
{
  if (!bootstrapped) {
    paddr_t addr;

    spinlock_acquire(&stealmem_lock);

    addr = ram_stealmem(npages);

    spinlock_release(&stealmem_lock);

    return addr;
  }

  spinlock_acquire(&stealmem_lock);

  paddr_t addr;
  int page = 0;

  for (int i = start_page; i < num_pages - npages; i++) {
    if (coremap_count[i] == 0) {
      page = i;
      coremap_count[i] = npages;
      for (int j = i+1; j < i+npages; j++) {
        coremap_count[j] = -1;
      }
      break;
    }
  }
  KASSERT(page != 0);
  addr = coremap_location[page];

  //kprintf("allocating %d pages at page %d, with address 0x%x\n", npages, page, addr);
	spinlock_release(&stealmem_lock);

  return addr;
}

/* Allocate/free some kernel-space virtual pages */
vaddr_t 
alloc_kpages(int npages)
{
  paddr_t pa;
  pa = getppages(npages);
  if (pa==0) {
    return 0;
  }

  return PADDR_TO_KVADDR(pa);
}

void 
free_kpages(vaddr_t addr)
{
  //kprintf("attempting to free page at address 0x%x\n", addr);

  spinlock_acquire(&stealmem_lock);

  int page = 0;
  int npages = 0;

  for (int i = start_page; i < num_pages; i++) {
    if (PADDR_TO_KVADDR(coremap_location[i]) == addr) {
      page = i;
      npages = coremap_count[i];
      break;
    }
  }
  KASSERT(page != 0);

  for (int i = page; i < page + npages; i++) {
    coremap_count[i] = 0;
  }

	spinlock_release(&stealmem_lock);
}

void
vm_tlbshootdown_all(void)
{
	panic("dumbvm tried to do tlb shootdown?!\n");
}

void
vm_tlbshootdown(const struct tlbshootdown *ts)
{
	(void)ts;
	panic("dumbvm tried to do tlb shootdown?!\n");
}

int
vm_fault(int faulttype, vaddr_t faultaddress)
{
	vaddr_t vbase1, vtop1, vbase2, vtop2, stackbase, stacktop;
	paddr_t paddr;
	int i;
	uint32_t ehi, elo;
	struct addrspace *as;
	int spl;
  // if entry is in text segment
  bool text = false;

	faultaddress &= PAGE_FRAME;

	DEBUG(DB_VM, "dumbvm: fault: 0x%x\n", faultaddress);

	switch (faulttype) {
	    case VM_FAULT_READONLY:
		/* We always create pages read-write, so we can't get this */
    return 0;
	    case VM_FAULT_READ:
	    case VM_FAULT_WRITE:
		break;
	    default:
		return EINVAL;
	}

	if (curproc == NULL) {
		/*
		 * No process. This is probably a kernel fault early
		 * in boot. Return EFAULT so as to panic instead of
		 * getting into an infinite faulting loop.
		 */
		return EFAULT;
	}

	as = curproc_getas();
	if (as == NULL) {
		/*
		 * No address space set up. This is probably also a
		 * kernel fault early in boot.
		 */
		return EFAULT;
	}

	/* Assert that the address space has been set up properly. */
	KASSERT(as->as_vbase1 != 0);
	KASSERT(as->page_table1[0] != 0);
	KASSERT(as->as_npages1 != 0);
	KASSERT(as->as_vbase2 != 0);
	KASSERT(as->page_table2[0] != 0);
	KASSERT(as->as_npages2 != 0);
	KASSERT(as->page_table3[0] != 0);
	KASSERT((as->as_vbase1 & PAGE_FRAME) == as->as_vbase1);
	//KASSERT((as->as_pbase1 & PAGE_FRAME) == as->as_pbase1);
	KASSERT((as->as_vbase2 & PAGE_FRAME) == as->as_vbase2);
	//KASSERT((as->as_pbase2 & PAGE_FRAME) == as->as_pbase2);
	//KASSERT((as->as_stackpbase & PAGE_FRAME) == as->as_stackpbase);

	vbase1 = as->as_vbase1;
	vtop1 = vbase1 + as->as_npages1 * PAGE_SIZE;
	vbase2 = as->as_vbase2;
	vtop2 = vbase2 + as->as_npages2 * PAGE_SIZE;
	stackbase = USERSTACK - DUMBVM_STACKPAGES * PAGE_SIZE;
	stacktop = USERSTACK;

	if (faultaddress >= vbase1 && faultaddress < vtop1) {
    int page = (faultaddress - vbase1) / PAGE_SIZE;
    int offset = (faultaddress - vbase1) % PAGE_SIZE;
		paddr = as->page_table1[page] + offset;
    text = true;
	}
	else if (faultaddress >= vbase2 && faultaddress < vtop2) {
    int page = (faultaddress - vbase2) / PAGE_SIZE;
    int offset = (faultaddress - vbase2) % PAGE_SIZE;
		paddr = as->page_table2[page] + offset;
	}
	else if (faultaddress >= stackbase && faultaddress < stacktop) {
    int page = (faultaddress - stackbase) / PAGE_SIZE;
    int offset = (faultaddress - stackbase) % PAGE_SIZE;
		paddr = as->page_table3[page] + offset;
	}
	else {
		return EFAULT;
	}

	/* make sure it's page-aligned */
	KASSERT((paddr & PAGE_FRAME) == paddr);

	/* Disable interrupts on this CPU while frobbing the TLB. */
	spl = splhigh();

	for (i=0; i<NUM_TLB; i++) {
		tlb_read(&ehi, &elo, i);
		if (elo & TLBLO_VALID) {
			continue;
		}
		ehi = faultaddress;
		elo = paddr | TLBLO_DIRTY | TLBLO_VALID;
    if (text && as->load_elf_completed) elo &= ~TLBLO_DIRTY;
		DEBUG(DB_VM, "dumbvm: 0x%x -> 0x%x\n", faultaddress, paddr);
		tlb_write(ehi, elo, i);
		splx(spl);
		return 0;
	}

  ehi = faultaddress;
  elo = paddr | TLBLO_DIRTY | TLBLO_VALID;
  if (text && as->load_elf_completed) elo &= ~TLBLO_DIRTY;
  DEBUG(DB_VM, "dumbvm: 0x%x -> 0x%x\n", faultaddress, paddr);
  tlb_random(ehi, elo);
  splx(spl);
  return 0;

	kprintf("dumbvm: Ran out of TLB entries - cannot handle page fault\n");
	splx(spl);
	return EFAULT;
}

struct addrspace *
as_create(void)
{
	struct addrspace *as = kmalloc(sizeof(struct addrspace));
	if (as==NULL) {
		return NULL;
	}

	as->as_vbase1 = 0;
	as->as_npages1 = 0;
	as->as_vbase2 = 0;
	as->as_npages2 = 0;
  as->load_elf_completed = false;
  as->page_table1 = NULL;
  as->page_table2 = NULL;
  as->page_table3 = NULL;

	return as;
}

void
as_destroy(struct addrspace *as)
{
  for (int i = 0; i < as->as_npages1; i++) {
    free_kpages(PADDR_TO_KVADDR(as->page_table1[i]));
  }
  for (int i = 0; i < as->as_npages2; i++) {
    free_kpages(PADDR_TO_KVADDR(as->page_table2[i]));
  }
  for (int i = 0; i < DUMBVM_STACKPAGES; i++) {
    free_kpages(PADDR_TO_KVADDR(as->page_table3[i]));
  }
  kfree(as->page_table1);
  kfree(as->page_table2);
  kfree(as->page_table3);
	kfree(as);
}

void
as_activate(void)
{
	int i, spl;
	struct addrspace *as;

	as = curproc_getas();
#ifdef UW
        /* Kernel threads don't have an address spaces to activate */
#endif
	if (as == NULL) {
		return;
	}

	/* Disable interrupts on this CPU while frobbing the TLB. */
	spl = splhigh();

	for (i=0; i<NUM_TLB; i++) {
		tlb_write(TLBHI_INVALID(i), TLBLO_INVALID(), i);
	}

	splx(spl);
}

void
as_deactivate(void)
{
	/* nothing */
}

int
as_define_region(struct addrspace *as, vaddr_t vaddr, size_t sz,
		 int readable, int writeable, int executable)
{
	size_t npages; 

	/* Align the region. First, the base... */
	sz += vaddr & ~(vaddr_t)PAGE_FRAME;
	vaddr &= PAGE_FRAME;

	/* ...and now the length. */
	sz = (sz + PAGE_SIZE - 1) & PAGE_FRAME;

	npages = sz / PAGE_SIZE;

	/* We don't use these - all pages are read-write */
	(void)readable;
	(void)writeable;
	(void)executable;

	if (as->as_vbase1 == 0) {
		as->as_vbase1 = vaddr;
		as->as_npages1 = npages;
    as->page_table1 = kmalloc(npages*sizeof(int));
    for (size_t i = 0; i < npages; i++) {
      as->page_table1[i] = 0;
    }
		return 0;
	}

	if (as->as_vbase2 == 0) {
		as->as_vbase2 = vaddr;
		as->as_npages2 = npages;
    as->page_table2 = kmalloc(npages*sizeof(int));
    for (size_t i = 0; i < npages; i++) {
      as->page_table2[i] = 0;
    }
		return 0;
	}

	/*
	 * Support for more than two regions is not available.
	 */
	kprintf("dumbvm: Warning: too many regions\n");
	return EUNIMP;
}

static
void
as_zero_region(paddr_t paddr, unsigned npages)
{
	bzero((void *)PADDR_TO_KVADDR(paddr), npages * PAGE_SIZE);
}

int
as_prepare_load(struct addrspace *as)
{
  /*
	KASSERT(as->page_table1[0] == 0);
	KASSERT(as->page_table2[0] == 0);
	KASSERT(as->page_table3[0] == 0);
  */

  for (int i = 0; i < as->as_npages1; i++) {
    as->page_table1[i] = getppages(1);
    if (as->page_table1[i] == 0) {
      return ENOMEM;
    }
    as_zero_region(as->page_table1[i], 1);
  }

  for (int i = 0; i < as->as_npages2; i++) {
    as->page_table2[i] = getppages(1);
    if (as->page_table2[i] == 0) {
      return ENOMEM;
    }
    as_zero_region(as->page_table2[i], 1);
  }

  as->page_table3 = kmalloc(DUMBVM_STACKPAGES*sizeof(int));
  for (int i = 0; i < DUMBVM_STACKPAGES; i++) {
    as->page_table3[i] = getppages(1);
    if (as->page_table3[i] == 0) {
      return ENOMEM;
    }
    as_zero_region(as->page_table3[i], 1);
  }

	return 0;
}

int
as_complete_load(struct addrspace *as)
{
	(void)as;
	return 0;
}

int
as_define_stack(struct addrspace *as, vaddr_t *stackptr)
{
	KASSERT(as->page_table3[0] != 0);

	*stackptr = USERSTACK;
	return 0;
}

int
as_copy(struct addrspace *old, struct addrspace **ret)
{
	struct addrspace *new;

	new = as_create();
	if (new==NULL) {
		return ENOMEM;
	}

	new->as_vbase1 = old->as_vbase1;
	new->as_npages1 = old->as_npages1;
  new->page_table1 = kmalloc(new->as_npages1*sizeof(int));
  for (int i = 0; i < new->as_npages1; i++) {
    new->page_table1[i] = 0;
  }
	new->as_vbase2 = old->as_vbase2;
	new->as_npages2 = old->as_npages2;
  new->page_table2 = kmalloc(new->as_npages2*sizeof(int));
  for (int i = 0; i < new->as_npages2; i++) {
    new->page_table2[i] = 0;
  }

	/* (Mis)use as_prepare_load to allocate some physical memory. */
	if (as_prepare_load(new)) {
		as_destroy(new);
		return ENOMEM;
	}

  /*
	KASSERT(new->as_pbase1 != 0);
	KASSERT(new->as_pbase2 != 0);
	KASSERT(new->as_stackpbase != 0);
	*/

  for (int i = 0; i < new->as_npages1; i++) {
    memmove((void *)PADDR_TO_KVADDR(new->page_table1[i]),
      (const void *)PADDR_TO_KVADDR(old->page_table1[i]),
      PAGE_SIZE);
  }
  for (int i = 0; i < new->as_npages2; i++) {
    memmove((void *)PADDR_TO_KVADDR(new->page_table2[i]),
      (const void *)PADDR_TO_KVADDR(old->page_table2[i]),
      PAGE_SIZE);
  }
  for (int i = 0; i < DUMBVM_STACKPAGES; i++) {
    memmove((void *)PADDR_TO_KVADDR(new->page_table3[i]),
      (const void *)PADDR_TO_KVADDR(old->page_table3[i]),
      PAGE_SIZE);
  }

	*ret = new;
	return 0;
}
