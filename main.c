#define _GNU_SOURCE

#include <linux/io_uring.h>
#include <sys/syscall.h>
#include <sys/mman.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define barrier() __asm__ __volatile__ ("" : : : "memory")

#define ENTRIES 32768

struct sq_ring {
  unsigned int *head;
  unsigned int *tail;
  unsigned int *entries;
  unsigned int *flags;
  unsigned int *dropped;
  unsigned int *array;
  struct io_uring_sqe *sqes;
  unsigned int mask;
  int fd;
};

struct cq_ring {
  unsigned int *head;
  unsigned int *tail;
  unsigned int *entries;
  unsigned int *overflow;
  unsigned int *flags;
  struct io_uring_cqe *cqes;
  unsigned int mask;
  int fd;
};

struct rings {
  struct sq_ring sq;
  struct cq_ring cq;
  int sq_lock;
  int cq_lock;
};

int setup_rings(struct rings *rings) {
  struct io_uring_params p = {
    .flags=IORING_SETUP_SQPOLL,
    .sq_thread_cpu=0,
    .sq_thread_idle=1000 * 60 // spin for up to 1 minute when ring is empty
  };
  int uring_fd = syscall(__NR_io_uring_setup, ENTRIES, &p);
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
  printf("sizeof(struct rings) = %zu\n", sizeof(struct rings));

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
int submit(struct rings *rings, struct io_uring_sqe *sqe)
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
int reap(struct rings *rings, struct io_uring_cqe *cqes, int max_count)
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

int main(int argc, char *argv[])
{
  struct rings rings;

  if (setup_rings(&rings) == -1)
    return EXIT_FAILURE;
  
  // The code below doesn't do any error checking
  int fd = open("test_file", O_DIRECT|O_CREAT|O_RDWR|O_DSYNC, 0600);

  // allocate memory and align to page size
  struct iovec *iovecs = malloc(sizeof(struct iovec) * *rings.sq.entries);
  unsigned char *buffer = malloc(*rings.sq.entries * 4096 + 4095);
  unsigned long buffer_mem = (unsigned long)buffer;
  buffer_mem = (buffer_mem + 4095) & ~4095;
  buffer = (unsigned char*)buffer_mem;

  for (unsigned int i = 0; i < *rings.sq.entries; ++i) {
    iovecs[i].iov_base = buffer + i * 4096;
    iovecs[i].iov_len = 4096;
  }

  int total_processed = 0;
  for (int i = 0; i < 10; ++i) {
    for (unsigned int i = 0; i < *rings.sq.entries; ++i) {
      struct io_uring_sqe sqe;
      memset(&sqe, 0, sizeof sqe);
      sqe.opcode = IORING_OP_WRITEV;
      sqe.flags = 0;
      sqe.ioprio = 0;
      sqe.fd = fd;
      sqe.off = i * 4096;
      sqe.addr = (unsigned long)(iovecs + i);
      sqe.len = 1;
      sqe.rw_flags = 0;
      sqe.user_data = i;
      while (submit(&rings, &sqe) == -1) {
        // wait for pending ios to be processed
      }
    }

    struct io_uring_cqe cqe;
    for (unsigned int i = 0; i < *rings.sq.entries; ++i) {
      while (reap(&rings, &cqe, 1) == 0) {
      }
      ++total_processed;
      puts("-------------------");
      printf("user data = %lu\n", cqe.user_data);
      printf("res = %d\n", cqe.res);
    }

    if (reap(&rings, &cqe, 1)) {
      fputs("ERROR: reaped empty ring", stderr);
    }
  }

  puts("-------------------");
  printf("processed %d submissions\n", total_processed);
  
  return 0;
}