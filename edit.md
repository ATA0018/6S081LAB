# PAGE TABLES LAB: SPEED UP SYSTEM CALLS (success)

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
