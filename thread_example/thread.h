#ifndef THREAD_H
#define THREAD_H

#define barrier() __asm__ __volatile__ ("" : : : "memory")

void spin_lock(int *lock);
int try_lock(int *lock);
void unlock(int *lock);

void spin_wait_for_0(unsigned long *x);

void locked_inc(unsigned long *x);
void locked_dec(unsigned long *x);

#endif