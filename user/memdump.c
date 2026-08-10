#include "kernel/types.h"
#include "user/user.h"
#include "kernel/fcntl.h"

void memdump(char *fmt, char *data);
int main(int argc, char *argv[])
{
    if (argc == 1)
    {
        printf("Example 1:\n");
        int a[2] = {61810, 2025};
        memdump("ii", (char *)a);
        printf("\n\n");

        printf("Example 2:\n");
        memdump("S", "a string");
        printf("\n\n");

        printf("Example 3:\n");
        char *s = "another";
        memdump("s", (char *)&s);
        printf("\n\n");

        struct sss
        {
            char *ptr;
            int num1;
            short num2;
            char byte;
            char bytes[8];
        } example;

        example.ptr = "hello";
        example.num1 = 1819438967;
        example.num2 = 100;
        example.byte = 'z';
        strcpy(example.bytes, "xyzzy");

        printf("Example 4:\n");
        memdump("pihcS", (char *)&example);
        printf("\n\n");

        printf("Example 5:\n");
        memdump("sccccc", (char *)&example);
        printf("\n");
    }
    else if (argc == 2)
    {
        char data[512];
        int n = 0;
        memset(data, '\0', sizeof(data));
        while (n < sizeof(data))
        {
            int nn = read(0, data + n, sizeof(data) - n);
            if (nn <= 0)
                break;
            n += nn;
        }
        memdump(argv[1], data);
        printf("\n");
    }
    else
    {
        printf("Usage: memdump [format]\n");
        exit(1);
    }
    exit(0);
}

// make GRADEFLAGS=memdump grade

void print_hex_byte(unsigned char b)
{
    char hex_chars[] = "0123456789abcdef";
    printf("%c%c", hex_chars[b >> 4], hex_chars[b & 0x0F]);
}
void memdump(char *fmt, char *data)
{
    char *p = fmt;
    int offset = 0;
    int first = 1;

    while (*p != '\0')
    {
        switch (*p)
        {
        case 'i':
            if (!first) printf("\n");
            printf("%d", *(int *)(data + offset));
            offset += 4;
            first = 0;
            break;

        case 'p':
            if (!first) printf("\n");
            unsigned char *bytes = (unsigned char *)(data + offset);
            // 大端序打印（从高地址到低地址）
            for (int j = 3; j >= 0; j--) {
                print_hex_byte(bytes[j]);
            }
            offset += 8;
            first = 0;
            break;

        case 'h': // 打印 short 类型
            if (!first) printf("\n");
            printf("%d", *(short *)(data + offset));
            offset += 2;
            first = 0;
            break;

        case 'c': // 打印单个字节的字符或十六进制表示
            if (!first) printf("\n");
            unsigned char ch = *(unsigned char *)(data + offset);
            if (ch >= 32 && ch <= 126) {
                printf("%c", ch);
            } else {
                printf("\\x");
                print_hex_byte(ch);
            }
            offset += 1;
            first = 0;
            break;

        case 's': // 打印 字符串
            if (!first) printf("\n");
            char *str = *(char **)(data + offset);
            if (str == 0) {
                printf("(null)");
            } else {
                printf("%s", str);
            }
            offset += 8;
            first = 0;
            break;

        case 'S': // 输出字符串
            if (!first) printf("\n");
            printf("%s", data + offset);
            offset += strlen(data + offset) + 1;
            first = 0;
            break;

        default:
            printf("Error: unknown format character '%c'\n", *p);
            return;
        }
        p++;
    }
}