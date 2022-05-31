#ifndef __LIB_USER_SYSCALL_H
#define __LIB_USER_SYSCALL_H
#include "stdint.h"
enum SYSCALL_NR {   // 用来存放子功能号
   SYS_GETPID,
   SYS_WRITE,
   SYS_MALLOC,
   SYS_FREE
};
uint32_t getpid(void);
uint32_t _syscall3(SYS_WRITE, fd, buf, count);
void* malloc(uint32_t size);
void free(void* ptr);
#endif