#include <cpuid.h>
#include <stdio.h>

int main(void) {
  unsigned a,b,c,d;

  __get_cpuid(0x8000000A,&a,&b,&c,&d);

  printf("rev=%u ASIDs=%u edx=0x%08x  NP=%d NRIPS=%d nRIP-save=%d\n",
         a&0xff,b,d,!!(d&1),!!(d&8),!!(d&8));

  return 0;
}
