#include "init.h"
#include "print.h"
#include "interrupt.h"
#include "timer.h"
#include "memory.h"
#include "thread.h"

/* 负责初始化所有模块 */
void init_all(){
	put_str("init_all\n");
	idt_init();		// 初始化 中断
	mem_init();		// 初始化内存池
	thread_init();	// 初始化线程
	timer_init();		// 初始化 PIT
}

