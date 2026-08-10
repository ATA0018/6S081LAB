#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"
#include "kernel/fs.h"

char *exec_args[16];
int exec_argc = 0;

void run_exec(char *path) {
    // 1. 没有 -exec 参数时，直接打印文件路径
    if (exec_argc == 0) {
        printf("%s\n", path);
        return;
    }
    
    // 2. 构建参数数组：命令参数 + 文件路径
    char *args[16];
    int i;
    for (i = 0; i < exec_argc; i++) {
        args[i] = exec_args[i]; // 复制命令和参数
    }
    args[i] = path; // 追加文件路径作为最后一个参数
    args[i + 1] = 0; // NULL 结尾
    
    // 3. 创建子进程执行命令
    if (fork() == 0) {
        exec(args[0], args); // 子进程替换为新程序  args[0]=要执行的程序 echo；args	程序的参数数组（NULL结尾）	{"echo", "/tmp/test", 0}
        fprintf(2, "find: exec %s failed\n", args[0]); // 错误
        exit(1);
    }
    wait(0); // 父进程等待子进程结束
}

void find(char *path, char *target) {
    char buf[512], *p;
    int fd;
    struct dirent de;
    struct stat st;

    if ((fd = open(path, 0)) < 0) {
        fprintf(2, "find: cannot open %s\n", path);
        return;
    }

    if (fstat(fd, &st) < 0) {
        fprintf(2, "find: cannot stat %s\n", path);
        close(fd);
        return;
    }

    switch (st.type) {
    case T_FILE:
        p = path + strlen(path);
        while (p >= path && *p != '/')
            p--;
        p++;
        
        if (strcmp(p, target) == 0) {
            run_exec(path);
        }
        break;
 
    case T_DIR:
        // 检查路径长度是否超过缓冲区大小，防止缓冲区溢出
        if (strlen(path) + 1 + DIRSIZ + 1 > sizeof buf) {
            printf("find: path too long\n");
            break;
        }
        // 2. 构建子路径前缀: "父路径/"
        strcpy(buf, path);
        p = buf + strlen(buf); // 等价于 p = buf[strlen(buf)]，指向 buf 的末尾
        *p++ = '/'; // 解引父路径用/分隔
        
        // 3. 读取目录项，递归搜索
        while (read(fd, &de, sizeof(de)) == sizeof(de)) {
            if (de.inum == 0) // 跳过空目录项
                continue;
            
            memmove(p, de.name, DIRSIZ);  // 拼接文件名
            p[DIRSIZ] = 0; // 字符串结尾
            
            if (strcmp(p, ".") == 0 || strcmp(p, "..") == 0) // 跳过 . 和 ..
                continue;
            
            find(buf, target); // 递归搜索子目录/文件
        }
        break;
    }
    close(fd);
}
/*
find("/usr", "test")
│
├── 打开 /usr 目录
├── 读取目录项:
│   ├── "bin"  → find("/usr/bin", "test")
│   ├── "lib"  → find("/usr/lib", "test")
│   ├── "."    → 跳过
│   └── ".."   → 跳过
└── 关闭目录
*/
int main(int argc, char *argv[]) {
    if (argc < 3) {
        fprintf(2, "Usage: find <path> <filename> [-exec command args...]\n");
        exit(1);
    }

    int i;
    for (i = 3; i < argc; i++) {
        if (strcmp(argv[i], "-exec") == 0) {
            i++; // 跳过 -exec
            break;
        }
    }
    
    for (; i < argc; i++) { // i = 4 读取的参数
        if (strcmp(argv[i], ";") == 0)
            break;
        exec_args[exec_argc++] = argv[i];
    }

    find(argv[1], argv[2]);
    exit(0);
}
