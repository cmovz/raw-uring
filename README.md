# raw-uring
Shows how to use the raw uring interface to submit IO without any syscalls.

You can compile the example code using:
```
gcc -o test -std=c99 -O3 -flto -fno-strict-aliasing *.c
```
**-fno-strict-aliasing is important because the code violates strict aliasing.** 

![](https://github.com/cmovz/raw-uring/blob/main/raw-uring.png?raw=true)
