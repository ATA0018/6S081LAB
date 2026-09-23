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

int refcnt[NPAGE]; // fault page reference count
struct spinlock refcnt_lock; // fault page reference count lock

struct run {
  struct run *next;
};

struct {
  struct spinlock lock;
  struct run *freelist;
} kmem;

void kinit() {
    initlock(&refcnt_lock, "refcnt"); // fault page reference count lock
    for (int i = 0; i < NPAGE; i++) refcnt[i] = 0; // initialize fault page reference count array.
    initlock(&kmem.lock, "kmem");
    freerange(end, (void*)PHYSTOP);
}

void
freerange(void *pa_start, void *pa_end)
{
  char *p;
  p = (char*)PGROUNDUP((uint64)pa_start);
  for(; p + PGSIZE <= (char*)pa_end; p += PGSIZE)
    freepage(p);          // 初始化：直接挂入，refcnt 保持 0
}

// Free the page of physical memory pointed at by pa,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
// 解除一次引用，归0才会真正释放
void 
kfree(void *pa) {
  if (((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
      panic("kfree");

  acquire(&refcnt_lock);
  int idx = PA2IDX(pa);
  if (refcnt[idx] < 1) panic("kfree refcnt");

  refcnt[idx]--;
  int last = (refcnt[idx] == 0);
  release(&refcnt_lock);

  if (!last)
    return;                   // 还有引用，不回收
  freepage(pa);               // 归零才真正挂回 freelist
}

// 供 uvmcopy / vmfault 显式加引用
void kref(void *pa) {
    acquire(&refcnt_lock);
    refcnt[PA2IDX(pa)]++;
    release(&refcnt_lock);
}

int
krefcount(void *pa)
{
  int c;
  acquire(&refcnt_lock);
  c = refcnt[PA2IDX(pa)];
  release(&refcnt_lock);
  return c;
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.

void *kalloc(void) {
    struct run *r;
    acquire(&kmem.lock);
    r = kmem.freelist;
    if (r) kmem.freelist = r->next;
    release(&kmem.lock);

    if (r) {
        memset((char*)r, 5, PGSIZE); // fill with junk
        // 初始化引用计数为 1
        acquire(&refcnt_lock);
        refcnt[PA2IDX(r)] = 1;
        release(&refcnt_lock);
    }
    return (void*)r;
}

// 真正把页挂回 freelist，不碰引用计数
void
freepage(void *pa)
{
  struct run *r;

  if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
    panic("freepage");
  memset(pa, 1, PGSIZE);

  r = (struct run*)pa;

  acquire(&kmem.lock);
  r->next = kmem.freelist;
  kmem.freelist = r;
  release(&kmem.lock);

}
