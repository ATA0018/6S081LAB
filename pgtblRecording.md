# 1、PAGE TABLES LAB: SPEED UP SYSTEM CALLS (success)
# 2、PAGE TABLES LAB: Print a page table

### 模拟的内容：Linux **vDSO（virtual dynamic shared object）**。

## 1、核心思想

> 
> 有些系统调用仅仅读取**进程少量、很少变化的信息（pid、ppid）**，不需要内核做复杂工作、不需要 IO、不需要解析参数。
> 不必每次都执行 `ecall` 触发 trap 陷入内核。
> 内核把数据放在**一块用户‑内核共享的只读物理页**。
> 用户态直接**内存读取**拿到结果，跳过陷入内核的开销，加速系统调用。

## 目标

为 `getpid()` 系统调用实现优化：在每个进程的用户页表中映射一个只读页面到 `USYSCALL` 地址，存储 `struct usyscall`（包含 PID），用户空间通过 `ugetpid()` 直接读取，无需陷入内核。

## 涉及文件

| 文件 | 改动 |
|------|------|
| `kernel/proc.h` | `struct proc` 中添加 `void *usyscallpage` 字段 |
| `kernel/memlayout.h` | 定义 `USYSCALL` 地址和 `struct usyscall`（已有，由 `#ifdef LAB_PGTBL` 保护） |
| `kernel/proc.c` | `allocproc` / `freeproc` / `userinit` / `kfork` 四个函数 |
| `kernel/exec.c` | `exec` 函数 |
| `.vscode/c_cpp_properties.json` | 添加 `LAB_PGTBL` 宏定义（解决 IDE IntelliSense 问题） |

## 核心设计思路

### 仿照 trapframe 的生命周期

trapframe 的处理模式是本题的参照模板：

```
allocproc:  kalloc() 分配物理页
proc_pagetable: mappages() 建立映射
freeproc:   kfree() 释放物理页
proc_freepagetable: uvmunmap() 移除映射
```

USYSCALL 遵循完全相同的模式，但有一个关键区别：**`uvmcopy` 会复制父进程的 USYSCALL 映射到子进程**，导致 allocproc 分配的页面泄漏。

### 页面生命周期（5 个阶段）

```
① allocproc    — 分配物理页，写入 PID，不映射
② userinit     — 映射到 USYSCALL（首进程）
③ kfork        — uvmcopy 复制父映射后，替换为子进程自己的页面
④ exec         — 旧页表 unmap，新页表重新 map
⑤ freeproc     — unmap + 释放物理页
```

## 各函数修改详解

### 1. `proc.h` — 添加字段

```c
struct proc {
  ...
  void *usyscallpage;        // User syscall page
};
```

### 2. `allocproc` — 分配但不映射

```c
// Allocate a usyscall page (mapped later in userinit/kfork).
if((p->usyscallpage = kalloc()) == 0){
  freeproc(p);
  release(&p->lock);
  return 0;
}
memset(p->usyscallpage, 0, PGSIZE);
((struct usyscall *)p->usyscallpage)->pid = p->pid;
```

**为什么不在这里映射？** 因为 `kfork` 中 `uvmcopy` 会覆盖映射，导致分配的物理页泄漏（后面详述）。

### 3. `userinit` — 首进程映射

```c
if(mappages(p->pagetable, USYSCALL, PGSIZE,
            (uint64)(p->usyscallpage), PTE_R | PTE_U) < 0){
  panic("userinit: mappages");
}
```

权限位 `PTE_R | PTE_U`：用户态只读。

### 4. `kfork` — 处理 uvmcopy 的映射覆盖

```c
// uvmcopy copied the parent's USYSCALL mapping into the child.
// Replace it with the child's own usyscall page (allocated in allocproc).
uvmunmap(np->pagetable, USYSCALL, 1, 0);  // don't free parent's page
if(mappages(np->pagetable, USYSCALL, PGSIZE,
            (uint64)(np->usyscallpage), PTE_R | PTE_U) < 0){
  freeproc(np);
  release(&np->lock);
  return -1;
}
```

**关键点：**
- `uvmunmap` 第 4 参数 `0`：不释放父进程的物理页（那不是我们的）
- `allocproc` 已为子进程分配了独立的 `usyscallpage`，这里只需重新映射

### 5. `exec` — 页表替换时保持 USYSCALL 映射

```c
// Unmap USYSCALL from old page table (don't free physical page, it's still in use).
uvmunmap(oldpagetable, USYSCALL, 1, 0);
proc_freepagetable(oldpagetable, oldsz);

// Remap USYSCALL in the new page table.
if(mappages(p->pagetable, USYSCALL, PGSIZE,
            (uint64)(p->usyscallpage), PTE_R | PTE_U) < 0){
  goto bad;
}
```

**为什么需要这两步？**
- `exec` 创建全新页表（`proc_pagetable`），不含 USYSCALL 映射
- 旧页表的 USYSCALL 必须先 unmap，否则 `proc_freepagetable → freewalk` 遇到叶子 PTE 会 panic
- 物理页 (`p->usyscallpage`) 不释放，因为进程还在用
- 新页表需要重新 map

### 6. `freeproc` — 清理

```c
if(p->usyscallpage){
  uvmunmap(p->pagetable, USYSCALL, 1, 1);  // unmap + 释放物理页
  p->usyscallpage = 0;
}
```

必须在 `proc_freepagetable` **之前**执行，因为 `freewalk` 要求页表中无叶子 PTE。

## 踩坑记录

### 坑 1：IDE 报未定义标识符

**现象：** VS Code 显示 `USYSCALL` 未定义、`struct usyscall` 不完整类型

**原因：** `memlayout.h` 中 `USYSCALL` 和 `struct usyscall` 在 `#ifdef LAB_PGTBL` 内，IDE 的 IntelliSense 不知道这个宏

**解决：** 创建 `.vscode/c_cpp_properties.json`，添加 `"defines": ["LAB_PGTBL"]`

### 坑 2：`panic: freewalk: leaf`（页表释放时有叶子 PTE）

**现象：** 内核启动后立即 panic

**根因：** `exec` 替换页表时调用 `proc_freepagetable(oldpagetable, ...)`，但该函数只 unmap 了 TRAMPOLINE 和 TRAPFRAME，没有 unmap USYSCALL。`freewalk` 遍历页表发现叶子 PTE 就 panic。

**解决：** 在 `exec.c` 中，`proc_freepagetable` 之前加 `uvmunmap(oldpagetable, USYSCALL, 1, 0)`。

### 坑 3：在 `allocproc` 中映射导致页面泄漏

**现象：** 如果 `allocproc` 中同时分配并映射 USYSCALL，`kfork` 中 `uvmcopy` 会复制父进程的映射覆盖子进程的，导致 allocproc 分配的物理页成为孤儿。

**流程分析：**
```
allocproc:  分配页面 A，映射到 USYSCALL
uvmcopy:    复制父进程映射（页面 C），覆盖页面 A 的映射 → 页面 A 泄漏
kfork:      又 kalloc() 分配页面 D → 更加混乱
```

**解决：** `allocproc` 只分配不映射，`userinit` 和 `kfork` 负责映射。这样 `uvmcopy` 覆盖的是无害的（还没有映射），不会泄漏。

## 编译与测试

```bash
# 编译（必须指定 LAB=pgtbl 才会编译 pgtbltest 用户程序）
make clean && make fs.img LAB=pgtbl

# 运行测试
python3 test-xv6.py pgtbltest
```

**注意：** Makefile 中 `_pgtbltest` 仅在 `LAB=pgtbl` 时加入 UPROGS。`conf/lab.mk` 已设置 `LAB=pgtbl`。

## Question: Which other xv6 system call(s) could be made faster using this shared page? Explain how. 
可以优化 `getppid()`（获取父进程 id）。
原理：内核把父进程 pid 存到这块只读共享页；用户态直接读内存，不需要`ecall`陷入内核，省去 trap 的开销。
该优化仅适合读取**很少发生变化的进程信息**；凡是需要 IO、修改内核状态、解析参数的系统调用（read/write/fork 等），不能用该方案。



# 2、PAGE TABLES LAB: Print a page table

这是一个非常经典的 xv6 操作系统课程实验（通常来自 MIT 6.S081）。这个练习（常被称为 **Print a Page Table** 或 `vmprint`）的核心目的是让你**通过编程，以树形结构可视化 RISC-V 架构下的多级页表**。

### 🎯 练习的核心目标
你的任务是实现一个内核函数 `vmprint()`，它接收一个页表（`pagetable_t`），然后递归地打印出这个页表的完整层级结构，包括每一级页表项（PTE）的虚拟地址、物理地址和权限位。

这不仅是完成一个实验，更重要的是帮助你：

1.  **深刻理解 RISC-V 三级页表结构**：从顶层（Level 2）到叶子节点（Level 0）的硬件查找过程。
2.  **掌握内核页表的遍历技巧**：学会如何通过 PTE 中的标志位判断下一级页表是否存在，并递归访问。
3.  **为后续调试提供工具**：当进程地址空间映射出错时，你可以调用 `vmprint` 来快速定位问题（这就是提示中提到的 "aid future debugging"）。

---

### 📋 输出格式要求
代码需要生成一个格式严格对齐的层级列表。以提示中的例子为例，输出应类似：

```text
page table 0x0000000087f22000
 ..0x0000000000000000: pte 0x0000000021fc7801 pa 0x0000000087f1e000 
 .. ..0x0000000000000000: pte 0x0000000021fc7401 pa 0x0000000087f1d000 
 .. .. ..0x0000000000000000: pte 0x0000000021fc7c5b pa 0x0000000087f1f000 RXU
 .. .. ..0x0000000000001000: pte 0x0000000021fc705b pa 0x0000000087f1c000 RXU
 .. .. ..0x0000000000002000: pte 0x0000000021fc6cd7 pa 0x0000000087f1b000 RWU
 .. .. ..0x0000000000003000: pte 0x0000000021fc6807 pa 0x0000000087f1a000 RW
 .. .. ..0x0000000000004000: pte 0x0000000021fc64d7 pa 0x0000000087f19000 RWU
 ..0x0000003fc0000000: pte 0x0000000021fc8401 pa 0x0000000087f21000 
 .. ..0x0000003fffe00000: pte 0x0000000021fc8001 pa 0x0000000087f20000 
 .. .. ..0x0000003fffffd000: pte 0x0000000021fd4853 pa 0x0000000087f52000 RU
 .. .. ..0x0000003fffffe000: pte 0x0000000021fd00c7 pa 0x0000000087f40000 RW
 .. .. ..0x0000003ffffff000: pte 0x0000000020001c4b pa 0x0000000080007000 RX
```

- **缩进**：每深入一层，缩进 `" .."`（注意前面有两个空格）。
- **每行内容**：`[缩进][索引]: pte [PTE值] pa [物理地址] [权限位]`
- **过滤**：只打印 **有效（`PTE_V` 标志位为1）** 的 PTE。
- **地址**：使用 `%p` 打印完整的 64 位十六进制地址。

---

### 💡 关键实现思路与提示解读
你的代码需要在 `kernel/vm.c` 中实现，并最终通过系统调用 `kpgtbl()` 来触发。

**1. 递归遍历设计**
页表是树形结构，最佳实现方式是**递归函数**。参考 `freewalk` 函数（它也是递归释放页表），你会看到遍历页表的逻辑。

- **终止条件**：当检查到当前 PTE 是**叶子节点**（即指向物理内存页，而非下级页表）时，打印它，不再继续深入。
- **深入条件**：如果 PTE 有效且**不是叶子节点**（即它是一个指向下级页表地址的中间节点），则递归调用自身，并增加缩进层级。

**2. 如何判断叶子节点？**
在 RISC-V 中，叶子节点一定同时满足以下条件：

- 有效位 `PTE_V` 为 1。
- 权限位中，`PTE_R`、`PTE_W`、`PTE_X` 三个标志位**至少有一个为 1**。如果这三个位全为 0，说明这是一个中间节点，指向下一级页表。

### 如何判断叶子节点？（解释）
在 RISC-V 的特权级规范（Privileged Specification）中，对页表项（PTE）的格式有明确约定：

    叶子页表项（最后一级）：必须至少设置 PTE_R（读）、PTE_W（写）、PTE_X（执行）中的任意一个。这三个标志位用于控制对该物理页面的访问权限。

    非叶子页表项（中间节点）：这三个标志位必须全部为 0。MMU 一旦看到这三个位全是 0，就知道这不是一个最终映射，而是一个指向下一级页表的指针。

**3. 如何提取物理地址？**
每个 PTE 的低 10 位是标志位（如有效位、权限位）。你需要用位运算 `PTE >> 10` 来得到真正的物理页帧号（PPN），然后再左移得到物理地址。但更方便的是，直接用 `%p` 打印整个 PTE 值（它已经包含了标志位），同时为了输出 `pa`，你需要提取物理地址。

提示中的宏 `kernel/riscv.h` 提供了 `PA2PTE`、`PTE2PA` 等宏，会很有帮助。

**4. 需要处理的虚拟地址**
在递归时，你需要追踪当前页表项对应的“虚拟地址”。从根开始，每一级的索引（0-511）共同构成一个完整的虚拟地址。例如，根的第 0 项、下一级的第 0 项、再下一级的第 0 项，组合起来就是虚拟地址 `0x0000000000000000`。

---

### 🚀 快速实现步骤
1.  **在 `kernel/vm.c` 中声明递归函数**：`void vmprint_rec(pagetable_t pagetable, int depth, uint64 va_prefix)`。
2.  **实现主函数 `vmprint(pagetable_t pagetable)`**：打印第一行 `"page table %p\n"`，然后调用递归函数。
3.  **实现递归函数**：
    - 遍历当前页表的 512 个条目（`for(int i = 0; i < 512; i++)`）。
    - 如果 `pte & PTE_V` 为真：
        - 按格式打印当前行的缩进、索引、PTE 值、物理地址和权限位。
        - 如果 `(pte & (PTE_R|PTE_W|PTE_X)) == 0`（即不是叶子节点）：
            - 从 `pte` 中提取下一级页表的物理地址：`uint64 child = PTE2PA(pte)`。
            - 计算下一级前缀虚拟地址：`va_prefix | ((uint64)i << (30 - 9*depth))`（注意 depth 从 0 开始计算）。
            - 递归调用 `vmprint_rec((pagetable_t)child, depth+1, va_prefix)`。
4. 当前作业已经在`pgtbletest`中，直接测试即可。

---

### ❗ 注意事项
- **递归深度**：RISC-V 是 3 级页表（SV39），所以递归深度最大为 3（从顶层到叶子），不会栈溢出。
- **地址计算**：确保你计算出的虚拟地址前缀在每一级都是正确的。最好的方式是传入一个累计的虚拟地址前缀，每次递归时加上当前索引的左移量。
- **用户态权限**：PTE 中的 `U` 位表示是否允许用户态访问，打印时别忘了。

这个练习做完后，你会对 `walk` 函数和 `uvmalloc` 如何构建页表有一个非常直观的理解。如果在实现过程中遇到具体问题，可以把代码发出来，我们一起看看。祝你实验顺利！🚀2





# 3、LAB: Use Superpages 
Task: Modify xv6 to use super pages for the kernel.

简答：RISC-V Sv39 页表有三级，Level-1 PTE 若设为叶子节点（带 R/W/X 位），可映射 2MB 连续内存，
称为超级页（super page）。对内核代码段和数据段使用超级页，可减少页表级数、降低 TLB miss 开销。

计划：为内核使用 2MB 超级页

背景
当前 kvmmake() 使用 4KB 页面映射内核所有区域（UART、PLIC、内核代码、内核数据+物理内存、
trampoline、内核栈）。内核代码段和数据段通常很大，使用 2MB 超级页可显著减少 TLB 压力。

核心设计
- 2MB 超级页在 Level-1 PTE 设置叶子节点（R/W/X），跳过 Level-0 页表
- VA 和 PA 必须 2MB 对齐，映射大小必须是 2MB 整数倍
- 对 2MB 不对齐的边界区域（etext 附近）仍用 4KB 页面
- 设备寄存器（UART、VIRTIO）、trampoline、内核栈保持 4KB 页面

2MB 对齐检查：
- KERNBASE (0x80000000) ÷ 2MB = 0x400，2MB 对齐
- PHYSTOP  (0x88000000) ÷ 2MB = 0x440，2MB 对齐
- PLIC     (0x0C000000) ÷ 2MB = 0x60，  2MB 对齐
- PLIC 大小 0x4000000 (64MB) 是 2MB 整数倍
- etext 不一定 2MB 对齐，需要边界处理

当前设计由gork 4.6完成

```c

#ifdef LAB_PGTBL
#define SUPERPGSIZE (2 * (1 << 20)) // bytes per page 1左移20位对应1MB，乘以2就是2MB
#define SUPERPGROUNDUP(sz)  (((sz)+SUPERPGSIZE-1) & ~(SUPERPGSIZE-1)) // 2MB向上对齐
#define SUPERPGROUNDDOWN(sz) (SUPERPGROUNDUP(sz)-SUPERPGSIZE) // 2MB向下对齐
#endif

/*
SUPERPGROUNDUP实现原理：

SUPERPGSIZE - 1：
假设 SUPERPGSIZE 是 2MB (0x200000)，这是一个 2 的幂次方数。
二进制表示为：0010 0000 0000 0000 0000 0000 (22位)
减 1 后：SUPERPGSIZE - 1 变为 0x1FFFFF。
二进制表示为：0001 1111 1111 1111 1111 1111。
特点：低 21 位全为 1，高位全为 0。
~(SUPERPGSIZE - 1)：
对上述结果按位取反。
结果为：1110 0000 0000 0000 0000 0000。
特点：低 21 位全为 0，高位全为 1。这是一个掩码，用于清除低 21 位的所有数据。
(sz) + SUPERPGSIZE - 1：
先将 sz 加上 (对齐单位 - 1)。这一步是为了处理 sz 本身不是对齐倍数的情况。加上这个偏移量后，只要产生进位，就会进位到下一个对齐边界。
& (按位与)：
将加法后的结果与掩码进行按位与操作。这会直接将低 21 位截断（变为0），从而得到向上对齐后的地址。

SUPERPGROUNDDOWN实现原理：

SUPERPGROUNDUP(sz) - SUPERPGSIZE：
假设 sz 是 0x200001，这是一个大于 2MB 的数。
SUPERPGROUNDUP(sz) - SUPERPGSIZE 变为 0x200000 - 0x200000 = 0。
二进制表示为：0000 0000 0000 0000 0000 0000。
特点：低 21 位全为 0，高位全为 0。
*/
```

步骤1:  kernel/defs.h 中添加函数注解

```c
1、superalloc()
2、superfree()
```

步骤2:  kernel/vm.c 中添加函数

part one
```c
// 构建超级页结构
#define NSUPER 32

struct {
  struct spinlock lock; // spinlock to protect the supermem structure
  void *pages[NSUPER]; // free lists of super pages
  int nfree; // number of free pages
} supermem;

// Return the base address of the super pages.
static uint64
superbase(void)
{
  return PHYSTOP - (uint64)NSUPER * SUPERPGSIZE;
}

/*
步骤：
初始化锁：调用 initlock 初始化自旋锁，命名为 "super"。
重置计数器：将 nfree 设为 0。
填充空闲链表：
通过 for 循环遍历 0 到 NSUPER-1。
superbase() + (uint64)i * SUPERPGSIZE：计算出第 i 个大页的物理起始地址。
将计算出的地址存入 pages 数组，并增加 nfree 计数。
结果：初始化完成后，pages 数组中按顺序存放了所有可用大页的地址，nfree 等于 NSUPER，表示所有大页都是空闲的。
*/

// 初始化内存分配器。通常在系统启动时被调用一次。
static void
superinit(void)
{
  initlock(&supermem.lock, "super");
  supermem.nfree = 0;
  for(int i = 0; i < NSUPER; i++)
    supermem.pages[supermem.nfree++] = (void*)(superbase() + (uint64)i * SUPERPGSIZE);
}

// 总结：
// 静态分配：内存空间不是动态寻找的，而是在编译/链接时通过计算 PHYSTOP 固定划分出来的。
// 线性管理：使用一个数组 pages 来维护空闲内存块。这比链表实现起来更简单，且遍历速度极快（缓存友好）。
// 后进先出 (LIFO) / 栈式分配：虽然这段初始化代码是按顺序填入的，但通常配合分配和释放函数（代码中未展示），这种数组结构很容易实现 LIFO（分配时取 pages[--nfree]，释放时存 pages[nfree++]），操作复杂度为 O(1)。


// parter two
// allocate a super page
void *superalloc(void)
{
  void *p;

  // 1. 获取锁，保证线程安全（防止多核并发分配冲突）
  acquire(&supermem.lock);
  
  // 2. 检查是否有空闲页
  if(supermem.nfree == 0)
    p = 0; // 如果没有空闲页，返回空指针
  else
    // 3. 如果有空闲页，从数组中取出一个
    // --supermem.nfree 是先减减，再作为索引使用
    p = supermem.pages[--supermem.nfree];
    
  // 4. 释放锁
  release(&supermem.lock);

  // 5. 如果分配成功（p不为0），将内存内容填充为 5
  // 这通常是为了调试（清除旧数据）或安全原因（清除敏感数据）
  if(p)
    memset(p, 5, SUPERPGSIZE);
    
  return p;
}


void superfree(void *pa)
{
  // 1. 参数校验：检查地址是否对齐、是否在合法的物理内存范围内
  // % SUPERPGSIZE != 0 确保地址是页大小的整数倍（对齐检查）
  if(((uint64)pa % SUPERPGSIZE) != 0 || (uint64)pa < superbase() || (uint64)pa >= PHYSTOP)
    panic("superfree, pa is not aligned or out of range"); // 如果不合法，内核崩溃

  // 2. 将内存内容填充为 1
  // 同样是为了调试或安全，防止释放后通过指针再次访问到旧数据（UAF漏洞利用）
  memset(pa, 1, SUPERPGSIZE);

  // 3. 获取锁
  acquire(&supermem.lock);
  
  // 4. 检查空闲列表是否已满
  if(supermem.nfree >= NSUPER)
    panic("superfree, supermem.nfree is full");
    
  // 5. 将地址压回数组栈中，并增加计数
  supermem.pages[supermem.nfree++] = pa;
  
  // 6. 释放锁
  release(&supermem.lock);
}

#endif

```


步骤3:  kernel/vm.c 中修改函数

```c

// 判定叶子节点
// #if defined(LAB_MMAP) || defined(LAB_PGTBL) || defined(LAB_COW)
#define PTE_LEAF(pte) (((pte) & PTE_R) | ((pte) & PTE_W) | ((pte) & PTE_X)) // 判断是否是叶子节点
// #endif

/*
非叶子节点：

指向下一级页表的物理地址。
关键规则：对于非叶子节点，其 R、W、X 位必须全部为 0。如果这些位不为 0，硬件会认为这是一个叶子节点，而不会将其视为指向下一级页表的指针。
叶子节点：

直接指向一个物理页帧或者一个巨型页。
关键规则：叶子节点必须至少设置 R、W、X 中的一个位，以表明该页的访问权限。
因此，PTE_LEAF 的用途是：
当内核遍历页表时，需要区分当前找到的项是指向下一级目录（需要继续遍历），还是已经到了最后一级映射到了物理内存（遍历结束）。通过 PTE_LEAF(pte)，内核可以快速做出判断：
如果结果为真：说明这是映射了物理内存的页，可以停止查找或操作物理页。
如果结果为假：说明这是一个指向子页表的指针，需要继续跳转到下一级地址进行查找。
*/

// Return the address of the PTE at page-table level `stop`
// (2 = root, 1 = 2MB superpage, 0 = 4KB page).
// If alloc!=0, create any required page-table pages.
static pte_t *
walkat(pagetable_t pagetable, uint64 va, int alloc, int stop)
{
  if(va >= MAXVA)
    panic("walk");

  for(int level = 2; level > stop; level--) {
    pte_t *pte = &pagetable[PX(level, va)];
    if(*pte & PTE_V) {
#ifdef LAB_PGTBL
      // Superpage leaf: do not treat this PTE as a next-level table.
      if(PTE_LEAF(*pte))
        return pte;
#endif
      pagetable = (pagetable_t)PTE2PA(*pte);
    } else {
      if(!alloc || (pagetable = (pde_t*)kalloc()) == 0)
        return 0;
      memset(pagetable, 0, PGSIZE);
      *pte = PA2PTE(pagetable) | PTE_V;
    }
  }
  return &pagetable[PX(stop, va)];
}

pte_t *
walk(pagetable_t pagetable, uint64 va, int alloc)
{
  return walkat(pagetable, va, alloc, 0);
}


// Look up a virtual address, return the physical address,
// or 0 if not mapped.
// Can only be used to look up user pages.
uint64
walkaddr(pagetable_t pagetable, uint64 va)
{
  pte_t *pte;
  uint64 pa;

  if(va >= MAXVA)
    return 0;

  pte = walk(pagetable, va, 0);
  if(pte == 0)
    return 0;
  if((*pte & PTE_V) == 0)
    return 0;
  if((*pte & PTE_U) == 0)
    return 0;
  pa = PTE2PA(*pte);
// #ifdef LAB_PGTBL
  // walk() returns the level-1 leaf for a superpage; add the offset
  // inside the 2MB region so callers see the containing 4KB page.
  if(pte == walkat(pagetable, va, 0, 1))
    pa += PGROUNDDOWN(va) & (SUPERPGSIZE - 1);
// #endif
  return pa;
}

// #if defined(LAB_PGTBL) || defined(SOL_MMAP) || defined(SOL_COW)
static void
vmprintwalk(pagetable_t pagetable, uint64 va, int depth)
{
  for(int i = 0; i < 512; i++){
    pte_t pte = pagetable[i];
    if((pte & PTE_V) == 0)
      continue;
    uint64 nva = va | ((uint64)i << (12 + 9 * (2 - depth)));
    for(int j = 0; j <= depth; j++)
      printf(" ..");
    printf("%p: pte %p pa %p\n", (void*)nva, (void*)pte, (void*)PTE2PA(pte));
    if((pte & (PTE_R|PTE_W|PTE_X)) == 0)
      vmprintwalk((pagetable_t)PTE2PA(pte), nva, depth + 1);
  }
}

void
vmprint(pagetable_t pagetable) {
  printf("page table %p\n", pagetable);
  vmprintwalk(pagetable, 0, 0);
}
// #endif

int
mappages(pagetable_t pagetable, uint64 va, uint64 size, uint64 pa, int perm)
{
  uint64 a, end;
  pte_t *pte;
  int sz;

  if((va % PGSIZE) != 0)
    panic("mappages: va not aligned");

  if((size % PGSIZE) != 0)
    panic("mappages: size not aligned");

  if(size == 0)
    panic("mappages: size");

  a = va;
  end = va + size;
  while(a < end){
    sz = PGSIZE;
// #ifdef LAB_PGTBL
    // Use a 2MB superpage when VA, PA, and remaining size allow it.
    // Unaligned edges (e.g. around etext) and small mappings
    // (UART, virtio, trampoline, kernel stacks) stay 4KB.
    if((a % SUPERPGSIZE) == 0 && (pa % SUPERPGSIZE) == 0 && (end - a) >= SUPERPGSIZE)
      sz = SUPERPGSIZE;
// #endif
    if((pte = walkat(pagetable, a, 1, sz == PGSIZE ? 0 : 1)) == 0)
      return -1;
    if(*pte & PTE_V)
      panic("mappages: remap");
    *pte = PA2PTE(pa) | perm | PTE_V;
    a += sz;
    pa += sz;
  }
  return 0;
}

/*
Question: demotesuper这个函数是对超级页降级的，然后按照普通页处理，那么超级页的优势又如何体现呢?

降级的必要性（为什么需要 demotesuper？）
尽管超级页有上述优势，但它也有一个致命的缺点：缺乏灵活性。

原子性：对超级页的操作（如修改权限）是针对整个 2MB 区域的。你无法只修改其中 1KB 的权限。
内存效率：如果一个进程只需要使用 4KB 的内存，却分配了 2MB 的超级页，会造成巨大的内存浪费。
☣️写时复制：在实现 COW 时，如果父进程共享一个 2MB 的超级页，而子进程只修改了其中 4KB 的内容，
那么必须将这 2MB 的超级页复制一份，然后只修改那 4KB。这个复制 2MB 数据的操作非常昂贵。更高效的做法是直接降级，只复制和修改那 4KB 的数据。
*/

// #ifdef LAB_PGTBL 
// 判断给定的虚拟地址（va）是否在超级页（superpage）中
static int
issuper(pagetable_t pagetable, uint64 va, pte_t *pte)
{
  return pte != 0 && pte == walkat(pagetable, va, 0, 1) && PTE_LEAF(*pte);
}

// Split a 2MB mapping into 512 ordinary 4KB pages, preserving contents.
/**
 * @brief 将一个超级页面（Superpage，例如2MB）分解为多个常规页面（Regular Page，例如4KB）。
 * 
 * 这个函数用于将一个大页的映射转换成多个小页的映射，同时保持原有的数据内容和访问权限。
 * 这通常在需要修改大页中部分区域的权限，或者释放大页中部分区域的内存时使用。
 *
 * @param pagetable 内存页表的根节点指针
 * @param va 虚拟地址，它必须位于一个有效的超级页面范围内
 */
static void
demotesuper(pagetable_t pagetable, uint64 va)
{
  // 1. 计算包含给定虚拟地址 va 的超级页面的起始地址。
  //    通过将 va 的低 (log2(SUPERPGSIZE)) 位清零来实现对齐。
  //    例如，如果 SUPERPGSIZE 是 2MB (2^21)，则 SUPERPGSIZE-1 是 0x1FFFFF，
  //    取反后 ~(SUPERPGSIZE-1) 是 0xFFE00000，用于清除低21位。
  uint64 start = va & ~(SUPERPGSIZE - 1);

  // 2. 在页表中查找该超级页面起始地址对应的页表项。
  //    walkat 是一个辅助函数，用于遍历多级页表。
  //    参数说明：
  //    - pagetable: 页表根节点
  //    - start: 要查找的虚拟地址
  //    - 0: 表示不需要创建中间页表项（只查找，不创建）
  //    - 1: 表示如果找到最终的页表项，需要返回其指针
  pte_t *spte = walkat(pagetable, start, 0, 1);

  // 声明变量，用于存储物理地址和页表项的标志位
  uint64 spa; // Super Physical Address, 超级页面的物理基地址
  int flags;  // 存储权限标志（如可读、可写、可执行等）

  // 3. 验证查找结果。
  //    - spte == 0: 表示 walkat 没有找到对应的页表项，可能是地址无效。
  //    - !PTE_LEAF(*spte): 表示该页表项不是一个叶子节点，意味着它指向的是下一级页表，
  //      而不是一个实际的物理页面。这与我们期望的超级页面映射矛盾。
  if(spte == 0 || !PTE_LEAF(*spte))
    panic("demotesuper: invalid superpage mapping");

  // 4. 从超级页表项中提取物理地址和权限标志。
  //    - PTE2PA: 一个宏，用于从页表项中解析出物理地址。
  //    - PTE_FLAGS: 一个宏，用于从页表项中提取所有标志位（权限位等）。
  spa = PTE2PA(*spte);
  flags = PTE_FLAGS(*spte);

  // 5. 清除超级页面的映射。
  //    将该页表项置为0，表示该虚拟地址范围不再映射到这个超级页面。
  //    这一步非常重要，因为它切断了虚拟地址到超级页面的直接映射。
  *spte = 0;

  // 6. 循环遍历超级页面中的每一个常规页面。
  //    - SUPERPGSIZE / PGSIZE 计算出一个超级页面包含多少个常规页面（例如 2MB / 4KB = 512）。
  //    - off 是当前常规页面相对于超级页面起始地址的偏移量。
  for(uint64 off = 0; off < SUPERPGSIZE; off += PGSIZE){
    // 6.1 为新的常规页面分配一页物理内存。
    //     kalloc() 是一个内核函数，用于分配一个标准大小的页面（通常是4KB）。
    char *mem = kalloc();
    
    // 检查内存分配是否成功。如果失败，系统将 panic，因为无法完成分解操作。
    if(mem == 0)
      panic("demotesuper: out of memory");

    // 6.2 将原超级页面中对应位置的数据复制到新分配的常规页面中。
    //     - spa + off: 原超级页面中当前常规页面的物理地址。
    //     - mem: 新分配的常规页面的虚拟地址（在内核直接映射区域，物理地址和虚拟地址相同）。
    //     - memmove: 用于内存拷贝，源和目标区域可能重叠，在这里是安全的。

    // Question: 为什么在降级时使用 memmove 而不是像 uvmcopy 那样的标准复制函数？
    // 答案是：因为 demotesuper 和 uvmcopy 解决的是两种完全不同的问题，它们的设计目标和操作场景有本质的区别。
    // 特性	         	demotesuper	         	         uvmcopy
    // 核心操作		   内存复制 (memmove)		         页表复制 (建立映射关系)
    // 数据源		     物理地址 (spa + off)		      虚拟地址 (通过页表查找)
    // 数据目标			  新分配的物理地址 (mem)			   新进程的页表
    // 触发时机			  需要修改超级页的部分内容时		  创建新进程 (fork) 时
    // 核心目的			  提高灵活性，允许精细操作			  实现高效的进程复制
    // 效率考量			  一次性复制，追求速度			     延迟复制，按需复制
    // 典型场景		  munmap 释放部分内存、修改部分区域的权限			     fork、exec 后的地址空间继承
    memmove(mem, (char*)(spa + off), PGSIZE);

    // 6.3 将新的常规页面映射到原来的虚拟地址空间。
    //     - mappages: 一个内核函数，用于在页表中建立一个新的映射关系。
    //     - start + off: 新常规页面要映射到的虚拟地址。
    //     - PGSIZE: 映射的大小（一页）。
    //     - (uint64)mem: 新分配的常规页面的物理地址。
    //     - flags: 从原超级页面复制过来的权限标志。
    //     如果映射失败，系统将 panic。
    if(mappages(pagetable, start + off, PGSIZE, (uint64)mem, flags) != 0)
      panic("demotesuper: failed to map new page");
  }

  // 7. 所有常规页面都已成功映射，现在可以释放超级页面的物理内存了。
  //    - superfree: 一个函数，用于释放之前由超级页分配器分配的内存。
  //    - (void*)spa: 将超级页面的物理地址转换为 void* 类型。
  superfree((void*)spa);
}

// Remove npages of mappings starting from va. va must be
// page-aligned. It's OK if the mappings don't exist.
// Optionally free the physical memory.
void
uvmunmap(pagetable_t pagetable, uint64 va, uint64 npages, int do_free)
{
  uint64 a, end;
  pte_t *pte;

  if((va % PGSIZE) != 0)
    panic("uvmunmap: not aligned");

  end = va + npages*PGSIZE;
  for(a = va; a < end; ){
    if((pte = walk(pagetable, a, 0)) == 0){ // leaf page table entry allocated?
      a += PGSIZE;
      continue;
    }
    if((*pte & PTE_V) == 0){  // has physical page been allocated?
      a += PGSIZE;
      continue;
    }
    // #ifdef LAB_PGTBL
    // 检查当前地址是否在超级页面中
    if(issuper(pagetable, a, pte)){
      // 计算超级页面的起始地址
      uint64 sstart = a & ~(SUPERPGSIZE - 1); // 向下对齐
      // 计算超级页面的结束地址
      uint64 send = sstart + SUPERPGSIZE;
      
      // 如果操作范围覆盖整个超级页面
      if(a == sstart && end >= send){
        // 如果需要，释放整个超级页面的物理内存
        if(do_free)
          superfree((void*)PTE2PA(*pte));
        // 清除页表映射
        *pte = 0;
        // 跳过整个超级页面，继续处理
        a += SUPERPGSIZE;
        continue;
      }
      // 如果只操作超级页面的一部分，则将其降级为普通页面
      demotesuper(pagetable, a);
      continue;
    }

#endif
    if(PTE_FLAGS(*pte) == PTE_V)
      panic("uvmunmap: not a leaf");
    if(do_free){
      uint64 pa = PTE2PA(*pte);
      kfree((void*)pa);
    }
    *pte = 0;
    a += PGSIZE;
  }
}


// Allocate PTEs and physical memory to grow process from oldsz to
// newsz, which need not be page aligned.  Returns new size or 0 on error.
uint64
uvmalloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz, int xperm)
{
  char *mem;
  uint64 a;
  int sz;

  if(newsz < oldsz)
    return oldsz;

  oldsz = PGROUNDUP(oldsz);
  for(a = oldsz; a < newsz; a += sz){
    sz = PGSIZE;
#ifdef LAB_PGTBL
    if((a % SUPERPGSIZE) == 0 && newsz - a >= SUPERPGSIZE){
      mem = superalloc();
      if(mem){
        sz = SUPERPGSIZE;
#ifndef LAB_SYSCALL
        memset(mem, 0, sz);
#endif
        if(mappages(pagetable, a, sz, (uint64)mem, PTE_R|PTE_U|xperm) != 0){
          superfree(mem);
          uvmdealloc(pagetable, a, oldsz);
          return 0;
        }
        continue;
      }
    }
#endif
    mem = kalloc();
    if(mem == 0){
      uvmdealloc(pagetable, a, oldsz);
      return 0;
    }
#ifndef LAB_SYSCALL
    memset(mem, 0, sz);
 #endif
    if(mappages(pagetable, a, sz, (uint64)mem, PTE_R|PTE_U|xperm) != 0){
      kfree(mem);
      uvmdealloc(pagetable, a, oldsz);
      return 0;
    }
  }
  return newsz;
}


int
uvmcopy(pagetable_t old, pagetable_t new, uint64 sz)
{
  pte_t *pte;
  uint64 pa, i;
  uint flags;
  char *mem;
  int szinc = PGSIZE;

  for(i = 0; i < sz; i += szinc){
    szinc = PGSIZE;
    if((pte = walk(old, i, 0)) == 0)
      continue;
    if((*pte & PTE_V) == 0) {
      continue;
    }
#ifdef LAB_PGTBL
    if((i % SUPERPGSIZE) == 0 && issuper(old, i, pte)){
      szinc = SUPERPGSIZE;
      pa = PTE2PA(*pte);
      flags = PTE_FLAGS(*pte);
      if((mem = superalloc()) == 0)
        goto err;
      memmove(mem, (char*)pa, SUPERPGSIZE);
      if(mappages(new, i, SUPERPGSIZE, (uint64)mem, flags) != 0){
        superfree(mem);
        goto err;
      }
      continue;
    }
#endif
    pa = PTE2PA(*pte);
    flags = PTE_FLAGS(*pte);
    if((mem = kalloc()) == 0)
      goto err;
    memmove(mem, (char*)pa, PGSIZE);
    if(mappages(new, i, PGSIZE, (uint64)mem, flags) != 0){
      kfree(mem);
      goto err;
    }
  }
  return 0;

 err:
  uvmunmap(new, 0, i / PGSIZE, 1);
  return -1;
}
```
