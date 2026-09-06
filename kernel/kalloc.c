// Physical memory allocator, for user processes,
// kernel stacks, page-table pages,
// and pipe buffers. Allocates whole 4096-byte pages.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"

void freerange(void *pa_start, void *pa_end);

extern char end[]; // first address after kernel.
                   // defined by kernel.ld.

struct run {
  struct run *next;
};

struct {
  struct spinlock lock;
  struct run *freelist;
} kmem;

#ifdef LAB_PGTBL
#define NSUPER 32

struct {
  struct spinlock lock;
  void *pages[NSUPER];
  int nfree;
} supermem;

static uint64
superbase(void)
{
  return PHYSTOP - (uint64)NSUPER * SUPERPGSIZE;
}

static void
superinit(void)
{
  initlock(&supermem.lock, "super");
  supermem.nfree = 0;
  for(int i = 0; i < NSUPER; i++)
    supermem.pages[supermem.nfree++] = (void*)(superbase() + (uint64)i * SUPERPGSIZE);
}

void *
superalloc(void)
{
  void *p;

  acquire(&supermem.lock);
  if(supermem.nfree == 0)
    p = 0;
  else
    p = supermem.pages[--supermem.nfree];
  release(&supermem.lock);

  if(p)
    memset(p, 5, SUPERPGSIZE);
  return p;
}

void
superfree(void *pa)
{
  if(((uint64)pa % SUPERPGSIZE) != 0 || (uint64)pa < superbase() || (uint64)pa >= PHYSTOP)
    panic("superfree, pa is not aligned or out of range");

  memset(pa, 1, SUPERPGSIZE);

  acquire(&supermem.lock);
  if(supermem.nfree >= NSUPER)
    panic("superfree, no free pages");
  supermem.pages[supermem.nfree++] = pa;
  release(&supermem.lock);
}
#endif

void
kinit()
{
  initlock(&kmem.lock, "kmem");
#ifdef LAB_PGTBL
  superinit();
  freerange(end, (void*)superbase());
#else
  freerange(end, (void*)PHYSTOP);
#endif
}

void
freerange(void *pa_start, void *pa_end)
{
  char *p;
  p = (char*)PGROUNDUP((uint64)pa_start);
  for(; p + PGSIZE <= (char*)pa_end; p += PGSIZE)
    kfree(p);
}

// Free the page of physical memory pointed at by pa,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
void
kfree(void *pa)
{
  struct run *r;

  if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
    panic("kfree");

  // Fill with junk to catch dangling refs.
  memset(pa, 1, PGSIZE);

  r = (struct run*)pa;

  acquire(&kmem.lock);
  r->next = kmem.freelist;
  kmem.freelist = r;
  release(&kmem.lock);
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
void *
kalloc(void)
{
  struct run *r;

  acquire(&kmem.lock);
  r = kmem.freelist;
  if(r)
    kmem.freelist = r->next;
  release(&kmem.lock);

  if(r)
    memset((char*)r, 5, PGSIZE); // fill with junk
  return (void*)r;
}
