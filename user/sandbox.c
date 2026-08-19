#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

int
main(int argc, char *argv[])
{
    if(argc < 4){
        fprintf(2, "usage: sandbox mask path command args...\n");
        exit(1);
    }

    int mask = atoi(argv[1]);
    char *path = argv[2];

    interpose(mask, path);

    exec(argv[3], &argv[3]);
    fprintf(2, "sandbox: exec failed\n");
    exit(1);
}
/*
提示：一些提示：

在 Makefile 中将 $U/_sandbox 添加到 UPROGS 中。

运行 make qemu，你会看到编译器无法编译 user/sandbox.c，因为拦截系统调用的用户空间 stub 还不存在：
需要在 user/user.h 中添加 interpose 的原型，在 user/usys.pl 中添加一个 stub，
并在 kernel/syscall.h 中添加一个系统调用号。Makefile 会调用 perl 脚本 user/usys.pl，
该脚本生成 user/usys.S，即实际的系统调用 stub，使用 RISC-V ecall 指令跳转到内核。
一旦解决了编译问题，就在 xv6 shell 中运行 sandbox 32768 - cat README；它会失败，因为你尚未在内核中实现该系统调用。

在 kernel/sysproc.c 中添加一个 sys_interpose() 函数，
通过将 mask 参数记录到 proc 结构的新字段中来实现新的系统调用（参见 kernel/proc.h）。
从用户空间获取系统调用参数的函数位于 kernel/syscall.c，你可以在 kernel/sysproc.c 中看到它们的使用示例。
将你的新 sys_interpose 函数添加到 kernel/syscall.c 中的 syscalls 数组中。

修改 kfork()（参见 kernel/proc.c），使其将父进程的 mask 传递给子进程。
修改 kernel/syscall.c 中的 syscall() 函数，以检查系统调用是否应被拒绝。

操作：
全部完成，编译通过。以下是所有修改的总结：
修改的文件
1. kernel/proc.h - 在 struct proc 中添加 uint64 mask 字段
2. kernel/sysproc.c - 实现 sys_interpose()：从用户空间获取 mask 参数并存入进程结构
3. kernel/proc.c - 两处修改：
    - kfork() 中将父进程的 mask 传递给子进程（np->mask = p->mask）
    - freeproc() 中重置 p->mask = 0
4. kernel/syscall.c - 两处修改：
    - 添加 extern uint64 sys_interpose(void) 声明
    - 在 syscall() 中检查：若当前系统调用被 mask 拦截（且不是 interpose 本身），返回 -1
5. user/sandbox.c - 改写为完整的用户程序：解析参数，调用 interpose(mask, path)，然后 exec 命令
6. user/usys.pl - 移除无效的 entry("stub")（SYS_stub 未定义）
7. Makefile - 将 $U/_sandbox 添加到 UPROGS
使用方法
在 xv6 shell 中运行：
$ sandbox 32768 - cat README
32768 = 1 << 15 = 1 << SYS_open，会阻止 cat 使用 open 系统调用。

新增功能：

编译通过。修改总结：
变更内容
1. kernel/proc.h — struct proc 新增 char allowed_path[MAXPATH] 字段
2. kernel/sysproc.c — sys_interpose() 用 argstr() 获取第二个参数存入 p->allowed_path
3. kernel/syscall.c — syscall() 中对被拦截的 open/exec 检查路径是否匹配 allowed_path，匹配则放行
4. kernel/proc.c — kfork() 复制 allowed_path 给子进程；freeproc() 清零
行为
- sandbox 32768 README cat README → open("README") 匹配允许路径，放行
- sandbox 32768 README cat Makefile → open("Makefile") 不匹配，拦截
- - 作为路径时 allowed_path[0] 为 -，不会匹配任何实际路径，等同于全部拦截
*/