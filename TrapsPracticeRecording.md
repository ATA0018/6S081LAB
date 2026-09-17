Number one: RISC-V assembly (knowledge supplement)

Background: Read the code in call.asm for the functions g, f, and main. The instruction manual for RISC-V is on the reference page.

int g(int x) {
  return x+3;
}

addi	sp,sp,-16  调整栈指针，分配16字节栈空间
sd	ra,8(sp) 保存寄存器ra和s0的值到栈中
sd	s0,0(sp)
addi	s0,sp,16    
addiw	a0,a0,3      将参数a0（x）加3，结果存回a0     	 

2c:	4635                	li	a2,13    # 将13加载到a2寄存器
2e:	45b1                	li	a1,12    # 将12加载到a1寄存器
30:	00001517          	auipc	a0,0x1   # 将格式字符串地址加载到a0寄存器


在RISC-V架构中，函数参数是通过以下寄存器传递的：

1. 参数传递寄存器：
   - a0: 第一个参数
   - a1: 第二个参数
   - a2: 第三个参数
   - a3: 第四个参数
   - a4-a7: 第五到第八个参数

2. 在main函数调用printf时：
   - a0: 指向格式字符串"%d %d\n"的指针
   - a1: 第一个参数值，即f(8)+1的结果（12）
   - a2: 第二个参数值，即13

所以，在main的printf调用中，数字13存储在a2寄存器中。这可以从汇编代码中得到验证：
```assembly
li a2,13     # 将13加载到a2寄存器
li a1,12     # 将12加载到a1寄存器
# a0已经在前面加载了格式字符串的地址
```

这种参数传递方式是RISC-V函数调用约定的一部分，它规定了函数参数如何通过寄存器传递，以及当参数超过8个时如何通过栈传递。

# 提升系统性能的方法
赏析：“That in turn would allow system call implementations in the kernel to take advantage of the current process’s user memory being mapped, allowing kernel code to directly dereference user pointers.
Many operating systems have used these ideas to increase efficiency. 

-- Xv6 avoids them in order to reduce the chances of security bugs
in the kernel due to inadvertent use of user pointers, and to reduce some complexity that would be
required to ensure that user and kernel virtual addresses don’t overlap. --”

### 模式 1：直接解引用（很多真实 OS 使用）

进程陷入内核之后，**仍然保留该进程完整的用户页表映射**。
系统调用拿到用户传进来的指针，内核直接 `*user_ptr` 读写，不需要专门拷贝函数。
✅优点：省去`copyin/copyout`拷贝，性能更高。
❌带来两大代价，也就是文中说的 **security bugs + complexity**

### 模式 2：xv6 现在的做法

系统调用收到用户指针，**不能直接解引用**，必须调用专门函数：
`copyin()` / `copyout()`。
在内核虚拟地址和用户虚拟地址之间做安全校验 + 内存拷贝。
xv6 的页表：每个进程页表同时包含用户地址空间 + 全局内核高地址映射；
但内核代码**绝不直接解用户传来的虚拟指针**，强制走 copyin/copyout。


要支持 “内核直接解引用用户指针”，必须保证：
**用户程序使用的虚拟地址范围，绝对不能和内核使用的虚拟地址范围重叠。**

维护不重叠带来一系列工程复杂度：

1. **地址空间划分要严谨**：要固定、审计用户 / 内核的地址区间，不能让 mmap、用户程序 brk 扩张越界伸进内核地址区域。
2. 内存分配、`brk`、`mmap`全部要增加边界检查，防止用户分配到内核的虚拟地址区间。
3. 内核的各个子系统、驱动、内核内存分配器，都不能把内核对象分配到属于用户的虚拟地址段。
4. 如果将来要修改内核布局、支持更多特性，还要持续维护这套隔离规则。

现实 Linux：是第一种思路，内核可以直接访问用户地址；但是内核开发中要非常小心指针校验，历史上大量漏洞就来自对用户指针的错误处理。xv6 作为教学系统，优先降低复杂度、减少安全 bug，牺牲一部分性能。

Number Two: Backtrace.
Question: For debugging it is often useful to have a backtrace: a list of the function calls on the stack above the point at which the error occurred. To help with backtraces, the compiler generates machine code that maintains a stack frame on the stack corresponding to each function in the current call chain. Each stack frame consists of the return address and a "frame pointer" to the caller's stack frame. Register s0 contains a pointer to the current stack frame (it actually points to the the address of the saved return address on the stack plus 8). Your backtrace should use the frame pointers to walk up the stack and print the saved return address in each stack frame. 

In hints:
``` c 
// kernel/riscv.h
static inline uint64
r_fp()
{
  uint64 x;
  asm volatile("mv %0, s0" : "=r" (x) );
  return x;
}
```

Operations:
```c
// kernel/printf.c
void
backtrace(void)
{
  uint64 fp = r_fp();
  uint64 stack_bottom = PGROUNDDOWN(fp);

  while (fp > stack_bottom) {
    uint64 ra = *(uint64 *)(fp - 8);
    printf("%p\n", (void *)ra);
    fp = *(uint64 *)(fp - 16);
  }
}

// kernel/defs.h
void            backtrace(void);

// kernel/sysproc.c
uint64
sys_pause(void)
{
  int n;
  uint ticks0;

  backtrace(); // 打印当前函数的调用栈
  
  argint(0, &n);
  if(n < 0)
    n = 0;
  acquire(&tickslock);
  ticks0 = ticks;
  while(ticks - ticks0 < n){
    if(killed(myproc())){
      release(&tickslock);
      return -1;
    }
    sleep(&ticks, &tickslock);
  }
  release(&tickslock);
  return 0;
}
```
## 图解backtrace 的栈帧使用情况
高地址
        ┌──────────────┐
        │ 调用者的帧    │
        ├──────────────┤
        │ 保存的 s0     │  ← s0       （指向调用者帧）
        ├──────────────┤
        │ 返回地址(ra)  │  ← s0 - 8   （低地址方向）
        ├──────────────┤
        │ 局部变量...   │  ← s0 - 16  （更低地址）
        └──────────────┘
低地址

Note: backtrace()不要放在持有锁之外、不要放在 sleep 之后做对比
`sleep()`会触发上下文切换，线程被切出去；当进程被时钟中断唤醒，从`sleep`返回继续执行 while 循环。
如果你把`backtrace`放在`sleep(...)`的**后面**：此时打印的是**被唤醒恢复执行时的调用栈**，栈内容和进入 sys_pause 是一样的，因为内核栈帧没有销毁。

为什么 sleep 做上下文切换，再次回来之后 backtrace 还能正常工作？
答：上下文切换只是把线程换出 CPU，**内核栈完整保留所有栈帧 (fp、ra)**；进程重新调度回来继续在原来栈上执行，栈回溯链表完好。

测试：
1、make qemu 
``` bash
$ bttest
backtrace:
0x0000000080001e4a
0x0000000080001d38
0x0000000080001ac2
0x0000003ffffff09c
# 退出qemu
$ riscv64-unknown-elf-addr2line -e kernel/kernel 
# riscv64-unknown-elf-addr2line -e kernel/kernel << EOF（明亮跑完直接退出）
ctrl + c复制
0x0000000080001e4a
0x0000000080001d38
0x0000000080001ac2

# You will look at the following text:
/Users/yourmac/Desktop/A501/底层操作系统原理xv-6/xv6-labs-2025/kernel/sysproc.c:73
/Users/yourmac/Desktop/A501/底层操作系统原理xv-6/xv6-labs-2025/kernel/syscall.c:141 (discriminator 1)
/Users/yourmac/Desktop/A501/底层操作系统原理xv-6/xv6-labs-2025/kernel/trap.c:80

# ctrl + D 退出 
```

Number Three: Alarm.(primitive user-level interrupt handler)
Question: The Unix alarm system call sets a timer that will expire after a given number of seconds. When the timer expires, a signal is sent to the process. The signal is SIGALRM by default, but can be changed with the setitimer system call. The system call takes a timeval structure as an argument, which specifies the interval between timer expirations.

解读：alarm lab 本质：**利用时钟中断，在内核中统计进程的 CPU 运行时间；到达阈值后，修改 trapframe，让进程返回用户态时执行预先注册好的用户回调函数，实现用户态的周期性中断处理。**
# 实验核心逻辑流程（简要）

1. 用户调用 `alarm(n, fn)` 系统调用，内核保存：告警间隔`n`、用户 handler 函数地址`fn`，初始化该进程的**CPU 运行 tick 计数器**。
2. 每次**时钟中断（timer interrupt）**进入内核。
3. 内核判断：当前是这个进程在 CPU 上运行 → 进程的 tick 计数器 + 1。
4. 如果计数器 == alarm 设置的 interval：
   - 保存当前进程的用户态上下文（trapframe 寄存器）
   - 修改 trapframe，使得**返回用户态时，直接跳转到 handler 函数**
   - 标记该进程 alarm 已经触发（防止递归嵌套触发 alarm）
5. handler 执行完毕后，用户调用`sigreturn()`系统调用，恢复原来的用户上下文，回到被打断的代码继续运行。

##  应用场景原文提到两类

1. **限制 CPU 占用**：计算密集型程序，定时检查自己运行多久，超时主动停止 / 降速。
2. **周期性动作**：一边持续做计算，每隔固定 CPU 时间，执行回调函数（类似定时器）。

## 场景一：限制 CPU 占用	场景二：周期性动作
核心目的	防止算太久 / 控制算力上限	------- 在计算中定期插入其他动作
回调里做什么	检查超时，停止 / 降速 / 退出	------- 保存进度、刷新界面、上报数据
回调执行完	可能直接退出进程	------- 继续回到计算循环
心智模型	看门狗 / 预算提醒	------- 定时巡检 / 心跳

---

# 最终设计：`alarm_tf` 用懒分配（推荐）

前面那段 2.3 里我给了两种方案，容易看混。**最终定稿只采用懒分配方案**，逻辑最干净、fork 不用特殊处理、也不会泄漏。下面把涉及 `alarm_tf` 生命周期的地方全部统一成这一套。

---

## 核心原则

- `alarm_tf` **不在 `allocproc()` 里分配**
- `allocproc()` 只把 `p->alarm_tf = 0`
- **第一次 `sigalarm(interval>0, handler)` 时**才 `kalloc`，即"懒分配"
- `freeproc()` 里统一 `if(p->alarm_tf) kfree(p->alarm_tf);`
- `fork()` **完全不用管 `alarm_tf`**——因为 `fork()` 里 `*np = *p` 会复制父进程的 `alarm_tf` 指针，所以这里仍需处理。见下文。

> ⚠️ 注意：懒分配解决的是"allocproc 里分配后被 `*np = *p` 覆盖导致泄漏"的问题，但**并没有**解决父子共享指针的问题。`fork()` 里 `*np = *p` 之后，`np->alarm_tf` 仍然指向父进程那块。所以 `fork()` 里必须**把子进程的 `alarm_tf` 置 0**（配合懒分配，子进程将来自己 sigalarm 时再分配）。这才是懒分配方案的完整闭环。

---

## 1. `kernel/proc.h`

```c
  // ===== alarm lab =====
  int alarm_interval;          // 0 = 关闭
  uint64 alarm_handler;        // 用户回调地址
  int alarm_ticks;             // 累计 CPU tick
  struct trapframe *alarm_tf;  // 懒分配，可能为 0
  int alarm_handling;          // 防嵌套
```

---

## 2. `kernel/proc.c`

### 2.1 `allocproc()`：只清 0，不分配

在初始化字段处：

```c
  // ===== alarm lab =====
  p->alarm_interval = 0;
  p->alarm_handler  = 0;
  p->alarm_ticks    = 0;
  p->alarm_tf       = 0;   // 懒分配，这里不 kalloc
  p->alarm_handling = 0;
```

> **删掉**之前"在 allocproc 里 `kalloc` alarm_tf"的代码。

### 2.2 `freeproc()`：统一释放

```c
  // ===== alarm lab =====
  if(p->alarm_tf)
    kfree((void*)p->alarm_tf);
  p->alarm_tf = 0;
  p->alarm_interval = 0;
  p->alarm_handler  = 0;
  p->alarm_ticks    = 0;
  p->alarm_handling = 0;
```

### 2.3 `fork()`：把子进程的 `alarm_tf` 置 0

在 `fork()` 里 `*np = *p;` 之后：

```c
  // ===== alarm lab: 子进程不继承 alarm =====
  np->alarm_interval = 0;
  np->alarm_handler  = 0;
  np->alarm_ticks    = 0;
  np->alarm_handling = 0;
  np->alarm_tf       = 0;   // 关键：断开与父进程共享的指针
```

这样：

- 不会共享父进程的 `alarm_tf` ✅
- 不会有旧块泄漏（因为 allocproc 没分配过）✅
- 子进程将来若自己 `sigalarm`，会在 `sys_sigalarm` 里懒分配 ✅

---

## 3. `kernel/sysproc.c` — `sys_sigalarm` 懒分配

```c
uint64
sys_sigalarm(void)
{
  int interval;
  uint64 handler;
  struct proc *p = myproc();

  argint(0, &interval);
  argaddr(1, &handler);

  if(interval < 0)
    return -1;

  // 懒分配：第一次注册（或 handler 非 0）时分配
  if(interval != 0 && p->alarm_tf == 0){
    p->alarm_tf = (struct trapframe *)kalloc();
    if(p->alarm_tf == 0)
      return -1;
  }

  p->alarm_interval = interval;
  p->alarm_handler  = handler;
  p->alarm_ticks    = 0;
  p->alarm_handling = 0;

  return 0;
}
```

> `interval == 0`（关闭）时不需要 `alarm_tf`，即使为 0 也没关系，因为关闭后不会再触发。

`sys_sigreturn` 不变：

```c
uint64
sys_sigreturn(void)
{
  struct proc *p = myproc();

  // 恢复被时钟中断打断时的完整用户现场（包括 a0、epc、所有通用寄存器）
  *p->trapframe = *p->alarm_tf;

  // re-arm：为下一轮告警做准备
  p->alarm_ticks    = 0;
  p->alarm_handling = 0;

  // ★ 关键：返回被恢复的 a0，抵消 syscall() 对 trapframe->a0 的覆盖
  return p->trapframe->a0;
}
```

> 因为 `alarm_interval != 0` 保证了 `alarm_tf` 已分配，这里不会空指针。

---

## 三者对比：为什么选懒分配

| 方案 | allocproc | fork | freeproc | 泄漏风险 |
|---|---|---|---|---|
| A. allocproc 分配 | kalloc | 需重新 kalloc + 处理旧块 | kfree | 有（fork 覆盖指针） |
| B. 懒分配（**采用**） | 不分配 | 只需置 0 | kfree | 无 |
| C. 每次 sigalarm 都重分配 | 不分配 | 置 0 | kfree | 无（但要先 free 旧的） |

方案 B 是最平衡的：改动少、无泄漏、fork 处理简单。



> `interval == 0` 时同样写入 interval=0、handler=用户给的值（通常是 0）、ticks=0，实现"关闭闹钟"。

---

## 4. `kernel/syscall.h` — 声明系统调用号

```c
#define SYS_sigalarm  22
#define SYS_sigreturn 23
```

> 号要与 `user/usys.pl`、`kernel/syscall.c` 一致。若 22/23 已被占用，选空闲号。

---

## 5. `kernel/syscall.c` — 注册分发

在 `syscall.c` 的 `syscalls[]` 数组里加入：

```c
extern uint64 sys_sigalarm(void);
extern uint64 sys_sigreturn(void);

static uint64 (*syscalls[])(void) = {
  // ... 已有的 ...
  [SYS_sigalarm]  sys_sigalarm,
  [SYS_sigreturn] sys_sigreturn,
};
```

（`extern` 声明可放在数组上方。）

---

## 6. `user/usys.pl` — 生成用户态 stub

在 `usys.pl` 末尾加入：

```perl
entry("sigalarm");
entry("sigreturn");
```

这会生成 `user/usys.S` 里的 `sigalarm` / `sigreturn` 汇编 stub。

---

## 7. `user/user.h` — 用户态声明

```c
int sigalarm(int ticks, void (*handler)());
int sigreturn(void);
```

---

## 8. `kernel/trap.c` — `usertrap()` 中处理时钟中断

找到 `usertrap()` 里 `if(which_dev == 2)`（时钟中断）分支，改成：

```c
  if(which_dev == 2){
    // ===== alarm lab =====
    if(p->alarm_interval != 0 && !p->alarm_handling){
      p->alarm_ticks++;
      if(p->alarm_ticks >= p->alarm_interval){
        // 触发 alarm
        p->alarm_handling = 1;
        // 保存原始现场
        *p->alarm_tf = *p->trapframe;
        // 返回用户态时跳转到 handler
        p->trapframe->epc = p->alarm_handler;
        // a0 一般不需要设置，handler 无参
      }
    }
    yield();
  }
```

关键点：

- **只在 `which_dev == 2` 时计数**，其它 trap 不计数 ✅
- 先保存 `*p->alarm_tf = *p->trapframe;`，再改 `epc` ✅
- `p->alarm_handling = 1` 在改 `epc` 之前设置 ✅

> 注意：`yield()` 是 xv6 原有的时钟中断处理，别漏掉。

---

## 9. `Makefile` — 加入 alarmtest

```makefile
UPROGS=\
	$U/_cat\
  ······
	$U/_alarmtest\
```

（`alarmtest.c` 需从 lab 提供的 `user/alarmtest.c` 拷入。）

---

## 10. 完整改动清单汇总

| 文件 | 改动 |
|---|---|
| `kernel/proc.h` | 加 5 个字段 |
| `kernel/proc.c` | `allocproc` 初始化字段；`freeproc` 释放 `alarm_tf` |
| `kernel/sysproc.c` | `sys_sigalarm` / `sys_sigreturn` |
| `kernel/syscall.h` | 加 2 个号 |
| `kernel/syscall.c` | 注册 2 个函数 |
| `kernel/trap.c` | `usertrap` 时钟中断分支处理 alarm |
| `user/usys.pl` | 加 2 个 entry |
| `user/user.h` | 加 2 个声明 |
| `Makefile` | UPROGS 加 `_alarmtest` |

---

## 11. 测试

```bash
make qemu
$ alarmtest test0
$ alarmtest test1
$ alarmtest test2
$ usertests
```

预期：`test0` 打印若干 `alarm!`，`test1`/`test2` 通过（`test2` 验证寄存器恢复是否正确，是最容易挂的一项）。


用户程序
  │
  ├─ sigalarm(n, fn) ──ecall──► sys_sigalarm
  │                              │ 写 p->alarm_interval/handler
  │                              │ 懒分配 p->alarm_tf
  │                              └─ return 0
  │
  ├─ 正常运行 ... 时钟中断
  │        │
  │        ▼
  │   usertrap (which_dev==2)
  │        │ alarm_ticks++
  │        │ if 到点 && !handling:
  │        │     *alarm_tf = *trapframe
  │        │     trapframe->epc = handler
  │        │     handling = 1
  │        └─ usertrapret ──► 跳到 handler
  │
  ├─ handler (periodic)
  │        │ 打印 / 改寄存器
  │        └─ sigreturn() ──ecall──► sys_sigreturn
  │                                    │ *trapframe = *alarm_tf
  │                                    │ ticks=0, handling=0
  │                                    └─ return trapframe->a0
  │
  └─ 回到被打断的指令，继续跑

$ alarmtest
........................................alarm!
test2 passed
test3 start
test3 passed

$ usertests -q
usertests starting
OK
test lazy_copy: OK
ALL TESTS PASSED