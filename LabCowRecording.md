## Page Fault 是什么？
Page fault 是 CPU 在访问内存时，发现虚拟地址到物理地址的映射不满足本次访问要求，于是陷入内核的一种异常。

触发条件有三类：
类型	触发原因	xv6 里 scause
缺页	PTE 无效（PTE_V=0），页还没映射	12 / 13
写保护	页有效但只读（无 PTE_W），却执行了写	15
权限	用户态访问内核页等	13 / 15

关键点：page fault 不是错误，而是一种“按需处理”的机制。 内核可以：

    分配页并建立映射（demand paging）

    复制页并改权限（COW）

    或者判定非法，杀进程（SIGSEGV）

Problem: xv6 中的 fork() 系统调用会将父进程的用户空间内存全部复制到子进程中。如果父进程较大，复制过程可能耗时较长。更糟糕的是，这项工作往往被大量浪费：在子进程中通常紧接着执行 exec()，而 exec() 会丢弃已复制的内存，通常不会使用其中大部分内容。另一方面，如果父进程和子进程都使用了同一份复制的页面，并且其中一方或双方对该页面进行了写操作，那么该复制确实是必要的。

### 提问：在内核fork中，子进程会得到父进程的用户内存，那么子进程会调用exec(); 这里为什么说exec会丢弃，已经复制的内存？


fork 后：  子进程地址空间 == 父进程地址空间（COW 共享）
exec 后：  子进程地址空间 == 新程序的地址空间

父进程那份内存对子进程来说完全没用了，因为子进程要执行的是另一个程序。留着它既占内存又没意义，所以 exec 直接把它释放掉。

Problem: 实现“写时复制”（COW）fork() 的目标是，将物理内存页的分配和复制推迟到实际需要这些副本时才进行。
COW fork() 仅为子进程创建一个页表，用户内存中的页面描述符（PTE）指向父进程的物理页。
COW fork() 将父进程和子进程中的所有用户 PTE 标记为只读。当任一进程尝试写入这些 COW 页面时，CPU 会强制触发页面错误。
内核的页面错误处理程序检测到此情况后，会为出错进程分配一个物理内存页，将原始页面复制到新页中，并修改出错进程的相关 PTE，
使其指向新页，同时将该 PTE 标记为可写。当页面错误处理程序返回后，用户进程便可对它的页面副本进行写操作。

### Q: page fault 和 demand paging有什么区别？

Page fault 是硬件触发的异常事件；Demand paging 是内核利用 page fault 实现的内存管理策略。
项目	Page fault	Demand paging
本质	CPU 硬件异常（事件）	内核内存管理策略（机制）
触发方	CPU 硬件	内核设计选择
作用	通知内核：当前虚拟地址访问失败	利用缺页异常，延迟物理内存分配
关系	demand paging 依赖 page fault 才能工作	page fault 可以用于很多其他用途，不只懒分配

### P: Implement copy-on-write fork
Backgroud: Your task is to implement copy-on-write fork in the xv6 kernel. You are done if your modified kernel executes both the cowtest and 'usertests -q' programs successfully. 

### Q: 为什么父类和子类中都要清 PTE_W ？
这是关键点，容易只改一边：
只清子进程的：父进程仍可写，写了之后子进程看到的是被改过的内容，隔离性破坏。
只清父进程的：子进程仍可写，同理破坏父进程数据。
两边都清：任何一方写都触发 fault，由内核统一处理“复制—改权限—重映射”。

### 实现流程设计：
uvmcopy：共享物理页 + 双向清 PTE_W + 打 PTE_COW。

vmfault：写 COW 页时复制；写只读页时杀进程。

引用计数：kalloc 置 1，共享 +1，解除映射 −1，归零才回收。

copyout：内核态写也走同一套 COW 复制逻辑。

# COW Fork 完整实现方案（xv6-riscv）

下面把前面四块拼图整合成一份可直接落地的实施方案，覆盖宏定义、数据结构、各函数改动、边界处理与测试。

---

## 一、基础宏定义（kernel/riscv.h）

在文件末尾 PTE 标志区加入：

```c
#define PTE_V (1L << 0) // valid
#define PTE_R (1L << 1) // read
#define PTE_W (1L << 2) // write
#define PTE_X (1L << 3) // execute
#define PTE_U (1L << 4) // user
#define PTE_G (1L << 5)
#define PTE_A (1L << 6)
#define PTE_D (1L << 7)
#define PTE_COW (1L << 8) // RSW 位：软件自定义，标记 COW 页

// 常用工具宏
#define PTE2PA(pte) (((pte) >> 10) << 12)
#define PA2PTE(pa) ((((uint64)(pa)) >> 12) << 10)
#define PTE_FLAGS(pte) ((pte) & 0x3FF)   // 低 10 位是标志
```

> 说明：RSW 是 bit 8–9，硬件忽略，正好给软件用。用 bit 8 标记 `PTE_COW`。

---

## 二、引用计数（kernel/kalloc.c）

### 2.1 全局数组与索引

```c
#include "spinlock.h"

extern char end[];          // 由 kernel.ld 定义，第一个空闲物理地址
#define NPAGE (PHYSTOP / PGSIZE)
#define PA2IDX(pa) ((uint64)(pa) / PGSIZE)

int refcnt[NPAGE];
struct spinlock refcnt_lock;
```

### 2.2 初始化（kinit 或 kalloc 第一次调用前）

```c
void kinit() {
    initlock(&refcnt_lock, "refcnt"); // fault page reference count lock
    for (int i = 0; i < NPAGE; i++) refcnt[i] = 0; // initialize fault page reference count array.
    initlock(&kmem.lock, "kmem");
    freerange(end, (void*)PHYSTOP);
}

// 真正把页挂回 freelist，不碰引用计数
void
freerange(void *pa_start, void *pa_end)
{
  char *p;
  p = (char*)PGROUNDUP((uint64)pa_start);
  for(; p + PGSIZE <= (char*)pa_end; p += PGSIZE)
    freepage(p);          // 初始化：直接挂入，refcnt 保持 0
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

```
### 2.3 kalloc：分配即计数 1

```c
void *kalloc(void) {
    struct run *r;
    acquire(&kmem.lock);
    r = kmem.freelist;
    if (r) kmem.freelist = r->next;
    release(&kmem.lock);

    if (r) {
        memset((char*)r, 5, PGSIZE);

        acquire(&refcnt_lock);
        refcnt[PA2IDX(r)] = 1;
        release(&refcnt_lock);
    }
    return (void*)r;
}

### 2.4 kfree：解除一次引用，归零才回收

```c
void kfree(void *pa) {
    if (((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
        panic("kfree");

    acquire(&refcnt_lock);
    int idx = PA2IDX(pa);
    if (refcnt[idx] < 1) panic("kfree refcnt");
    refcnt[idx]--;
    int last = (refcnt[idx] == 0);
    release(&refcnt_lock);

    if (!last) return;   // 还有别人引用，不回收

    memset(pa, 1, PGSIZE);
    struct run *r = (struct run*)pa;
    acquire(&kmem.lock);
    r->next = kmem.freelist;
    kmem.freelist = r;
    release(&kmem.lock);
}

void 
kfree(void *pa) {
  if (((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
      panic("kfree");

  acquire(&refcnt_lock);
  int idx = PA2IDX(pa);
  if (refcnt[idx] < 1) {
    printf("kfree refcnt: pa=%p idx=%d refcnt=%d\n", pa, idx, refcnt[idx]); // 测试的时候，加这行
    panic("kfree refcnt");
  }
  refcnt[idx]--;
  int last = (refcnt[idx] == 0);
  release(&refcnt_lock);

  if (!last)
    return;                   // 还有引用，不回收
  freepage(pa);               // 归零才真正挂回 freelist
}

// 原本的操作
// void kref(void *pa)
// {
//     acquire(&refcnt_lock);
//     refcnt[PA2IDX(pa)]++;
//     release(&refcnt_lock);
// }
/**
 * 引用一次物理地址
 * @param pa 物理地址指针
 */
// 使用原子操作实现kref
void kref(void *pa) {
    refcnt[PA2IDX(pa)]++;  // 假设refcnt是atomic类型
}

// 使用原子操作实现krefcount
/**
 * 获取物理地址对应的引用计数
 * @param pa 物理地址指针
 * @return 返回该物理地址对应的引用计数值
 */
int krefcount(void *pa) {
    return refcnt[PA2IDX(pa)];  // 直接返回原子值
}

```

> 记得在 defs.h 里声明 `void kref(void*);`

---

## 三、fork 时共享（kernel/vm.c: uvmcopy）

```c
// int
// uvmcopy(pagetable_t old, pagetable_t new, uint64 sz)
// {
//   pte_t *pte;
//   uint64 pa, i;
//   uint flags;
//   char *mem;

//   for(i = 0; i < sz; i += PGSIZE){
//     if((pte = walk(old, i, 0)) == 0)
//       continue;   // page table entry hasn't been allocated
//     if((*pte & PTE_V) == 0)
//       continue;   // physical page hasn't been allocated
//     pa = PTE2PA(*pte);
//     flags = PTE_FLAGS(*pte);
//     if((mem = kalloc()) == 0)
//       goto err;
//     memmove(mem, (char*)pa, PGSIZE);
//     if(mappages(new, i, PGSIZE, (uint64)mem, flags) != 0){
//       kfree(mem);
//       goto err;
//     }
//   }
//   return 0;

//   err:
//     uvmunmap(new, 0, i / PGSIZE, 1);
//     return -1;
// }
int uvmcopy(pagetable_t old, pagetable_t new, uint64 sz) {
  pte_t *pte;
  uint64 pa, i;
  uint64 flags;

  for (i = 0; i < sz; i += PGSIZE) {
      if ((pte = walk(old, i, 0)) == 0)
        continue;                       // lazy：页表项可能尚未分配
      if ((*pte & PTE_V) == 0)
        continue;

      pa = PTE2PA(*pte);
      flags = PTE_FLAGS(*pte);

      // 原本可写 -> COW：清 PTE_W、打 PTE_COW、双向只读
      if (flags & PTE_W) {
          flags = (flags & ~PTE_W) | PTE_COW;
          *pte = PA2PTE(pa) | flags;   // 改父进程 PTE
      }
      // 原本只读（如文本段）：保持只读，不加 PTE_COW

      kref((void*)pa);                  // 引用 +1
      if (mappages(new, i, PGSIZE, pa, flags) != 0) {
          kfree((void*)pa);
          goto err;
      }
  }
  sfence_vma();                         // 父进程 PTE 权限已变，刷新 TLB
  return 0;

  err:
      uvmunmap(new, 0, i / PGSIZE, 1);
      return -1;
}
```

要点：

- **父进程 PTE 也要改**（清 `PTE_W` + 打 `PTE_COW`），否则父写会破坏共享。
- 只读页共享但**不标 COW**，这样 vmfault 才能区分“真非法写”。
- 每共享一页 `kref` 一次。

---

## 四、页错误处理（kernel/trap.c: usertrap + vm.c: cowfault）

### 4.1 统一复制函数（vm.c）

```c
// 返回 0 成功，-1 失败（无内存）
int cowfault(pagetable_t pagetable, uint64 va) {
    pte_t *pte = walk(pagetable, va, 0);
    if (pte == 0 || (*pte & PTE_V) == 0) return -1;

    uint64 pa = PTE2PA(*pte);
    unit64 flags = PTE_FLAGS(*pte);
    if ((flags & PTE_COW) == 0) return -1;   // 不是 COW 页

    char *mem = kalloc();
    if (mem == 0) return -1;                 // 无空闲内存 -> 杀进程

    memmove(mem, (char*)pa, PGSIZE);
    unit64 newflags = (flags | PTE_W) & ~PTE_COW;
    *pte = PA2PTE(mem) | newflags;

    kfree((void*)pa);                        // 旧页引用 -1
    return 0;
}

int cowfault(pagetable_t pagetable, uint64 va) {
    pte_t *pte;
    uint64 pa;
    uint64 flags;
    char *mem;

    va = PGROUNDDOWN(va);
    pte = walk(pagetable, va, 0);
    if (pte == 0 || (*pte & PTE_V) == 0)
      return -1;
    if ((*pte & PTE_U) == 0)
      return -1;

    flags = PTE_FLAGS(*pte);
    if ((flags & PTE_COW) == 0)
      return -1;                            // 不是 COW 页

    pa = PTE2PA(*pte);

    // 唯一引用：无需复制，直接恢复可写
    if (krefcount((void*)pa) == 1) {
      *pte = PA2PTE(pa) | ((flags | PTE_W) & ~PTE_COW);
      sfence_vma();
      return 0;
    }

    mem = kalloc();
    if (mem == 0)
      return -1;                            // 无空闲内存 -> 杀进程

    memmove(mem, (char*)pa, PGSIZE);
    *pte = PA2PTE((uint64)mem) | ((flags | PTE_W) & ~PTE_COW);
    kfree((void*)pa);                       // 旧页引用 -1
    sfence_vma();
    return 0;
}
```

### 4.2 usertrap 里识别写错误

```c
void usertrap(void) {
    ...
    else if(r_scause() == 13 || r_scause() == 15) {
    // load/store page fault: lazy 分配或 COW
    uint64 va = r_stval();
    if (va >= MAXVA || va >= p->sz) {
      setkilled(p);
    } else {
      pte_t *pte = walk(p->pagetable, va, 0);
      if (pte && (*pte & PTE_V)) {
        // 已映射：写 COW 页则复制，否则非法
        if (r_scause() == 15 && (*pte & PTE_COW)) {
          if (cowfault(p->pagetable, va) < 0) setkilled(p);
        } else {
          setkilled(p);
        }
      } else {
        // 未映射：尝试 lazy 分配
        if (vmfault(p->pagetable, va, 0) == 0) setkilled(p);
      }
    }
  }
    ...
}
```

要点：

- 写只读文本段（无 `PTE_COW`）→ `cowfault` 返回 -1 → 杀进程。
- 无空闲内存 → `kalloc` 返回 0 → 杀进程（题目明确要求）。
- 除零等已有分支保留。

> 注意 va 越界检查：`va >= p->sz` 直接杀，避免 walk 到无效区。

---

## 五、内核态写用户页（kernel/vm.c: copyout）

```c
// int
// copyout(pagetable_t pagetable, uint64 dstva, char *src, uint64 len)
// {
//   uint64 n, va0, pa0;
//   pte_t *pte;

//   while(len > 0){
//     va0 = PGROUNDDOWN(dstva);
//     if(va0 >= MAXVA)
//       return -1;
  
//     pa0 = walkaddr(pagetable, va0);
//     if(pa0 == 0) {
//       if((pa0 = vmfault(pagetable, va0, 0)) == 0) {
//         return -1;
//       }
//     }

//     pte = walk(pagetable, va0, 0);
//     // forbid copyout over read-only user text pages.
//     if((*pte & PTE_W) == 0)
//       return -1;
      
//     n = PGSIZE - (dstva - va0);
//     if(n > len)
//       n = len;
//     memmove((void *)(pa0 + (dstva - va0)), src, n);

//     len -= n;
//     src += n;
//     dstva = va0 + PGSIZE;
//   }
//   return 0;
// }
int copyout(pagetable_t pagetable, uint64 dstva, char *src, uint64 len) {
    uint64 n, va0, pa0;

    while (len > 0) {
        va0 = PGROUNDDOWN(dstva);
        if (va0 >= MAXVA) return -1;

        pte_t *pte = walk(pagetable, va0, 0);
        if (pte == 0 || (*pte & PTE_V) == 0) return -1;

        // 遇到 COW 页，先复制（与 vmfault 同一套逻辑）
        if ((*pte & PTE_COW) && (*pte & PTE_U)) {
            if (cowfault(pagetable, va0) < 0)
                return -1;
        }

        pa0 = PTE2PA(*pte);
        n = PGSIZE - (dstva - va0);
        if (n > len) n = len;
        memmove((void*)(pa0 + (dstva - va0)), src, n);

        len -= n;
        src += n;
        dstva = va0 + PGSIZE;
    }
    return 0;
}
int copyout(pagetable_t pagetable, uint64 dstva, char *src, uint64 len) {
    uint64 n, va0, pa0;
    pte_t *pte;

    while (len > 0) {
        va0 = PGROUNDDOWN(dstva);
        if (va0 >= MAXVA)
          return -1;

        pte = walk(pagetable, va0, 0);
        if (pte == 0 || (*pte & PTE_V) == 0) {
          // lazy 分配尚未建映射
          if (vmfault(pagetable, va0, 0) == 0)
            return -1;
          pte = walk(pagetable, va0, 0);
          if (pte == 0 || (*pte & PTE_V) == 0)
            return -1;
        }

        if ((*pte & PTE_U) == 0)
          return -1;

        // 遇到 COW 页，先复制（与页错误路径同一套逻辑）
        if (*pte & PTE_COW) {
            if (cowfault(pagetable, va0) < 0)
                return -1;
        }

        // 禁止写只读用户页（如文本段）
        if ((*pte & PTE_W) == 0)
          return -1;

        pa0 = PTE2PA(*pte);
        n = PGSIZE - (dstva - va0);
        if (n > len)
          n = len;
        memmove((void*)(pa0 + (dstva - va0)), src, n);

        len -= n;
        src += n;
        dstva = va0 + PGSIZE;
    }
    return 0;
}
```

要点：

- `copyin` 只读，不需要处理。
- 一定要用 `cowfault` 而不是自己重写，保证与页错误路径一致。
- `cowfault` 失败（无内存）返回 -1，`copyout` 返回 -1，由系统调用层处理（通常返回 -1 给用户）。

---

## 六、解除映射时正确减引用（kernel/vm.c: uvmunmap）

```c
// if((va % PGSIZE) != 0)
//     panic("uvmunmap: not aligned");

//   for(a = va; a < va + npages*PGSIZE; a += PGSIZE){
//     if((pte = walk(pagetable, a, 0)) == 0) // leaf page table entry allocated?
//       continue;   
//     if((*pte & PTE_V) == 0)  // has physical page been allocated?
//       continue;
//     if(do_free){
//       uint64 pa = PTE2PA(*pte);
//       kfree((void*)pa);
//     }
//     *pte = 0;
//   }
void uvmunmap(pagetable_t pagetable, uint64 va, uint64 npages, int do_free) {
    uint64 a;
    pte_t *pte;

    if ((va % PGSIZE) != 0)
        panic("uvmunmap: not aligned");

    for (a = va; a < va + npages * PGSIZE; a += PGSIZE) {
        if ((pte = walk(pagetable, a, 0)) == 0)
            continue;                 // lazy：中间页表可能不存在
        if ((*pte & PTE_V) == 0)
            continue;
        if (PTE_FLAGS(*pte) == PTE_V)
            panic("uvmunmap: not a leaf");
        if (do_free) {
            uint64 pa = PTE2PA(*pte);
            kfree((void*)pa);
        }
        *pte = 0;
    }
}
```

注意：COW 后同一物理页可能被多个 PTE 引用，`uvmunmap` 里 **每个 PTE 都要调一次 `kfree`**，这正是引用计数机制的意义。`exec` 换地址空间时 `uvmfree` 会逐页调用，自然覆盖。

---

## 七、易错点清单

| 位置 | 陷阱 | 正确做法 |
|------|------|---------|
| `uvmcopy` | 只改子进程 PTE，父进程仍可写 | **双向**清 `PTE_W`、打 `PTE_COW` |
| `uvmcopy` | 给只读页也打 `PTE_COW` | 只对原本 `PTE_W` 的页打 |
| `vmfault` | 用“没有 `PTE_W`”判断 COW | 必须用 `PTE_COW` 标志 |
| `vmfault` | 无内存时继续运行 | `kalloc` 失败 → `setkilled` |
| `kfree` | 计数未归零就回收 | 归零才挂回 freelist |
| `kfree` | 对同一页重复减 | 每个引用方各减一次，成对 |
| `copyout` | 直接写 COW 页 | 先调 `cowfault` |
| `uvmunmap` | 对无效 PTE 减引用 | `(*pte & PTE_V) == 0` 时跳过 |
| `walk` 前 | va 越界 | 检查 `va >= p->sz` / `va0 >= MAXVA` |
| `PTE_FLAGS` | 掩码写错 | 用 `0x3FF` 保留全部 10 位标志 |

---

## 八、测试与验证

### 8.1 必跑测试

```bash
make qemu
$ cowtest          # 专门测 COW
$ usertests -q     # 覆盖 cowtest 未涉及的边界
```

题目明确要求 **两者都过**。

### 8.2 cowtest 三个子测试对应点

| 子测试 | 验证内容 |
|--------|---------|
| `simple` | fork 后子写不影响父，父写不影响子 |
| `three` | 多次 fork 后引用计数正确（不会提前释放/double free） |
| `file` | `copyout` 路径（read 写入 COW 页）正确处理 |

### 8.3 usertests -q 重点覆盖

- `sbrk` 边界、`fork` 大量进程、`exec` 后地址空间释放。
- 内存耗尽场景：`kalloc` 失败时进程被杀而非 panic。
- `copyout` 越界、非法地址。

### 8.4 内存泄漏自查

跑完 `usertests -q` 后，`kalloc` 应能继续成功分配；可在 `kfree` 归零分支加计数打印，确认 free 次数与 alloc 次数平衡。

---

## 九、整体数据流回顾

```
fork:
  父 PTE (W) --清W+COW--> 只读 COW
  子 PTE 共享同一 pa，refcnt++

用户写 COW 页:
  store -> scause=15 -> usertrap -> cowfault
    kalloc 新页 -> memmove -> PTE 指向新页+W, 清COW
    kfree 旧页 (refcnt--)
  无内存 -> setkilled

内核 copyout 写 COW 页:
  主动检查 PTE_COW -> cowfault -> 再 memmove

进程退出 / exec:
  uvmunmap 逐 PTE kfree (refcnt--)
  最后一个引用 -> 真正回收
```

---

## 十、一句话总结

把 COW 拆成**共享（uvmcopy 双向写保护 + kref）→ 复制（cowfault 统一逻辑）→ 生命周期（refcnt 控制 kfree）→ 内核写路径（copyout 复用 cowfault）**四条线，每条线各司其职，缺一不可。`PTE_COW` 用 RSW 位区分“可复制的只读”与“真只读”，无内存时杀进程，最后用 `cowtest` 和 `usertests -q` 双重验证。