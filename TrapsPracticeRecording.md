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

Number Three: Alarm.
Question: The Unix alarm system call sets a timer that will expire after a given number of seconds. When the timer expires, a signal is sent to the process. The signal is SIGALRM by default, but can be changed with the setitimer system call. The system call takes a timeval structure as an argument, which specifies the interval between timer expirations.