#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define MAXLINES 5000    /* max #lines to be sorted */
#define MAXLEN 1000      /* max length of any input line */

char *lineptr[MAXLINES]; /* pointers to text lines */

int readlines(char *lineptr[], int maxlines);
void writelines(char *lineptr[], int nlines);
void qsort(char *v[], int left, int right);
void swap(char *v[], int i, int j);
int getline(char *s, int lim);

/* 测试内容
zebra
apple
mango
banana
orange
pear
grape
cat
dog
ant
*/

/* sort input lines */
int main(void)
{
    int nlines; /* number of input lines read */

    if ((nlines = readlines(lineptr, MAXLINES)) >= 0) {
        qsort(lineptr, 0, nlines - 1);
        writelines(lineptr, nlines);
        return 0;
    } else {
        printf("error: input too big to sort\n");
        return 1;
    }
}

/* readlines: read input lines 准确的说：这个函数就是读取数组中有多少字符串 */
int readlines(char *lineptr[], int maxlines)
{
    int len, nlines;
    char *p, line[MAXLEN];

    nlines = 0;
    while ((len = getline(line, MAXLEN)) > 0) {
        if (nlines >= maxlines || (p = malloc(len)) == NULL) {
            return -1;
        } else {
            line[len - 1] = '\0'; /* delete newline */
            strcpy(p, line); // **复制**字符串
            lineptr[nlines++] = p;
        }
    }
    return nlines;
}
// int readmyline(char *linechar[], int maxlen){
//     int len, nlines;
//     char *p, line[MAXLEN];

//     nlines = 0;
//     while((len = getline(line,MAXLEN)) > 0) {
//         if(nlines >= maxlen || (p = malloc(len)) == NULL){
//             return -1;
//         } else {
//             line[len - 1] = '\0';
//             strcpy(p, line);
//             linechar[nlines++] = p;
//         }
//     }
//     return nlines;
// }

/* writelines: write output lines */
void writelines(char *lineptr[], int nlines)
{
    int i;
    for (i = 0; i < nlines; i++)
        printf("%s\n", lineptr[i]);
}

/* qsort: sort v[left]...v[right] into increasing order  快速排序 */
void qsort(char *v[], int left, int right)
{
    int i, last;
    if (left >= right)  /* do nothing if array contains fewer than two elements */
        return;
    swap(v, left, (left + right) / 2); // 将中间元素放到最左边，作为哨兵
    last = left;
    for (i = left + 1; i <= right; i++) {
        if (strcmp(v[i], v[left]) < 0) { // strcmp() 函数用于比较两个字符串的大小，返回值小于0表示第一个字符串小于第二个字符串
            swap(v, ++last, i);
        }
    }
    swap(v, left, last); // 哨兵划分，将中间元素放到正确的位置
    qsort(v, left, last - 1); // 递归调用，对左子区间进行快速排序
    qsort(v, last + 1, right); // 递归调用，对右子区间进行快速排序
}
/*
为什么不直接选 left 当 pivot？
选最左 / 最右当 pivot，有序数组的时候快排会退化 O (n²)。
取**中间下标元素做 pivot**是简单的优化，规避有序数组最坏情况。
但是算法逻辑要求 pivot 要放在 left 位置，于是做一步 swap 挪过去。
*/

/* swap: interchange v[i] and v[j] */
void swap(char *v[], int i, int j)
{
    char *temp;
    temp = v[i];
    v[i] = v[j];
    v[j] = temp;
}

/* getline: read a line into s, return length  K&R原版 */
int getline(char *s, int lim)
{
    int c, i;
    for (i = 0; i < lim - 1 && (c = getchar()) != EOF && c != '\n'; ++i)
        s[i] = c;
    if (c == '\n') { // 换行符
        s[i] = c;
        ++i;
    }
    s[i] = '\0'; //  \0 是结束字符
    return i;
}


// 1. **`char *lineptr[MAXLINES]` 指针数组**
// 数组每个元素是`char*`，存放每一行字符串的地址；**排序的时候只交换指针，不搬运字符串本身**，节省拷贝开销。`swap`交换的仅仅是指针变量，不是字符串内容。

// 2. `readlines()`
// - `getline()`读取一行到本地缓冲区`line[]`；
// - `malloc(len)`分配内存保存该行；
// - 把换行符`\n`替换成`\0`；
// - `strcpy(p,line)`拷贝到堆内存；
// - 将`p`存入`lineptr`指针数组。

// 3. `qsort()` 快速排序（书中手写快排，不是 stdlib 库 qsort）

// - 选中间元素作为 pivot，交换到最左边；
// - `strcmp(v[i],v[left])`做字符串字典比较；
// - 分区后递归左右子区间。

// 4. `writelines()`：遍历指针数组，打印每行。