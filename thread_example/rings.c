#define _GNU_SOURCE

#include "rings.h"
#include <sys/syscall.h>
#include <sys/mman.h>
#include <unistd.h>
#include <stdio.h>

#define barrier() __asm__ __volatile__ ("" : : : "memory")

int rings_setup(struct rings *rings, int entries) {
  struct io_uring_params p = {
    .flags=IORING_SETUP_SQPOLL,
    .sq_thread_cpu=0,
    .sq_thread_idle=1000 * 60 // spin for up to 1 minute when ring is empty
  };
  int uring_fd = syscall(__NR_io_uring_setup, entries, &p);
  if (uring_fd == -1) {
    perror("io_uring_setup()");
    goto err;
  }

  size_t sq_map_size = p.sq_off.array + p.sq_entries * sizeof(__u32);
  void *sq_addr = mmap(
    NULL, sq_map_size, PROT_READ|PROT_WRITE, MAP_SHARED|MAP_POPULATE, uring_fd,
    IORING_OFF_SQ_RING
  );
  if (sq_addr == MAP_FAILED) {
    perror("mmap()");
    goto err1;
  }

  size_t cq_map_size = 
    p.cq_off.cqes + p.cq_entries * sizeof(struct io_uring_cqe);
  void *cq_addr = mmap(
    NULL, cq_map_size, PROT_READ|PROT_WRITE, MAP_SHARED|MAP_POPULATE, uring_fd,
    IORING_OFF_CQ_RING
  );
  if (cq_addr == MAP_FAILED) {
    perror("mmap()");
    goto err2;
  }

  rings->sq.head = (unsigned int*)((char*)sq_addr + p.sq_off.head);
  rings->sq.tail = (unsigned int*)((char*)sq_addr + p.sq_off.tail);
  rings->sq.mask = *(unsigned int*)((char*)sq_addr + p.sq_off.ring_mask);
  rings->sq.entries = (unsigned int*)((char*)sq_addr + p.sq_off.ring_entries);
  rings->sq.flags = (unsigned int*)((char*)sq_addr + p.sq_off.flags);
  rings->sq.dropped = (unsigned int*)((char*)sq_addr + p.sq_off.dropped);
  rings->sq.array = (unsigned int*)((char*)sq_addr + p.sq_off.array);
  rings->sq.fd = uring_fd;
  rings->cq.head = (unsigned int*)((char*)cq_addr + p.cq_off.head);
  rings->cq.tail = (unsigned int*)((char*)cq_addr + p.cq_off.tail);
  rings->cq.mask = *(unsigned int*)((char*)cq_addr + p.cq_off.ring_mask);
  rings->cq.entries = (unsigned int*)((char*)cq_addr + p.cq_off.ring_entries);
  rings->cq.overflow = (unsigned int*)((char*)cq_addr + p.cq_off.overflow);
  rings->cq.cqes = (struct io_uring_cqe*)((char*)cq_addr + p.cq_off.cqes);
  rings->cq.flags = (unsigned int*)((char*)cq_addr + p.cq_off.flags);
  rings->cq.fd = uring_fd;

  size_t sqes_map_size = sizeof(struct io_uring_sqe) * p.sq_entries;
  rings->sq.sqes = mmap(
    NULL, sqes_map_size, PROT_READ|PROT_WRITE, MAP_SHARED|MAP_POPULATE,
    uring_fd, IORING_OFF_SQES
  );
  if (rings->sq.sqes == MAP_FAILED) {
    perror("mmap()");
    goto err3;
  }

  rings->sq_lock = 0;
  rings->cq_lock = 0;

  return 0;

err3:
  munmap(cq_addr, cq_map_size);
err2:
  munmap(sq_addr, sq_map_size);
err1:
  close(uring_fd);
err:
  return -1;
}

// return 0 on success and -1 on failure
int rings_submit(struct rings *rings, struct io_uring_sqe *sqe)
{
  unsigned int tail = *rings->sq.tail;
  unsigned int idx = tail & rings->sq.mask;
  unsigned int next_idx = (idx + 1) & rings->sq.mask;
  if (next_idx == *rings->sq.head)
    return -1;
  
  struct io_uring_sqe *dest = rings->sq.sqes + idx;
  *dest = *sqe;

  rings->sq.array[idx] = idx;
  ++tail;

  barrier();
  *rings->sq.tail = tail;
  barrier();

  if (*rings->sq.flags & IORING_SQ_NEED_WAKEUP)
    syscall(
      __NR_io_uring_enter, rings->sq.fd, 0, 0, IORING_ENTER_SQ_WAKEUP, NULL
    );

  return 0;
}

// return the number of items reaped
int rings_reap(struct rings *rings, struct io_uring_cqe *cqes, int max_count)
{
  barrier();
  unsigned int n = 0;
  unsigned int head = *rings->cq.head;
  while (n != max_count && head != *rings->cq.tail) {
    unsigned int idx = head & rings->cq.mask;
    cqes[n] = rings->cq.cqes[idx];

    ++n;
    ++head;
    barrier();
  }
  *rings->cq.head = head;
  barrier();
  return n;
}