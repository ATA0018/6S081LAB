#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

// 自己实现的 atoi，支持负数
int my_atoi(const char *s)
{
    int n = 0;
    int sign = 1;
    
    // 处理负号 - 的 ascii 码是 45
    if (*s == 45) {
        sign = -1;
        s++;
    }
    
    // 解析数字
    while ('0' <= *s && *s <= '9') {
        n = n * 10 + *s++ - '0';
    }
    
    return n * sign;
}

int
main(int argc, char *argv[])
{
    int ticks;

    if (argc != 2) {
        fprintf(2, "Usage: sleep <ticks>\n");
        exit(1);
    }

    ticks = my_atoi(argv[1]);
    
    if (ticks < 0) {
        fprintf(2, "sleep: ticks must be non-negative\n");
        exit(1);
    }

    pause(ticks);
    exit(0);
}

/*
// Shell 内部会这样做：
char *argv[] = {
    "sleep",   // argv[0] - 程序名
    "10",      // argv[1] - 第一个参数 所以我们一般选择argv[1]作为第一个参数
    "abc",     // argv[2] - 第二个参数
    "-x",      // argv[3] - 第三个参数
    NULL       // 必须以 NULL 结尾
};
int argc = 4;  // 参数个数
*/