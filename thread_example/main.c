#define _GNU_SOURCE

#include "rings.h"
#include "thread.h"

#include <unistd.h>
#include <sched.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ENTRIES 32768
#define THREAD_COUNT 4
#define STACK_SIZE (1024 * 1024 * 8)

struct rings rings;
struct iovec *iovecs;
unsigned long total_submitted;
unsigned long total_processed;
unsigned long workers_active;
int fd;

int do_work(void *thread_id_ptr)
{
  unsigned int thread_id = (unsigned long)thread_id_ptr;
  struct io_uring_sqe sqe;
  struct io_uring_cqe cqe;
  memset(&sqe, 0, sizeof sqe);

  for (int i = 0; i < 10; ++i) {
    for (unsigned int i = thread_id; i < *rings.sq.entries; i += THREAD_COUNT) {
      sqe.opcode = IORING_OP_WRITEV;
      sqe.flags = 0;
      sqe.ioprio = 0;
      sqe.fd = fd;
      sqe.off = i * 4096;
      sqe.addr = (unsigned long)(iovecs + i);
      sqe.len = 1;
      sqe.rw_flags = 0;
      sqe.user_data = i;

      spin_lock(&rings.sq_lock);
      while (rings_submit(&rings, &sqe) == -1) {
        // wait for pending ios to be processed, process results meanwhile
        unlock(&rings.sq_lock); // sq_lock is not required for cq_lock
        spin_lock(&rings.cq_lock); 
        if (rings_reap(&rings, &cqe, 1)) {
          if (cqe.res != 4096)
            fprintf(stderr, "ERROR for part %u\n", cqe.user_data);
          else
            locked_inc(&total_processed);
        }
        unlock(&rings.cq_lock);
        spin_lock(&rings.sq_lock);
      }
      unlock(&rings.sq_lock);
      locked_inc(&total_submitted);
    }

    while (total_processed != total_submitted) {
      spin_lock(&rings.cq_lock);
      if (rings_reap(&rings, &cqe, 1)) {
        if (cqe.res != 4096)
          fprintf(stderr, "ERROR for part %u\n", cqe.user_data);
        else
          locked_inc(&total_processed);
      }
      unlock(&rings.cq_lock);
    }
  }

  locked_dec(&workers_active);

  return 0;
}

int main(int argc, char *argv[])
{
  if (rings_setup(&rings, ENTRIES) == -1)
    return EXIT_FAILURE;
  
  fd = open("test_file", O_DIRECT|O_CREAT|O_RDWR|O_DSYNC, 0600);
  if (fd == -1) {
    perror("open()");
    return EXIT_FAILURE;
  }

  // allocate memory and align to page size
  iovecs = malloc(sizeof(struct iovec) * *rings.sq.entries);
  if (!iovecs) {
    perror("malloc()");
    return EXIT_FAILURE;
  }

  // io buffer
  unsigned char *buffer = malloc(*rings.sq.entries * 4096 + 4095);
  if (!buffer) {
    perror("malloc()");
    return EXIT_FAILURE;
  }

  // align to page size
  unsigned long buffer_mem = (unsigned long)buffer;
  buffer_mem = (buffer_mem + 4095) & ~4095;
  buffer = (unsigned char*)buffer_mem;

  // setup iovecs
  for (unsigned int i = 0; i < *rings.sq.entries; ++i) {
    iovecs[i].iov_base = buffer + i * 4096;
    iovecs[i].iov_len = 4096;
  }

  // spawn threads
  int clone_flags = CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SYSVSEM
    | CLONE_SIGHAND | CLONE_THREAD;
  for (unsigned long i = 0; i < THREAD_COUNT; ++i) {
    char *stack = malloc(STACK_SIZE);
    if (!stack) {
      perror("malloc()");
      return EXIT_FAILURE;
    }
    if (clone(do_work, stack + STACK_SIZE, clone_flags, (void*)i) == -1) {
      perror("clone()");
      return EXIT_FAILURE;
    }
    locked_inc(&workers_active);
  }

  // wait until all threads have completed
  spin_wait_for_0(&workers_active);

  puts("-------------------");
  printf("processed %d submissions\n", total_processed);
  
  return 0;
}