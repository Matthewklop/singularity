#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>

static int m; static void i(void){m=open("/dev/cpu/0/msr",O_RDONLY);}
static uint64_t r(uint32_t a){uint64_t v;pread(m,&v,8,a);return v;}

int main(){ i(); volatile uint64_t s=0;
 for(int c=0;c<100000;c++){
  s += c * 7;
        s += c & 0xFF;
        s += (c / 32) & 0xFF;
        s += (c / 32) & 0xFF;
  asm volatile("": : :"memory");
 } printf("done\n"); return (int)(s&0xFF);}
