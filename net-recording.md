# Tulip（Intel e1000）网卡驱动讲解

> 对应文件：`kernel/tulip.c`
> 硬件：QEMU 模拟的 Intel 82540EM（`-device e1000,netdev=net0,bus=pcie.0`）
> 在本仓库里课程把这个实验叫作 "Tulip 驱动"，但网卡实际是 e1000，因此驱动对外沿用内核约定的
> `e1000_*` 接口名（`pci.c` / `net.c` / `trap.c` / `defs.h` 都是按这个名字调用的）。

---

## 0. 整体思路

网卡驱动的本质只有两件事：

1. **初始化（Step One）**：告诉网卡"发送环和接收环在哪里、缓冲区长什么样"，然后打开发送、接收和中断。以后网卡自己通过 DMA 读写内存，CPU 只负责在环上放/取描述符。
2. **收发**：
   - 发送 = 把一个以太网帧的地址和长度填进"发送描述符"，再踢一下网卡；
   - 接收 = 网卡把收到的包 DMA 到缓冲区，并在描述符上打一个 "DD(done)" 标记，驱动看到标记后把包取走，再补一个新缓冲区到环上。

关键概念是**描述符环（descriptor ring）**：一块连续内存，里面是一串描述符；网卡和驱动各持有头/尾指针在环上协作。

---

## 1. 硬件模型：寄存器 + DMA

e1000 暴露两类东西：

- **寄存器（MMIO）**：映射在物理地址 `0x40000000`（由 `pci.c` 把 BAR 指到这里，`vm.c` 建立映射）。
  驱动用 `regs[...]` 读写，下标是"寄存器偏移 ÷ 4"。
- **DMA 内存**：描述符环和数据缓冲区，都在普通物理内存里。网卡直接按描述符里写的**物理地址**去读写。

> xv6 内核把物理内存**恒等映射**（虚拟地址 == 物理地址），所以代码里 `(uint64)ptr` 直接就能当 DMA 物理地址用。

---

## 2. 数据结构

```c
#define TX_RING_SIZE 16
static struct tx_desc tx_ring[TX_RING_SIZE] __attribute__((aligned(16)));

#define RX_RING_SIZE 16
static struct rx_desc rx_ring[RX_RING_SIZE] __attribute__((aligned(16)));

static volatile uint32 *regs;      // 网卡寄存器基址
struct spinlock e1000_lock;        // 保护发送环
```

描述符结构定义在 `kernel/e1000_dev.h`：

```c
struct tx_desc {          // 发送描述符（Legacy 格式）
  uint64 addr;            // 数据缓冲区物理地址
  uint16 length;          // 帧长度
  uint8  cso, cmd, status, css;
  uint16 special;
};

struct rx_desc {          // 接收描述符
  uint64 addr;            // 数据缓冲区物理地址
  uint16 length;          // 网卡写入的包长度
  uint16 csum;
  uint8  status;          // DD / EOP 等
  uint8  errors;
  uint16 special;
};
```

常用位（`e1000_dev.h`）：

| 名字 | 值 | 含义 |
|---|---|---|
| `E1000_TXD_CMD_EOP` | 0x01 | End Of Packet，一帧的最后一个描述符 |
| `E1000_TXD_CMD_RS`  | 0x08 | Report Status，发完后让网卡回写 DD |
| `E1000_TXD_STAT_DD` | 0x01 | 发送描述符已完成 |
| `E1000_RXD_STAT_DD` | 0x01 | 接收描述符已被网卡填好 |
| `E1000_RXD_STAT_EOP`| 0x02 | 一帧的结束 |

---

## 3. `e1000_init()`：Step One 初始化

被 `pci_init()` 在探测到 PCI id `0x100e8086` 后调用，传入寄存器基址。

```c
initlock(&e1000_lock, "e1000");
regs = xregs;
```

### 3.1 复位

```c
regs[E1000_IMS] = 0;              // 先关中断
regs[E1000_CTL] |= E1000_CTL_RST; // 置 RST 位复位
regs[E1000_IMS] = 0;              // 复位后再关一次
__sync_synchronize();             // 内存屏障，保证顺序
```

### 3.2 发送环（TX）

```c
memset(tx_ring, 0, sizeof(tx_ring));
for (i = 0; i < TX_RING_SIZE; i++) {
  tx_ring[i].status = E1000_TXD_STAT_DD;  // 初始标记为"空闲/已完成"
  tx_ring[i].addr = 0;
}
regs[E1000_TDBAL] = (uint64) tx_ring;     // 环的基址
regs[E1000_TDLEN] = sizeof(tx_ring);      // 环字节数（必须 128 的整数倍）
regs[E1000_TDH] = regs[E1000_TDT] = 0;    // 头尾都指向 0
```

- **TDBAL / TDLEN**：告诉网卡发送环的基址和长度。
- **TDH / TDT**：Head / Tail。网卡消费到 TDH，驱动从 TDT 处放新描述符。
- 初始把每个描述符的 `status` 设为 **DD**，含义是"这一格可以立刻使用"，这样前 16 次发送天然可用。

### 3.3 接收环（RX）

```c
memset(rx_ring, 0, sizeof(rx_ring));
for (i = 0; i < RX_RING_SIZE; i++) {
  rx_ring[i].addr = (uint64) kalloc();   // 每格先挂一个缓冲区
  if (!rx_ring[i].addr) panic("e1000");
}
regs[E1000_RDBAL] = (uint64) rx_ring;
regs[E1000_RDLEN] = sizeof(rx_ring);
regs[E1000_RDH] = 0;
regs[E1000_RDT] = RX_RING_SIZE - 1;      // 尾指向最后一格
```

- 每个接收描述符都要预先挂好一块 DMA 缓冲区，网卡才有地方写包。
- **RDT 初始为 `N-1`**：表示"驱动已处理到最后一格"，于是网卡从第 0 格开始填。`RDT+1` 就是驱动认为下一个可以处理的格子。
- 缓冲区大小由 `RCTL` 的 `SZ_2048` 决定（我们直接给了一整页 `PGSIZE`，足够）。

### 3.4 MAC 地址过滤

```c
regs[E1000_RA]   = 0x12005452;             // MAC 低 4 字节 52:54:00:12
regs[E1000_RA+1] = 0x5634 | (1<<31);       // MAC 高 2 字节 34:56，bit31=AV 有效位
for (i = 0; i < 4096/32; i++)
  regs[E1000_MTA + i] = 0;                 // 清空多播表
```

QEMU 给这块网卡的 MAC 是 `52:54:00:12:34:56`。**RA**（Receive Address）写的是小端排列的 MAC，最高位 AV=1 表示启用该过滤项。这里顺便把组播表清零。

### 3.5 发送/接收控制寄存器

```c
// 发送控制
regs[E1000_TCTL] = E1000_TCTL_EN            // 使能发送
                 | E1000_TCTL_PSP           // 短包自动补齐
                 | (0x10 << E1000_TCTL_CT_SHIFT)    // 冲突阈值
                 | (0x40 << E1000_TCTL_COLD_SHIFT); // 冲突距离
regs[E1000_TIPG] = 10 | (8<<10) | (6<<20);  // 包间隔 IPG

// 接收控制
regs[E1000_RCTL] = E1000_RCTL_EN            // 使能接收
                 | E1000_RCTL_BAM           // 收广播
                 | E1000_RCTL_SZ_2048       // 缓冲区按 2048 计
                 | E1000_RCTL_SECRC;        // 剥掉 4 字节 CRC
```

到这里发送、接收都打开了。

### 3.6 中断

```c
regs[E1000_RDTR] = 0;      // 不用定时器
regs[E1000_RADV] = 0;      // 不用绝对延时
regs[E1000_IMS]  = (1 << 7); // RXDW：每收到一个包就中断一次
```

`IMS` = Interrupt Mask Set，`1<<7` 是 RXDW（Receiver Descriptor Write Back）—— 网卡每往接收环写回一个描述符就产生中断，去触发 `e1000_intr()`。

---

## 4. `e1000_transmit(buf, len)`：发送一个以太网帧

调用方（`net.c` 的 `sys_send` / `arp_rx`）用 `kalloc()` 造好帧后传进来，**把缓冲区的所有权交给驱动**（因此驱动负责最终 `kfree`）。返回 0 成功，-1 表示环满（调用方自己回收 buf）。

```c
int e1000_transmit(char *buf, int len)
{
  acquire(&e1000_lock);                    // 串行化对发送环的访问

  uint32 index = regs[E1000_TDT];          // 当前可用的格子 = 尾指针

  if ((tx_ring[index].status & E1000_TXD_STAT_DD) == 0) {
    release(&e1000_lock);
    return -1;                             // 上一圈还没发完，环满
  }

  if (tx_ring[index].addr)                 // 回收这一格上一轮用过的缓冲区
    kfree((void *) tx_ring[index].addr);

  tx_ring[index].addr   = (uint64) buf;    // 填新帧
  tx_ring[index].length = len;
  tx_ring[index].status = 0;               // 清 DD，等网卡写完再置位
  tx_ring[index].cmd    = E1000_TXD_CMD_EOP | E1000_TXD_CMD_RS;

  regs[E1000_TDT] = (index + 1) % TX_RING_SIZE;  // 推进尾指针，通知网卡

  release(&e1000_lock);
  return 0;
}
```

要点：

- **判断格子是否空闲**看 `status & DD`。初始 16 格都是 DD，所以一开始就有 16 个可用槽。
- **不能立刻 `kfree(buf)`**：网卡可能还在 DMA 读这块内存。做法是把指针存进描述符，等下一圈用到同一格、且 DD 已置位（说明上一包发完了）时再 `kfree`。
- 设置 `RS` 后，网卡发完会把该描述符 `status` 的 DD 位置 1，作为"这格空了"的信号。
- 最后写 `TDT`（推进尾指针）才真正"通知"网卡去发。

---

## 5. `e1000_recv()`：接收

在中断上下文里被 `e1000_intr()` 调用，循环把接收环里所有已完成的包取出交给上层 `net_rx()`。

```c
static void e1000_recv(void)
{
  while (1) {
    uint32 index = (regs[E1000_RDT] + 1) % RX_RING_SIZE;   // 下一格待处理

    if ((rx_ring[index].status & E1000_RXD_STAT_DD) == 0)
      return;                                              // 没新包了，退出

    // 交给网络栈；net_rx 会接管并最终释放这块缓冲区
    net_rx((char *) rx_ring[index].addr, rx_ring[index].length);

    // 给这一格补一块新缓冲区，重新挂上
    char *buf = kalloc();
    if (buf == 0) panic("e1000_recv");
    rx_ring[index].addr   = (uint64) buf;
    rx_ring[index].status = 0;

    regs[E1000_RDT] = index;                               // 推进尾指针，把这格还给网卡
  }
}
```

要点：

- 驱动拥有 `RDT+1 .. RDH-1` 之间的格子；判断"有没有包"看 `DD`。
- **先取包再补新 buffer**：`net_rx()` 会消费掉旧 buffer（释放或转发），所以必须重新 `kalloc()` 一块挂回去。
- 处理完把 `RDT` 推进到 `index`，表示这格已归还网卡。

`net_rx()`（`kernel/net.c`）的分发：

- 以太类型 = `ARP` → `arp_rx()`：打印 `arp_rx: received an ARP packet`，回一个 ARP reply（走 `e1000_transmit`），再 `kfree`。
- 以太类型 = `IP`  → `ip_rx()`：打印 `ip_rx: received an IP packet`（Part Two 再补 UDP 解析与 `kfree`）。
- 其它 → 直接 `kfree`。

---

## 6. `e1000_intr()`：中断处理

```c
void e1000_intr(void)
{
  regs[E1000_ICR] = 0xffffffff;  // 读 ICR 清中断原因，否则网卡不再产生中断
  e1000_recv();                  // 把收到的包取走
}
```

`ICR`（Interrupt Cause Read）读一次即清除中断；再调用 `e1000_recv()` 处理接收环。

---

## 7. 和其它内核代码怎么串起来

```
main.c: pci_init() ──► pci.c 探测 0x100e8086 ──► e1000_init(0x40000000)
        netinit()  ──► net.c 初始化锁

用户进程 send() ─► sys_send() ─► e1000_transmit() ─► 网卡发出
                                          ▲
网卡收到包 ─► PLIC(IRQ=33) ─► trap.c devintr() ─► e1000_intr() ─► e1000_recv()
                                                                      │
                                                              net_rx() ─► arp_rx()/ip_rx()
```

- `pci.c`：把网卡 BAR 映射到 `0x40000000`，调用 `e1000_init`。
- `plic.c`：使能 PCIe 的 IRQ 32–35；`E1000_IRQ = 33`（`memlayout.h`）。
- `trap.c`：`devintr()` 在 `irq == E1000_IRQ` 时调用 `e1000_intr()`。
- `net.c`：`sys_send()` 组帧后调 `e1000_transmit()`；`net_rx()` 分发到 `arp_rx()` / `ip_rx()`。

---

## 8. 易错点小结

1. **缓冲区所有权**：`transmit` 里不能马上 `kfree(buf)`，要等描述符被回收（DD 置位）再释放；`recv` 里 `net_rx` 会释放旧 buffer，必须补新 buffer。
2. **DD 位语义**：发送描述符初始全为 DD（空闲）；发送前清 DD，靠网卡发完置回 DD。接收靠 DD 判断网卡是否写好。
3. **环基址对齐**：`TDBAL/RDBAL` 需 16 字节对齐，`TDLEN/RDLEN` 需 128 的整数倍（代码里有 `panic` 兜底）。
4. **指针即物理地址**：xv6 恒等映射，`(uint64)ptr` 可直接当 DMA 地址。
5. **中断必须先清 `ICR`**，否则网卡不会再中断。
6. **锁**：`e1000_lock` 保护发送环的读改写；接收在中断上下文，注意别和发送路径产生死锁（本实现里接收不碰发送环，安全）。

---

## 9. 完成度

- ✅ **Part One（本部分）**：驱动初始化 + 收发。
  - `txone`：`nettest.py` 收到 xv6 发出的 UDP 包 → `txone: OK`。
  - `rxone`：内核打印 `arp_rx: received an ARP packet` 与 `ip_rx: received an IP packet`。
- ⏳ **Part Two（后续）**：`net.c` 里的 `sys_bind` / `sys_recv` / `ip_rx` 解析，才能通过 `ping0..3`、`dns`、`free`。
  - 目前 `ip_rx` 是空桩、不释放缓冲区，每个 IP 包会漏一页，补上 `kfree` 后 `free: OK` 才有望通过。
