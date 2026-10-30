xv6 那种**单链表纯 LRU 的致命问题：顺序扫描大文件（流式 workload）每一页访问一次，全部冲到 MRU，把热点 working set 全部挤出去（缓存击穿 / 缓存污染）**。
传统 Linux（5.18 之前）用 **active/inactive 双 LRU（双队列 Clock 近似）**；新版主流（6.1+）换成 **Multi‑Gen LRU（多代 LRU）**，再叠加一系列辅助机制一起解决这个流式冲刷问题。

> 
> 对比 xv6：xv6 bcache = 单链表 LRU，新 buffer 进来直接进 MRU；Linux 不让一次性访问的流式页轻易进入受保护的热点区。

## 1. 经典方案：active /inactive 双链表（最重要，面试必考）

Linux 把 page cache 文件页分成两条链表：

- **inactive（ probation 缓刑队列，回收候选区）**：**所有新读到的页，先进 inactive，不直接进 active**
- **active（热点保护队列）**：真正的热页，**默认不会直接从这里回收**

晋升规则：

> 
> 一个页**第二次被访问**，才从 inactive 提升到 active；只访问一次的流式页，永远停在 inactive。

回收逻辑：

- kswapd 回收**只优先扫 inactive 的尾部**
- 顺序读大文件产生的大量一次性页面，全部堆积在 inactive；它们只访问一次，永远不晋升 active，满了直接从 inactive 尾部释放，**碰不到 active 里的老热点**

这就根治了最经典的 “顺序扫描冲刷热点”。

- 当 inactive 比例太低，才会把 active 尾部一批页降级回 inactive，再去扫描；active 不是永久保险箱，只是**高门槛保护**。
- 而且 Linux 是 4 条 LRU：`active_anon / inactive_anon / active_file / inactive_file`，文件页和匿名页分开，防止进程堆内存把文件缓存热点冲掉。

> 
> 本质：给热点加一道 “二次访问” 的门槛，模拟 2Q 算法的思想。

## 2. 新一代：Multi‑Gen LRU（多代 LRU，内核 6.1 正式合入，现在主流）

双 LRU 依然有短板：只有 “冷 / 热” 二值，无法区分**高频热、中频、刚热起来、变冷的页**，refault（刚淘汰马上又缺页）依然会损失性能。

Multi‑Gen LRU 不再是两条链表，而是按**世代（generation）**给页面打年龄标签：

- 新页 = 新生代；长期没访问 = 老代
- 访问页 → 升级到最新一代；老化时世代序号递增，老代优先被回收
- 它用**缺页重试反馈（refault feedback，PID 控制器）**自动调节：如果刚回收的页大量马上又缺页，说明回收错了，提高这一类页面的保护优先级；如果是流式一次性页，refault 率极低，内核就放心大量回收它Linux Kern...
- 不再依赖单纯的 “第二次访问” 硬门槛，而是**基于访问频率 + 缺页反馈的自适应冷热分级**，对混合 workload（随机热点 + 穿插大顺序扫描）效果远好于老双 LRU

## 3. 配套的辅助机制（一起防御 LRU 击穿）

### （1）顺序访问识别 + 预读控制（readahead）

内核会自动检测 `VM_SEQ_READ`：识别这个文件是顺序读。

- 顺序读场景：预读窗口变大，但**不长期缓存这些页**；
- 应用也可以主动给内核提示：`madvise(MADV_SEQUENTIAL)`，告诉内核：这一段是顺序扫描，读完就可以尽快释放，不要占用缓存；`MADV_RANDOM` 则关掉激进预读Ubuntu Man...。

### （2）memcg 隔离（cgroup 内存分组）

不同业务 / 容器各自拥有独立的 LRU 链表。

> 
> 一个容器里跑一个扫 TB 日志的流式任务，只会污染自己组的 inactive 链表，**不会冲掉别的 cgroup 里数据库、redis 的热点缓存**。这是生产环境非常关键的隔离手段。

### （3）回收偏好：优先回收干净文件页

干净的 page cache 页不需要写盘，回收代价极低；脏页、匿名页（进程内存）回收代价高，内核默认优先淘汰干净的、inactive 的文件页。
`vm.swappiness` 控制匿名页 vs 文件页回收倾向，不是直接防冲刷，但可以减轻连锁伤害。

### （4）workingset 检测（工作集追踪）

内核估算当前真正的工作集大小；如果内存 > working set，热点天然稳；当内存压力上来、working set 装不下时，才开始认真回收，避免过早回收热点。

### （5）DAMON（可选）

DAMON 是内核的访问采样模块，可以主动识别高频热区，配合 `DAMON_LRU_SORT` 主动保护高频页，适合复杂、长尾负载，属于增强方案，不是默认主路径Linux Kern...。

## 4. 边界：Linux 也不是万能的

如果**工作集本身已经大于可用内存**，不管是双 LRU 还是 Multi‑Gen LRU，依然会不断淘汰热点，这是物理上限，算法解决不了；
算法解决的是：**当内存足够放下热点工作集，只是被一次性顺序 IO 的垃圾页挤占时，保护热点不被无辜驱逐**—— 也就是你问的 workload 击穿 LRU 场景。

## 5. 和 xv6 bcache 的对照总结（方便你串起 6.S081）

- xv6 bcache：单 LRU 链表，新块进来直接进 MRU，没有门槛；顺序扫描直接冲刷热点 → 极易击穿
- Linux 旧：active/inactive 双队列 Clock，二次访问门槛，流式页锁死在 inactive
- Linux 新：Multi‑Gen LRU，多世代 + refault 反馈自适应冷热


2026年10月26日 18:07
Q： 在 xv-6 的 logging layer 中，Xv6 通过一种简单的日志记录机制，解决了文件系统操作期间发生崩溃的问题。

A：xv6 的日志层是**写前日志（WAL, Write-Ahead Logging）**，目标：**把一个系统调用对应的一堆磁盘修改打包成一个事务，做到原子性：要么全部写到磁盘生效，要么全部等于没发生，不会卡在中间破损状态**，以此解决操作中途崩溃导致文件系统元数据撕裂（inode、空闲位图、目录不一致）的问题。

Q：那 linux 是这样写 fs？
A：Linux 文件系统不是 xv6 这种极简 WAL 日志方案，但思想同源（WAL）；现代 Linux 主流文件系统（ext4、xfs、btrfs）做了大量取舍、分层，和 xv6 log layer 差别非常大。

---

## 6. xv6 Lab：给 bmap() 增加双重间接块（doubly-indirect block）

2026年10月30日

### 6.1 这个练习要解决什么

xv6 原来的 inode 只有 **直接块 + 一级间接块**：

- `addrs[0..11]`：12 个直接块
- `addrs[12]`：一级间接块，指向 256 个数据块

所以原最大文件是：

```
MAXFILE = NDIRECT + NINDIRECT = 12 + 1024/4 = 12 + 256 = 268 个块
```

练习要求再加一层 **双重间接块**，同时不改变磁盘 inode 的大小，因此必须把直接块从 12 个减到 11 个，把 `addrs[]` 的第 13 个槽位（下标 12）让给双重间接块：

| 下标 | 用途 |
| --- | --- |
| `addrs[0..10]` | 11 个直接块 |
| `addrs[11]` | 一级间接块（single indirect） |
| `addrs[12]` | 双重间接块（double indirect） |

新的最大文件大小：

```
NINDIRECT  = BSIZE / sizeof(uint) = 1024 / 4 = 256
NDINDIRECT = NINDIRECT * NINDIRECT = 256 * 256 = 65536
MAXFILE    = NDIRECT + NINDIRECT + NDINDIRECT
           = 11 + 256 + 65536
           = 65803 个块
```

`bigfile` 测试要求的正是 `wrote 65803 blocks`。

### 为什么最大文件只增加了 256*256 - 1 块？

原来 12 个直接块，现在只有 11 个，少了 1 个直接块；多出来的双重间接区是 `256*256 = 65536` 块。净增量：

```
65536 - 1 = 65535 = 256*256 - 1
```

### 6.2 三层索引结构图

```
ip->addrs[0]   ──────────────────────────────► data block
ip->addrs[1]   ──────────────────────────────► data block
...
ip->addrs[10]  ──────────────────────────────► data block

ip->addrs[11]  ──► single indirect block
                     [0]  ────────────────────► data block
                     [1]  ────────────────────► data block
                     ...
                     [255] ──────────────────► data block

ip->addrs[12]  ──► double indirect block
                     [0]  ──► single indirect block
                                [0]  ─────────► data block
                                [1]  ─────────► data block
                                ...
                                [255] ───────► data block
                     [1]  ──► single indirect block
                                [0..255] ────► data blocks
                     ...
                     [255] ─► single indirect block
                                [0..255] ────► data blocks
```

一个双重间接块本身占 1 个数据块，里面存 256 个一级间接块的块号；每个一级间接块再存 256 个数据块块号。因此双重间接区一共覆盖 `256*256` 个数据块。

### 6.3 逻辑块号 bn 如何映射

设 `bn` 是从 0 开始的逻辑块号：

```
bn < 11                      → 直接块：ip->addrs[bn]
11 <= bn < 11+256            → 一级间接：
                               x = bn - 11
                               数据块 = single[x]

267 <= bn < 65803            → 双重间接：
                               x = bn - 267
                               outer = x / 256          // 选择哪个一级间接块
                               inner = x % 256          // 一级间接块里的第几项
                               数据块 = double[outer][inner]
```

其中 `267 = NDIRECT + NINDIRECT = 11 + 256`。

`bmap()` 的写法是先逐层减去前面的区段，再在对应层里用 `/ NINDIRECT` 和 `% NINDIRECT` 取下标。关键是不能忘记 `bn -= NINDIRECT`，否则双重间接区会从错误的逻辑块号开始。

### 6.4 修改的三个文件

### （1）`kernel/fs.h`

```c
#define NDIRECT 11
#define NINDIRECT (BSIZE / sizeof(uint))
#define NDINDIRECT (NINDIRECT * NINDIRECT)
#define MAXFILE (NDIRECT + NINDIRECT + NDINDIRECT)

struct dinode {
  ...
  uint addrs[NDIRECT+2];   // 11 direct + 1 single + 1 double = 13 项
};
```

磁盘 inode 的大小必须保持不变：

- 原：`NDIRECT=12`，`addrs[13]`，13 个 uint
- 新：`NDIRECT=11`，`addrs[NDIRECT+2] = addrs[13]`，仍然是 13 个 uint

所以 `struct dinode` 仍然是 64 字节，`BSIZE % sizeof(struct dinode) == 0` 依然成立，mkfs 的 `static_assert` 不会失败。

### （2）`kernel/file.h`

内存中的 `struct inode` 必须和 `struct dinode` 的 `addrs[]` 项数一致：

```c
uint addrs[NDIRECT+2];
```

`iupdate()` 和 `ilock()` 用 `memmove(..., sizeof(ip->addrs))` 在两者之间拷贝；如果一边是 13 项、另一边是 12 项，就会越界写或漏拷，直接破坏内存。

### （3）`kernel/fs.c` 的 `bmap()`

双重间接分支的核心：

```c
bn -= NINDIRECT;                 // 跳过直接块和一级间接区

if(bn < NDINDIRECT){
  // 1. 按需分配双重间接块
  if((addr = ip->addrs[NDIRECT+1]) == 0){
    addr = balloc(ip->dev);
    if(addr == 0)
      return 0;
    ip->addrs[NDIRECT+1] = addr;
  }

  // 2. 在双重间接块里找/分配一个一级间接块
  bp = bread(ip->dev, addr);
  a = (uint*)bp->data;
  if((addr = a[bn / NINDIRECT]) == 0){
    addr = balloc(ip->dev);
    if(addr == 0){
      brelse(bp);               // 提前返回也要释放 buffer
      return 0;
    }
    a[bn / NINDIRECT] = addr;
    log_write(bp);              // 修改了元数据块，必须走日志
  }
  brelse(bp);

  // 3. 在这个一级间接块里找/分配真正的数据块
  bp = bread(ip->dev, addr);
  a = (uint*)bp->data;
  if((addr = a[bn % NINDIRECT]) == 0){
    addr = balloc(ip->dev);
    if(addr){
      a[bn % NINDIRECT] = addr;
      log_write(bp);
    }
  }
  brelse(bp);
  return addr;
}
```

注意几个细节：

- **按需分配**：只有真正访问到双重间接区时，才分配 `addrs[12]`；只有访问到某个 256 块区间时，才分配对应的二级间接块。不能一上来就把 256 个一级间接块全部分配掉。
- **`bn / NINDIRECT`** 选择双重间接块中的某一项（哪一块一级间接块）；
- **`bn % NINDIRECT`** 选择该一级间接块中的具体数据块；
- 每个 `bread()` 都必须有配对的 `brelse()`，包括分配失败提前返回的路径；
- 修改过的间接块要 `log_write(bp)`，否则崩溃恢复后元数据不一致。

### （4）`kernel/fs.c` 的 `itrunc()`

删除文件时必须把双重间接结构整棵树都释放，否则块会泄漏，最终 `balloc: out of blocks`。释放顺序是：先数据块，再一级间接块，最后双重间接块。

```c
if(ip->addrs[NDIRECT+1]){
  bp = bread(ip->dev, ip->addrs[NDIRECT+1]);
  a = (uint*)bp->data;
  for(i = 0; i < NINDIRECT; i++){
    if(a[i] == 0)
      continue;

    bp2 = bread(ip->dev, a[i]);       // 某个一级间接块
    a2 = (uint*)bp2->data;
    for(j = 0; j < NINDIRECT; j++){
      if(a2[j])
        bfree(ip->dev, a2[j]);        // 释放数据块
    }
    brelse(bp2);

    bfree(ip->dev, a[i]);             // 释放一级间接块
  }
  brelse(bp);
  bfree(ip->dev, ip->addrs[NDIRECT+1]); // 释放双重间接块
  ip->addrs[NDIRECT+1] = 0;
}
```

### 6.5 常见坑

1. **改了 `NDIRECT` 却没有重建 `fs.img`**
   旧镜像里的 `addrs[11]` 是数据块，而新内核会把它当成一级间接块来解释，文件系统直接错乱。改完 `NDIRECT` 后要在宿主机上删除 `fs.img`（不是进 xv6 删），让 `make` 用新的 `mkfs` 重新生成。

2. **只改了 `file.h` 或只改了 `fs.h`**
   两边的 `addrs[]` 项数必须一致。磁盘 inode 的大小不能变，所以正确写法是 `NDIRECT 12→11` 且 `addrs[NDIRECT+1]→addrs[NDIRECT+2]`。

3. **双重间接分支忘记 `bn -= NINDIRECT`**
   会让本应落在双重间接区的块号被当成一级间接区，索引全错，甚至越界 panic。

4. **`brelse()` 遗漏**
   尤其是 `balloc` 失败提前 `return 0` 的路径，很容易忘记先释放已经 `bread` 的块。

5. **`itrunc()` 只释放了双重间接块本身**
   必须递归释放它指向的 256 个一级间接块以及这些块里的数据块，否则会泄漏大量磁盘块。

6. **事务/日志**
   `bmap()` 修改间接块后要 `log_write()`；`itrunc()` 里的 `bfree()` 内部会更新位图并走日志。所有这些操作都要在 `begin_op()/end_op()` 事务范围内完成（上层 `writei`、`iput` 已经保证）。

### 6.6 验证

`bigfile` 会顺序写满整个文件系统允许的最大文件，然后重新读取并校验每个块的内容：

```
wrote 65803 blocks
reading bigfile
bigfile done; ok
```

本次实现已通过该测试（约 140 秒）。完整的文件系统实验还应运行：

```sh
python3 grade-lab-fs bigfile
python3 grade-lab-fs usertests
```

本次两个测试均已通过：

- `bigfile`：约 140 秒，输出 `wrote 65803 blocks` / `bigfile done; ok`
- `usertests`：约 175 秒，输出 `ALL TESTS PASSED`

`usertests -q` 会覆盖大文件读写、截断、删除后重新分配等场景，能有效检验 `itrunc()` 是否把双重间接块完整释放。如果 `itrunc()` 漏释放双重间接块或其下属的一级间接块，长时间运行后会出现 `balloc: out of blocks` 或相关 `usertests` 失败。

### 6.7 和 Linux 的对照（承接前面的笔记）

xv6 的“直接块 + 一级间接 + 双重间接”是早期 Unix（如 V7、ext2）的经典多级索引结构：

- 优点：实现简单，查找复杂度固定为 O(1)～O(3)，元数据紧凑；
- 缺点：固定层级限制了最大文件大小；大文件会产生大量间接块，随机访问要多次读盘；删除/截断需要遍历整棵索引树；对超大文件和碎片化不友好。

现代 Linux 文件系统（ext4、xfs、btrfs）改用 **extent（区段）** 或 **B 树** 来记录“逻辑块区间 → 物理块区间”的映射，不再为每个数据块保存一个指针，因此支持更大的文件、更少的元数据开销和更好的空间局部性。但 xv6 这套多级索引仍然是理解“逻辑块号如何映射到磁盘块”的最好起点，也是文件系统课程实验的核心。