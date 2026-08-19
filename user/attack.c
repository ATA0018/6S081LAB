#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

#define PGSIZE 4096

int
main(int argc, char *argv[])
{
  char *p;
  char secret[128];
  int i, j;
  volatile int x;

  p = (char*)((uint64)&x & ~(PGSIZE - 1));
  p -= PGSIZE;

  for(i = 0; i < PGSIZE * 2; i++) {
    if((p[i] >= '0' && p[i] <= '9') || 
       (p[i] >= 'a' && p[i] <= 'z') ||
       (p[i] >= 'A' && p[i] <= 'Z')) {
      j = 0;
      while(j < 127 && i+j < PGSIZE * 2 && 
            ((p[i+j] >= '0' && p[i+j] <= '9') ||
             (p[i+j] >= 'a' && p[i+j] <= 'z') ||
             (p[i+j] >= 'A' && p[i+j] <= 'Z'))) {
        secret[j] = p[i+j];
        j++;
      }
      if(j >= 4) {
        secret[j] = '\0';
        printf("%s\n", secret);
        exit(0);
      }
      i += j;
    }
  }

  printf("No secret found\n");
  exit(0);
}
