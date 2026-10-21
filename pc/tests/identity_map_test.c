#include <sys/mman.h>
#include <stdio.h>
#include <string.h>
int main(void){
  void*a=mmap((void*)0x80000000,0x200000,PROT_READ|PROT_WRITE|PROT_EXEC,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE,-1,0);
  void*b=mmap((void*)0x1F800000,0x1000,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE,-1,0);
  printf("ram=%p scratch=%p sizeof(void*)=%zu\n",a,b,sizeof(void*));
  /* write a jmp rel32 at 0x80017294 to a host function and call through it */
  extern int target(int);
  unsigned char*p=(unsigned char*)0x80017294; p[0]=0xE9; int rel=(int)((char*)target-(char*)(p+5)); memcpy(p+1,&rel,4);
  int (*fp)(int)=(int(*)(int))0x80017294; printf("call via MIPS addr -> %d\n",fp(20));
  return 0;}
int target(int x){return x+22;}
