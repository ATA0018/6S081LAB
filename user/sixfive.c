#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"
#include "kernel/fcntl.h"

int is_separator(char c) {
    // 使用 strchr 检查字符是否在分隔符字符串中
    char *separators = " -\r\t\n.,";
    return strchr(separators, c) != 0;
}

int is_multiple_of_5_or_6(int num) {
    return num % 5 == 0 || num % 6 == 0;
}

int main(int argc, char *argv[]) {
    int fd; // 文件描述符
    char c; // 当前字符
    int num = 0; // 当前数字
    int in_number = 0; // 当前字符是否为数字
    int i; // 循环变量
    
    // 检查参数数量：这里的意思是：参数数量必须大于等于2，因为argv[0]是程序名，argv[1]是第一个文件名，所以至少需要一个文件名作为参数
    if (argc < 2) {
        fprintf(2, "Usage: sixfive <files...>\n");
        exit(1);
    }
    
    // 遍历参数
    for (i = 1; i < argc; i++) {
        fd = open(argv[i], O_RDONLY); // 打开文件 成功为0 失败为-1
        if (fd < 0) {
            fprintf(2, "sixfive: cannot open %s\n", argv[i]);
            continue;
        }
        
        // 解释：当前循环体中，读取数字后，再读取一个数字验证，就需要重置数字状态
        num = 0; // 重置数字
        in_number = 0; // 重置数字状态
        
        while (read(fd, &c, 1) > 0) {
            if (c >= '0' && c <= '9') {
                num = num * 10 + (c - '0');
                in_number = 1;
            } else if (is_separator(c)) { // 如果是分隔符，触发打印
                if (in_number) {
                    if (is_multiple_of_5_or_6(num)) {
                        printf("%d\n", num);
                    }
                    num = 0;
                    in_number = 0;
                }
            }
            // 其他字符忽略
        }
        
        // 文件结束处理：如果文件以数字结尾，需要处理 案例：“15 20 25”
        if (in_number) {
            if (is_multiple_of_5_or_6(num)) {
                printf("%d\n", num);
            }
        }
        
        close(fd);
    }
    
    exit(0);
}

// int my_main(int argc, char *argv[]) { 
//     int fd; // 文件描述符
//     char c; // 当前字符
//     int num = 0; // 当前数字
//     int in_number = 0; // 当前字符是否为数字
//     int i; // 循环变量

//     if (argc < 2) {
//         fprintf(2, "Usage: sixfive <files...>\n");
//         exit(1);
//     }

//     for (i = 1; i < argc; i++) {
//         fd = open(argv[i], O_RDONLY);
//         if (fd < 0) {
//             fprintf(2, "sixfive: cannot open %s\n", argv[i]);
//             continue;
//         }
//         num = 0;
//         in_number = 0;

//         while (read(fd, &c, 1) > 0) {
//             if (c >= '0' && c <= '9'){
//                 num = num * 10 + (c - '0');
//                 in_number = 1;
//             }
//             else if (is_separator(c)) {
//                 if (in_number) {
//                     if (is_multiple_of_5_or_6(num)) {
//                         printf("%d\n", num);
//                     }
//                     num = 0;
//                     in_number = 0;
//                 }
//             }
//         }
//         // 文件结束处理：如果文件以数字结尾，需要处理 案例：“15 20 25”
//         if (in_number) {
//             if (is_multiple_of_5_or_6(num)) {
//                 printf("%d\n", num);
//             }
//         }
//         close(fd);
//     }
//     exit(0);
// }