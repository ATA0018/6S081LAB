# GitLab：将官方的Lab整合到自己的Github 仓库中（学习记录）


## 情况：你其实想要 2025 版的 traps（更可能是这个）

那现在的 `traps` 分支不能用，需要先换成 2025 版官方仓库，再重建。

**1. 先处理 pgtbl 上的未提交改动**（同上，`git status` 确认，有就 commit 或 stash）。

**2. 删掉 2021 版的 upstream，换 2025 版：**

```bash
git remote remove upstream
git remote add upstream git://g.csail.mit.edu/xv6-labs-2025

git fetch upstream
From git://g.csail.mit.edu/xv6-labs-2025
 * [new branch]      cow        -> upstream/cow
 * [new branch]      fs         -> upstream/fs
 * [new branch]      lock       -> upstream/lock
 * [new branch]      mmap       -> upstream/mmap
 * [new branch]      net        -> upstream/net
 * [new branch]      pgtbl      -> upstream/pgtbl
 * [new branch]      riscv      -> upstream/riscv
 * [new branch]      syscall    -> upstream/syscall
 * [new branch]      traps      -> upstream/traps
 * [new branch]      util       -> upstream/util

git branch -r | grep upstream
  upstream/HEAD -> upstream/util
  upstream/cow
  upstream/fs
  upstream/lock
  upstream/mmap
  upstream/net
  upstream/pgtbl
  upstream/riscv
  upstream/syscall
  upstream/traps
  upstream/util

```

**3. 看 2025 版有没有 traps 分支：**

```bash
git branch -r | grep traps
```

- 如果有 `upstream/traps`，重建本地 traps：

```bash
git checkout pgtbl # 切回 pgtbl 分支
git branch -D traps  # 删掉本地 traps 分支
git checkout -b traps upstream/traps  # 从 2025 版的 traps 分支新建本地 traps 分支

git remote -v # 确认一下当前有哪些远程
6S081LAB        https://github.com/ATA0018/6S081LAB.git (fetch)
6S081LAB        https://github.com/ATA0018/6S081LAB.git (push)
mit     https://github.com/mit-pdos/xv6-riscv.git (fetch)
mit     https://github.com/mit-pdos/xv6-riscv.git (push)
upstream        git://g.csail.mit.edu/xv6-labs-2025 (fetch)
upstream        git://g.csail.mit.edu/xv6-labs-2025 (push)


git push -u 6S081LAB traps # 推送到 6S081LAB
```
