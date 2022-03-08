#ifndef __LIB_USER_SYSCALL_H
#define __LIB_USER_SYSCALL_H
#include "stdint.h"
enum SYSCALL_NR {   // 用来存放子功能号
    SYS_GETPID
};
uint32_t getpid(void);
#endif // !__LIB_USER_SYSCALL_H