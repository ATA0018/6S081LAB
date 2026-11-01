# xv6 MMAP 实验记录

2026年10月31日

## 1. 实验目标

实现两个系统调用：

```c
void *mmap(void *addr, uint64 length, int prot, int flags, int fd, uint64 offset);
int munmap(void *addr, uint64 length);
```

把文件内容映射到进程地址空间，并满足：

- **懒加载**：`mmap` 只登记映射关系，不立刻读文件；第一次访问对应地址时才分配物理页并读入文件内容；
- **权限控制**：`PROT_READ / PROT_WRITE / PROT_EXEC` 决定 PTE 权限，非法写入要触发致命缺页；
- **共享/私有**：`MAP_SHARED` 的修改在 `munmap`/退出时写回文件；`MAP_PRIVATE` 的修改只影响本进程；
- **部分解除映射**：`munmap` 可以只解除一段，支持整段、前缀、后缀，甚至中间一段；
- **fork 继承**：子进程拥有和父进程相同的 VMA；缺页时子进程自己分配新物理页；
- **exit 清理**：进程退出时像调用 `munmap` 一样释放所有映射。

整体思路：**用 VMA 记录“虚拟地址区间 → 文件 + 偏移 + 权限”的映射关系，真正的物理页在缺页时按需创建。**

## 2. 关键文件

| 文件 | 作用 |
| --- | --- |
| `kernel/proc.h` | `struct vma`、`struct proc` 中的 VMA 数组和 `mmap_top` |
| `kernel/memlayout.h` | `MMAPBASE`，mmap 区域起始地址 |
| `kernel/sysfile.c` | `mmap` / `munmap`、缺页处理、写回、退出清理 |
| `kernel/trap.c` | 用户态缺页入口，分派给 heap 或 mmap |
| `kernel/proc.c` | `fork` 复制 VMA、`exit` 清理 VMA |
| `kernel/defs.h` | `mmapfault()`、`vmaexit()` 等跨文件声明 |
| `kernel/syscall.h/.c`、`user/usys.pl`、`user/user.h` | 系统调用接线 |
| `Makefile` | 把 `_mmaptest` 加入 `UPROGS` |

## 3. 数据结构

### 3.1 VMA

```c
#define NVMA 16

struct vma {
  int used;          // 该表项是否正在使用
  uint64 addr;       // 映射起始虚拟地址（页对齐）
  uint64 len;        // 映射长度（页对齐）
  uint64 offset;     // 在文件中的起始偏移
  int prot;          // PROT_READ / PROT_WRITE / PROT_EXEC
  int flags;         // MAP_SHARED / MAP_PRIVATE
  struct file *file; // 被映射的文件，持有一个引用
};
```

xv6 内核没有通用的小对象分配器，所以直接在每个进程里放一个固定大小数组：

```c
struct proc {
  ...
  struct vma vmas[NVMA];
  uint64 mmap_top;   // 下一次 mmap 的起始地址
};
```

### 3.2 地址空间布局

```text
低地址
  ┌──────────────┐
  │ text / data  │
  │ heap (sbrk)  │  p->sz 以下由 vmfault 懒分配
  ├──────────────┤
  │              │
  │ mmap 区域     │  MMAPBASE = 0x40000000，向上增长
  │              │
  ├──────────────┤
  │   ...        │
  │ TRAPFRAME    │
  │ TRAMPOLINE   │
  └──────────────┘
高地址
```

选择把 mmap 区域放在 `0x40000000`（1GB）而不是紧挨着 `p->sz`：

- `p->sz` 只有约 128MB 物理内存可增长，正常不会碰到 1GB；
- mmap 区域与 heap 分离，`sbrk` 不会和 mmap 冲突；
- 缺页时可以用 `va >= p->sz` 快速区分“普通 lazy heap”和“mmap 区域”。

`mmap_top` 在每次 `mmap` 成功后按页向上推进。本实验没有实现地址复用，解除映射后的虚拟地址不会回收，但 16 个 VMA 和测试规模下完全够用。

## 4. 系统调用接线

```c
// kernel/syscall.h
#define SYS_mmap   22
#define SYS_munmap 23

// kernel/syscall.c
extern uint64 sys_mmap(void);
extern uint64 sys_munmap(void);

[SYS_mmap]   sys_mmap,
[SYS_munmap] sys_munmap,
```

```perl
# user/usys.pl
entry("mmap");
entry("munmap");
```

```c
// user/user.h
void *mmap(void*, uint64, int, int, int, uint64);
int munmap(void*, uint64);
```

Makefile 的 `UPROGS` 中加入 `$U/_mmaptest`。

## 5. mmap 的实现

### 5.1 参数与校验

```c
uint64
sys_mmap(void)
{
  uint64 addr, length, offset, va;
  int prot, flags, fd, i;
  struct file *f;
  struct proc *p = myproc();
  struct vma *v;

  argaddr(0, &addr);
  argaddr(1, &length);
  argint(2, &prot);
  argint(3, &flags);
  argint(4, &fd);
  argaddr(5, &offset);

  (void)addr;  // 忽略地址提示，由内核选择

  if(length == 0 || (offset % PGSIZE) != 0)
    return -1;
  if(fd < 0 || fd >= NOFILE || (f = p->ofile[fd]) == 0)
    return -1;
  if(f->type != FD_INODE)
    return -1;
  if((flags & (MAP_SHARED | MAP_PRIVATE)) == 0 ||
     ((flags & MAP_SHARED) && (flags & MAP_PRIVATE)))
    return -1;
  if((flags & MAP_SHARED) && (prot & PROT_WRITE) && !f->writable)
    return -1;
  ...
}
```

要点：

- `offset` 必须页对齐（`mmaptest` 只用 0）；
- 只能映射普通 inode 文件，不能映射 pipe；
- `MAP_SHARED | MAP_PRIVATE` 必须二选一；
- **共享可写映射要求文件本身可写**：以 `O_RDONLY` 打开的文件不能做 `MAP_SHARED + PROT_WRITE`；
- `MAP_PRIVATE + PROT_WRITE` 允许映射只读文件，因为修改只落在私有物理页上，不会写回。

### 5.2 分配地址与 VMA

```c
  length = PGROUNDUP(length);
  if(p->mmap_top + length < p->mmap_top ||
     p->mmap_top + length >= TRAPFRAME)
    return -1;

  for(i = 0; i < NVMA; i++){
    if(!p->vmas[i].used)
      break;
  }
  if(i == NVMA)
    return -1;
  v = &p->vmas[i];

  va = p->mmap_top;
  p->mmap_top = va + length;

  v->used = 1;
  v->addr = va;
  v->len = length;
  v->offset = offset;
  v->prot = prot;
  v->flags = flags;
  v->file = filedup(f);

  return va;
```

`filedup(f)` 很关键：`mmap` 之后用户可能立刻 `close(fd)`，但 VMA 仍然要能读文件、写回文件，所以 VMA 必须自己持有一个 `struct file` 引用。

此时**不分配物理页、不读文件**，只登记 VMA，这就是“懒”。

## 6. 缺页处理

### 6.1 usertrap 分派

```c
} else if(r_scause() == 15 || r_scause() == 13){
  // 13 = load page fault, 15 = store page fault
  uint64 va = r_stval();
  int read = (r_scause() == 13);
  uint64 pa;

  if(va >= p->sz)
    pa = mmapfault(va, read);    // mmap 区域
  else
    pa = vmfault(p->pagetable, va, read);  // 普通 lazy heap

  if(pa == 0){
    printf("usertrap(): unexpected scause ...");
    setkilled(p);
  }
}
```

为什么用 `va >= p->sz` 分派：

- `vmfault()` 原本只处理 `p->sz` 以下的懒分配堆；
- mmap 区域在 `MMAPBASE` 以上，必然大于 `p->sz`；
- 这样不需要修改 `vmfault()` 的原有逻辑，也不会影响 usertests。

### 6.2 mmapfault

```c
uint64
mmapfault(uint64 va, int read)
{
  struct proc *p = myproc();
  struct vma *v;
  struct inode *ip;
  uint64 a, mem;
  int perm;

  a = PGROUNDDOWN(va);
  if((v = vmafind(p, a)) == 0)
    return 0;
  if(read == 0 && (v->prot & PROT_WRITE) == 0)
    return 0;              // 写只读映射：致命错误
  if(ismapped(p->pagetable, a))
    return 0;              // 已映射还缺页：权限问题，交给上层杀掉
  if((mem = (uint64)kalloc()) == 0)
    return 0;
  memset((void*)mem, 0, PGSIZE);

  ip = v->file->ip;
  ilock(ip);
  readi(ip, 0, mem, v->offset + (a - v->addr), PGSIZE);
  iunlock(ip);

  perm = PTE_U;
  if(v->prot & PROT_READ)
    perm |= PTE_R;
  if(v->prot & PROT_WRITE)
    perm |= PTE_W | PTE_R;
  if(v->prot & PROT_EXEC)
    perm |= PTE_X;

  if(mappages(p->pagetable, a, PGSIZE, mem, perm) != 0){
    kfree((void*)mem);
    return 0;
  }
  return mem;
}
```

细节：

- **按页处理**：先把 `va` 向下取整到页边界；
- **先清零**：如果文件不足一页（例如文件只有 1.5 页），`readi` 只填前半页，剩下必须是 0；
- **`readi` 的偏移**：`v->offset + (a - v->addr)`，即“文件起始偏移 + 页在映射内的偏移”；
- **锁 inode**：`readi` 要求调用者持有 `ip->lock`；
- **PTE 权限**：`PTE_U` 必须有，`PROT_WRITE` 时同时给 `PTE_R | PTE_W`（RISC-V 不允许只有 W 没有 R）；
- `mappages` 失败要 `kfree`，否则泄漏物理页。

### 6.3 懒加载语义

因为 `mmap` 不读文件，所以下面这个测试能通过：

```c
p = mmap(0, PGSIZE*2, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
// 此时还没读文件
write(fd2, "m", 1);   // 修改文件第一个字节
if(*p != 'm')         // 第一次访问才缺页读文件，看到的是 'm'
  ...
```

如果 `mmap` 时就把内容读进内存，这里会读到旧内容，测试失败。

## 7. munmap 与写回

### 7.1 查找 VMA

```c
static struct vma*
vmafind(struct proc *p, uint64 va)
{
  for(int i = 0; i < NVMA; i++){
    struct vma *v = &p->vmas[i];
    if(v->used && va >= v->addr && va < v->addr + v->len)
      return v;
  }
  return 0;
}
```

### 7.2 部分解除映射

`munmap` 可能只解除一个 VMA 的一部分，分四种情况：

```text
原 VMA:        [ A ][ B ][ C ][ D ]
munmap 整段:    ^^^^^^^^^^^^^^^^^^^^    → 删除 VMA，fileclose
munmap 前缀:    ^^^^^^^^                → addr/offset 后移，len 减小
munmap 后缀:                  ^^^^^^^^  → 只减小 len
munmap 中间:        ^^^^^^^^            → 拆成两个 VMA
```

核心代码：

```c
static int
vmaunmap(struct proc *p, uint64 addr, uint64 len)
{
  struct vma *v, *nv;
  uint64 a, end;
  int i, middle;

  if(len == 0 || (addr % PGSIZE) != 0)
    return -1;
  len = PGROUNDUP(len);
  if(addr + len < addr)
    return -1;
  end = addr + len;

  if((v = vmafind(p, addr)) == 0 || end > v->addr + v->len)
    return -1;

  middle = (addr > v->addr && end < v->addr + v->len);
  nv = 0;
  if(middle){
    for(i = 0; i < NVMA; i++){
      if(!p->vmas[i].used){
        nv = &p->vmas[i];
        break;
      }
    }
    if(nv == 0)
      return -1;   // 没有空表项，无法拆分
  }

  for(a = addr; a < end; a += PGSIZE){
    vmawriteback(p, v, a);
    uvmunmap(p->pagetable, a, 1, 1);
  }

  if(addr == v->addr && end == v->addr + v->len){
    fileclose(v->file);
    v->used = 0;
    v->file = 0;
  } else if(addr == v->addr){
    v->offset += len;
    v->addr += len;
    v->len -= len;
  } else if(end == v->addr + v->len){
    v->len -= len;
  } else {
    nv->used = 1;
    nv->addr = end;
    nv->len = v->addr + v->len - end;
    nv->offset = v->offset + (end - v->addr);
    nv->prot = v->prot;
    nv->flags = v->flags;
    nv->file = filedup(v->file);
    v->len = addr - v->addr;
  }

  return 0;
}
```

注意：

- 只有**整个 VMA 都被解除**时才 `fileclose`；
- 前缀解除时 `offset` 也要跟着前移；
- 中间拆分时，后半段新 VMA 要 `filedup`，否则两边都 `fileclose` 会多减一次引用；
- `uvmunmap(..., do_free=1)` 会释放物理页；如果该页本来就没被缺页分配过，`uvmunmap` 会跳过。

### 7.3 MAP_SHARED 写回

```c
static void
vmawriteback(struct proc *p, struct vma *v, uint64 va)
{
  pte_t *pte;
  uint64 pa, off;
  uint n;
  struct inode *ip;

  if((v->flags & MAP_SHARED) == 0 || (v->prot & PROT_WRITE) == 0)
    return;
  if((pte = walk(p->pagetable, va, 0)) == 0 || (*pte & PTE_V) == 0)
    return;

  pa = PTE2PA(*pte);
  ip = v->file->ip;
  off = v->offset + (va - v->addr);

  begin_op();
  ilock(ip);
  // 不要写超过文件当前大小：mmap 页可能超出 EOF，
  // 超出部分只是缺页时补的零。
  if(off < ip->size){
    n = PGSIZE;
    if(off + n > ip->size)
      n = ip->size - off;
    writei(ip, 0, pa, off, n);
  }
  iunlock(ip);
  end_op();
}
```

关键点：

1. **写回条件**：`MAP_SHARED` 且 `PROT_WRITE`。`MAP_PRIVATE` 绝不写回；只读共享映射也不会被修改。
2. **内核地址**：`PTE2PA(*pte)` 得到物理地址，xv6 内核直接映射物理内存，所以可以作为 `writei(..., user_src=0, ...)` 的源地址。
3. **事务**：`writei` 会修改数据块、inode，必须放在 `begin_op()/end_op()` 中。
4. **必须按文件大小截断写回长度**。

### 7.4 为什么必须限制写回长度（踩坑记录）

最初的实现直接写回整页：

```c
writei(ip, 0, pa, v->offset + (va - v->addr), PGSIZE);
```

`mmaptest` 在 `test mmap dirty` 的 `dirty read #2` 失败。

原因：

- 测试文件只有 **1.5 页**（6144 字节）；
- 映射了 3 页；
- 第一页写 `'B'`，第二页写 `'C'`；
- 第二页在文件中只有前 2048 字节，后 2048 字节是缺页时补的 0；
- 整页写回会把文件从 6144 字节扩展到 8192 字节；
- 测试随后从偏移 4096 读 4096 字节，期望只读到 2048 字节（`PGSIZE/2`），结果读到了 4096 字节，于是 `dirty read #2` 失败。

修正：写回长度取 `min(PGSIZE, ip->size - off)`，并且 `off >= ip->size` 时直接不写。这样映射超出 EOF 的零页不会把文件扩大。

## 8. fork 与 exit

### 8.1 fork 复制 VMA

```c
  np->sz = p->sz;

  // 复制 mmap 区域；子进程独立持有一个文件引用
  for(i = 0; i < NVMA; i++){
    if(p->vmas[i].used){
      np->vmas[i] = p->vmas[i];
      np->vmas[i].file = filedup(p->vmas[i].file);
    }
  }
  np->mmap_top = p->mmap_top;
```

注意：

- `uvmcopy()` 只复制 `p->sz` 以下的页，**不会复制 mmap 区域已经缺页加载的物理页**；
- 子进程第一次访问映射地址时，会走 `mmapfault` 自己分配新页、重新读文件；
- 这符合实验提示：“子进程分配新物理页即可，不要求和父进程共享”；
- VMA 元数据必须复制，否则子进程不知道自己有映射；
- 每个 VMA 的文件引用要 `filedup`，父子各一份。

### 8.2 exit 清理

```c
void
vmaexit(struct proc *p)
{
  for(int i = 0; i < NVMA; i++){
    if(p->vmas[i].used){
      if(vmaunmap(p, p->vmas[i].addr, p->vmas[i].len) < 0){
        // 理论上不会失败；兜底关闭文件，避免泄漏
        fileclose(p->vmas[i].file);
        p->vmas[i].used = 0;
        p->vmas[i].file = 0;
      }
    }
  }
}
```

在 `kexit()` 中、关闭普通 fd 之前调用：

```c
  if(p == initproc)
    panic("init exiting");

  vmaexit(p);   // 像 munmap 一样解除所有映射、写回共享页

  // Close all open files.
  ...
```

`vmaunmap` 内部会对 `MAP_SHARED` 页写回，然后 `uvmunmap` 释放物理页，最后 `fileclose` 释放 VMA 的文件引用。

### 8.3 allocproc / freeproc

```c
  p->pid = allocpid();
  p->state = USED;

  memset(p->vmas, 0, sizeof(p->vmas));
  p->mmap_top = MMAPBASE;
```

`freeproc` 里也清零 VMA 并重置 `mmap_top`，保证进程表项复用时没有残留状态。

## 9. 测试结果

```sh
python3 grade-lab-mmap
```

结果：

```text
== Test running mmaptest ==
  mmaptest: mmap basic: OK
  mmaptest: mmap private: OK
  mmaptest: mmap read-only: OK
  mmaptest: mmap read/write: OK
  mmaptest: mmap dirty: OK
  mmaptest: not-mapped unmap: OK
  mmaptest: lazy access: OK
  mmaptest: two files: OK
  mmaptest: fork_test: OK
  mmaptest: munmap_noaccess: OK
  mmaptest: read_only_write: OK

== Test usertests == usertests: OK (67.3s)
== Test time == time: FAIL (Cannot read time.txt)

Score: 169/170
```

`time.txt` 只需要写入花在实验上的小时数：

```sh
echo 10 > time.txt
```

之后即可拿到 170/170。

## 10. 常见坑

1. **忘记 `filedup`**
   `mmap` 后用户立刻 `close(fd)`，如果 VMA 不持有引用，文件结构会被释放，缺页或写回时崩溃。

2. **mmap 时就把文件读进内存**
   违反懒加载，`test lazy access` 会失败。

3. **越界写回**
   把 EOF 之后的零页写回文件，文件被错误扩大，`dirty read #2` 失败。

4. **MAP_PRIVATE 也写回**
   `test mmap private` 会检查文件内容没有被修改。

5. **`MAP_SHARED + PROT_WRITE` 不检查文件可写**
   `test mmap read-only` 要求这种 `mmap` 直接失败。

6. **写只读映射不杀进程**
   缺页处理里要检查 `read == 0 && !(prot & PROT_WRITE)`，否则会错误地映射可写页，`read_only_write` 测试失败。

7. **`readi` 不锁 inode**
   xv6 的 `readi` 要求调用者持有 `ip->lock`，否则可能读到不一致状态。

8. **`munmap` 前缀/中间拆分时 `offset` 没调整**
   后续缺页会读错文件位置。

9. **拆分 VMA 时忘记 `filedup`**
   两个 VMA 共用一个 `struct file` 引用，先关闭的一方会让另一方悬空。

10. **exit 不清理 VMA**
    物理页和文件引用泄漏；`fork_test`、`more_test` 以及长时间 usertests 可能出问题。

11. **缺页分派直接改 `vmfault`**
    `vmfault` 只认识 `p->sz` 以下的地址；把 mmap 混进去容易破坏原有 lazy heap 逻辑。用 `va >= p->sz` 在 `usertrap` 层分派更清晰。

## 11. 与 Linux mmap 的对照

xv6 的实现是教学版的最小模型，Linux 的 `mmap` 机制复杂得多：

| 维度 | xv6 | Linux |
| --- | --- | --- |
| VMA 管理 | 固定 16 项数组，线性查找 | `vm_area_struct` + 红黑树/ maple tree，支持大量 VMA |
| 地址分配 | `mmap_top` 单调向上 | 复杂的空闲区域查找，可复用地址 |
| 文件缓存 | 缺页时直接 `readi` 读磁盘 | 统一 page cache，多进程共享同一文件页 |
| 写回 | `munmap`/exit 时全页写回 | 依赖 dirty bit、writeback 线程、`msync` |
| 脏页判断 | 未使用 PTE dirty 位 | 使用 PTE dirty 位和 page dirty 标志 |
| COW | 未实现，fork 后子进程重新读文件 | 父子共享只读页，写时复制 |
| 匿名映射 | 未实现 `MAP_ANONYMOUS` | 支持匿名页、交换、THP |
| 权限变更 | 无 `mprotect` | 支持 `mprotect`、`pkey` 等 |
| 缺页类型 | 只处理 load/store fault | 处理 major/minor fault、COW、swap、文件回写等 |

xv6 这套实现虽然简单，但已经包含 mmap 的核心思想：

1. **VMA 描述映射关系**；
2. **缺页时按需建立“虚拟页 → 物理页/文件页”的映射**；
3. **解除映射时释放页并处理脏数据写回**；
4. **fork/exit 要维护映射的生命周期**。

## 12. 可以继续改进的点

- 用 PTE 的 dirty 位（RISC-V `PTE_D`）只写回真正修改过的页；
- 实现 `mprotect`；
- 支持 `MAP_ANONYMOUS`；
- 支持 `mmap` 的地址提示 `addr`，并复用已解除映射的地址区间；
- `fork` 时让父子共享只读物理页，写时复制；
- `exec` 时清理旧 VMA；
- 用 page cache 让多个进程共享同一个文件的物理页，而不是各自 `readi` 一份。


Q: 为什么sys_mmap and sys_munmap放在sysfile.c?
A: 放 sysfile.c 是因为 mmap 在这个实验里主要是“文件页映射”，而 sysfile.c 已经拥有所需的文件/inode/日志接口；放 sysproc.c 也能跑，但会让进程模块承担文件系统细节。

这是代码组织选择，不是 xv6 的硬性规定。系统调用真正注册的地方是 kernel/syscall.c 里的函数指针表；sys_mmap / sys_munmap 放在哪个 .c 文件都能工作，链接时没有区别。
为什么放在 sysfile.c
按 xv6 现有的职责划分：
文件	负责的系统调用	依赖
sysproc.c	fork/exit/wait/getpid/sbrk/pause/uptime/kill 等	proc.h、进程状态、虚拟内存
sysfile.c	open/read/write/close/link/unlink/mkdir/fstat 等	file.h、fs.h、inode、日志
而这次实现的 mmap 本质上是文件映射：
- VMA 里保存的是 struct file *
- mmap 要调用 filedup
- 缺页要调用 readi、ilock
- munmap 写回要调用 writei、begin_op/end_op
- 完整解除映射要 fileclose
这些接口和数据结构 sysfile.c 已经全部 include 好了：
#include "fs.h"
#include "sleeplock.h"
#include "file.h"
#include "fcntl.h"
如果放到 sysproc.c，需要额外引入：
#include "fs.h"
#include "sleeplock.h"
#include "file.h"
#include "fcntl.h"
#include "stat.h"
等于把文件系统内部细节（inode、日志、文件表）引进“进程管理”模块，耦合更重。

