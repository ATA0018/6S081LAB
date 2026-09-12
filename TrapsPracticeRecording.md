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

