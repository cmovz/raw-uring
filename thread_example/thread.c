#include "thread.h"


void spin_lock(int *lock)
{
  __asm__ goto
  (
    "mov $1,%%rcx\n"
    "xor %%rax,%%rax\n"
    "lock cmpxchg %%ecx,(%0)\n"
    "jz %l1"
    :
    : "r" (lock)
    : "%rcx", "%rax", "memory", "cc"
    : end
  );

l0:
  __asm__ goto
  (
    "pause\n"
    "mov $1,%%rcx\n"
    "xor %%rax,%%rax\n"
    "lock cmpxchg %%ecx,(%0)\n"
    "jnz %l1"
    :
    : "r" (lock)
    : "%rcx", "%rax", "memory", "cc"
    : l0
  );

end:
}

int try_lock(int *lock)
{
  __asm__ goto 
  (
    "mov $1,%%rcx\n"
    "xor %%rax,%%rax\n"
    "lock cmpxchg %%ecx,(%0)\n"
    "jz %l1"
    :
    : "r" (lock)
    : "%rcx", "%rax", "memory", "cc"
    : success
  );

  return -1;

success:
  return 0;
}

void unlock(int *lock)
{
  __asm__
  (
    "movl $0,(%0)"
    :
    : "r" (lock)
    : "memory"
  );
}

void spin_wait_for_0(unsigned long *x)
{
  if (*x == 0)
    return;
  
spin:
  __asm__ goto
  (
    "pause\n"
    "cmpq $0,(%0)\n"
    "jne %l1"
    :
    : "r" (x)
    : "memory", "cc"
    : spin
  );
}

void locked_inc(unsigned long *x)
{
  __asm__
  (
    "lock incq (%0)"
    :
    : "r" (x)
    : "memory", "cc"
  );
}

void locked_dec(unsigned long *x)
{
  __asm__
  (
    "lock decq (%0)"
    :
    : "r" (x)
    : "memory", "cc"
  );
}