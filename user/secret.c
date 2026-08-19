// user/secret.c - 查看实际内容
#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

int main(int argc, char *argv[]) {
    if (argc < 2) {
        printf("Usage: secret <secret>\n");
        exit(1);
    }
    
    // 秘密存储在栈上的局部变量中
    char secret[100];
    strcpy(secret, argv[1]);
    
    printf("Secret stored, exiting...\n");
    
    // 当函数返回时，栈内存被释放，但内容保留
    exit(0);
}