#include "thread.h"
#include "stdint.h"
#include "string.h"
#include "global.h"
#include "memory.h"
#include "interrupt.h"
#include "debug.h"
#include "print.h"
#include "process.h"
#include "sync.h"
#include "console.h"
#include "fs.h"
#include "file.h"
#include "stdio.h"

struct task_struct* main_thread; // 主线程 PCB
struct task_struct* idle_thread;        // idle 线程
struct list thread_ready_list; // 就绪队列
struct list thread_all_list; // 所有任务队列
struct lock pid_lock;                   // 分配 pid 锁
static struct list_elem* thread_tag; // 用于保存队列中的线程结点

extern void switch_to(struct task_struct* cur, struct task_struct* next);

// pid 的位图，最大支持 1024 个 pid
uint8_t pid_bitmap_bits[128] = {0};

// pid 池
struct pid_pool {
    struct bitmap pid_bitmap;   // pid 位图
    uint32_t pid_start; // 起始 pid
    struct lock pid_lock;   // 分配 pid 锁
}pid_pool;

// 初始化 pid 池
static void pid_pool_init(void) {
    pid_pool.pid_start = 1;
    pid_pool.pid_bitmap.bits = pid_bitmap_bits;
    pid_pool.pid_bitmap.btmp_bytes_len = 128;
    bitmap_init(&pid_pool.pid_bitmap);
    lock_init(&pid_pool.pid_lock);
}

// 分配 pid
static pid_t allocate_pid(void) {
    lock_acquire(&pid_pool.pid_lock);
    int32_t bit_idx = bitmap_scan(&pid_pool.pid_bitmap, 1);
    bitmap_set(&pid_pool.pid_bitmap, bit_idx, 1);
    lock_release(&pid_pool.pid_lock);
    return (bit_idx + pid_pool.pid_start);
}

// 释放 pid
void release_pid(pid_t pid) {
    lock_acquire(&pid_pool.pid_lock);
    int32_t bit_idx = pid - pid_pool.pid_start;
    bitmap_set(&pid_pool.pid_bitmap, bit_idx, 0);
    lock_release(&pid_pool.pid_lock);
}

// 系统空闲时运行的线程
static void idle(void* arg /*UNUSED*/) {
    while (1) {
        thread_block(TASK_BLOCKED);
        // 执行 hlt 时必须要保证目前处在开中断的情况下
        asm volatile ("sti; hlt" : : : "memory");
    }
}

// 获取当前线程 PCB 指针
struct task_struct* running_thread(void) {
    uint32_t esp;
    asm("mov %%esp, %0" : "=g" (esp));
    // 取 esp 整数部分, 即 PCB 起始地址
    return (struct task_struct*)(esp & 0xfffff000);
}

// 由 kernel_thread 去执行 function(func_arg)
static void kernel_thread(thread_func* function, void* func_arg) {
    // 执行 function 前需要开中断,
    // 避免后面的时钟中断被屏蔽, 而无法调度其它线程
    intr_enable();
    //while(1);   
    function(func_arg);
}

// 初始化线程栈 thread_stack，将待执行的函数和参数放到 thread_stack 中相应的位置
void thread_create(struct task_struct* pthread, //待创建的线程指针 
                   thread_func function,        //线程函数
                   void* func_arg) {            //线程参数
    // 先预留中断使用栈的空间
    pthread->self_kstack -= sizeof(struct intr_stack);

    // 在留出线程栈空间
    pthread->self_kstack -= sizeof(struct thread_stack);
    struct thread_stack* kthread_stack = (struct thread_stack*)pthread->self_kstack;
    kthread_stack->eip = kernel_thread;
    kthread_stack->function = function;
    kthread_stack->func_arg = func_arg;
    kthread_stack->ebp = 0;
    kthread_stack->ebx = 0;
    kthread_stack->esi = 0;
    kthread_stack->edi = 0;
}

// 初始化线程基本信息
// 待初始化线程指针（PCB），线程名称，线程优先级
void init_thread(struct task_struct* pthread, char* name, int prio) {
    memset(pthread, 0, sizeof(*pthread));
    pthread->pid = allocate_pid();
    strcpy(pthread->name, name);

    if(pthread == main_thread) {
        // 把 main 函数也封装成一个线程, main 函数是一直运行的
        pthread->status = TASK_RUNNING;
    } else {
        pthread->status = TASK_READY;
    }
    // self_kstack 是线程自己在内核态下使用的栈顶地址
    pthread->self_kstack = (uint32_t*)((uint32_t)pthread + PG_SIZE);
    pthread->priority = prio;
    pthread->ticks = prio;
    pthread->elapsed_ticks = 0;
    pthread->pgdir = NULL;

    // 预留标准输入输出
    pthread->fd_table[0] = 0;
    pthread->fd_table[1] = 1;
    pthread->fd_table[2] = 2;
    // 其余的全置为 -1
    uint8_t fd_idx = 3;
    while(fd_idx < MAX_FILES_OPEN_PER_PROC) {
        pthread->fd_table[fd_idx] = -1;
        fd_idx++;
    }
    pthread->cwd_inode_nr = 0;
    pthread->parent_pid = -1;
    pthread->stack_magic = 0x19870916; // 自定义魔数
}

// 线程所执行的函数是 function(func_arg)
struct task_struct* thread_start(char* name,            //线程名
                                 int prio,              //优先级
                                 thread_func function,  //要执行的函数
                                 void* func_arg)        //函数的参数
{
    // PCB 都位于内核空间, 包括用户进程的 PCB 也是在内核空间
    struct task_struct* thread = get_kernel_pages(1);   //申请一页内核空间存放PCB

    init_thread(thread, name, prio);                    //初始化线程
    thread_create(thread, function, func_arg);          //创建线程

    // 确保之前不在队列中
    ASSERT(!elem_find(&thread_ready_list, &thread->general_tag));
    // 加入就绪线程队列
    list_append(&thread_ready_list, &thread->general_tag);
    // 确保之前不在队列中
    ASSERT(!elem_find(&thread_all_list, &thread->all_list_tag));
    // 加入全部线程队列
    list_append(&thread_all_list, &thread->all_list_tag);

    return thread;
}

// 将 main 函数封装为主线程
static void make_main_thread(void) {
    main_thread = running_thread();
    init_thread(main_thread, "main", 31);

    ASSERT(!elem_find(&thread_all_list, &main_thread->all_list_tag));
    list_append(&thread_all_list, &main_thread->all_list_tag);
}

// 实现线程调度
void schedule(void) {
    ASSERT(intr_get_status() == INTR_OFF);

    struct task_struct* cur = running_thread();
    if(cur->status == TASK_RUNNING) {
        // 若此线程只是 CPU 时间片到了, 将其加入到就绪队尾
        ASSERT(!elem_find(&thread_ready_list, &cur->general_tag));
        list_append(&thread_ready_list, &cur->general_tag);
        cur->ticks = cur->priority;
        cur->status = TASK_READY;
    } else {
        // 若此线程阻塞, 不需要将其加入队列
    }

    // 如果就绪队列中没有可运行的任务, 就唤醒 idle
    if (list_empty(&thread_ready_list)) {
        thread_unblock(idle_thread);
    }

    ASSERT(!list_empty(&thread_ready_list));
    thread_tag = NULL;
    thread_tag = list_pop(&thread_ready_list);
    struct task_struct* next = elem2entry(struct task_struct, general_tag, thread_tag);
    next->status = TASK_RUNNING;

    process_activate(next);
    //while(1);
    switch_to(cur, next);
}

// 主动让出 cpu, 换其它线程运行
void thread_yield(void) {
    struct task_struct* cur = running_thread();
    enum intr_status old_status = intr_disable();
    ASSERT(!elem_find(&thread_ready_list, &cur->general_tag));
    list_append(&thread_ready_list, &cur->general_tag);
    cur->status = TASK_READY;
    schedule();
    intr_set_status(old_status);
}

// 回收 thread_over 的 pcb 和页表, 并将其从调度队列中去除
void thread_exit(struct task_struct* thread_over, bool need_schedule) {
    // 要保证 schedule 在关中断情况下调用
    intr_disable();
    thread_over->status = TASK_DIED;

    // 如果 thread_over 不是当前线程, 就有可能还在就绪队列中, 将其从中删除
    if (elem_find(&thread_ready_list, &thread_over->general_tag)) {
        list_remove(&thread_over->general_tag);
    }
    if (thread_over->pgdir) { // 如果是进程, 回收进程的页表
        mfree_page(PF_KERNEL, thread_over->pgdir, 1);
    }

    // 从 all_thread_list 中去掉此任务
    list_remove(&thread_over->all_list_tag);

    // 回收 pcb 所在的页, 主线程的 pcb 不在堆中, 跨过
    if (thread_over != main_thread) {
        mfree_page(PF_KERNEL, thread_over, 1);
    }
    
    // 归还 pid
    release_pid(thread_over->pid);

    // 如果需要下一轮调度则主动调用 schedule
    if (need_schedule) {
        schedule();
        PANIC("thread_exit: should not be here\n");
    }
}

// 比对任务的 pid
static bool pid_check(struct list_elem* pelem, int32_t pid) {
    struct task_struct* pthread = elem2entry(struct task_struct, all_list_tag, pelem);
    return pthread->pid == pid;
}

// 根据 pid 找 pcb，若找到则返回该 pcb，否则返回 NULL
struct task_struct* pid2thread(int32_t pid) {
    struct list_elem* pelem = list_traversal(&thread_all_list, pid_check, pid);
    if (pelem == NULL) {
        return NULL;
    }
    struct task_struct* thread = elem2entry(struct task_struct, all_list_tag, pelem);
    return thread;
}

// 初始化线程环境
void thread_init(void) {
    put_str("thread_init start\n");

    list_init(&thread_ready_list);
    list_init(&thread_all_list);
    pid_pool_init();

    // 先创建第一个用户进程 init
    process_execute(init, "init"); // init 进程的 pid 是 1

    // 将当前 main 函数创建为线程
    make_main_thread();

    // 创建 idle 线程
    idle_thread = thread_start("idle", 10, idle, NULL);

    put_str("thread_init done\n");
}


// 当前线程将自己阻塞, 标志其状态为 stat
void thread_block(enum task_status stat) {
    ASSERT(stat == TASK_BLOCKED || 
           stat == TASK_WAITING || 
           stat == TASK_HANGING);
    enum intr_status old_status = intr_disable();
    struct task_struct* cur_thread = running_thread();
    cur_thread->status = stat;
    schedule(); // 将当前线程换下处理器
    intr_set_status(old_status);
}

// 将线程解除阻塞
void thread_unblock(struct task_struct* pthread) {
    enum intr_status old_status = intr_disable();
    ASSERT(pthread->status == TASK_BLOCKED || 
           pthread->status == TASK_WAITING || 
           pthread->status == TASK_HANGING);
    ASSERT(!elem_find(&thread_ready_list, &pthread->general_tag));
    // 放在就绪队列最前面, 使其尽快得到调度
    list_push(&thread_ready_list, &pthread->general_tag);
    pthread->status = TASK_READY;
    intr_set_status(old_status);
}

pid_t fork_pid(void) {
    return allocate_pid();
}

/* 以填充空格的方式输出buf */
static void pad_print(char* buf, int32_t buf_len, void* ptr, char format) {
   memset(buf, 0, buf_len);
   uint8_t out_pad_0idx = 0;
   switch(format) {
      case 's':
	 out_pad_0idx = sprintf(buf, "%s", ptr);
	 break;
      case 'd':
	 out_pad_0idx = sprintf(buf, "%d", *((int16_t*)ptr));
      case 'x':
	 out_pad_0idx = sprintf(buf, "%x", *((uint32_t*)ptr));
   }
   while(out_pad_0idx < buf_len) { // 以空格填充
      buf[out_pad_0idx] = ' ';
      out_pad_0idx++;
   }
   sys_write(stdout_no, buf, buf_len - 1);
}

/* 用于在list_traversal函数中的回调函数,用于针对线程队列的处理 */
static bool elem2thread_info(struct list_elem* pelem/*, int arg UNUSED*/) {
   struct task_struct* pthread = elem2entry(struct task_struct, all_list_tag, pelem);
   char out_pad[16] = {0};

   pad_print(out_pad, 16, &pthread->pid, 'd');

   if (pthread->parent_pid == -1) {
      pad_print(out_pad, 16, "NULL", 's');
   } else { 
      pad_print(out_pad, 16, &pthread->parent_pid, 'd');
   }

   switch (pthread->status) {
      case 0:
	 pad_print(out_pad, 16, "RUNNING", 's');
	 break;
      case 1:
	 pad_print(out_pad, 16, "READY", 's');
	 break;
      case 2:
	 pad_print(out_pad, 16, "BLOCKED", 's');
	 break;
      case 3:
	 pad_print(out_pad, 16, "WAITING", 's');
	 break;
      case 4:
	 pad_print(out_pad, 16, "HANGING", 's');
	 break;
      case 5:
	 pad_print(out_pad, 16, "DIED", 's');
   }
   pad_print(out_pad, 16, &pthread->elapsed_ticks, 'x');

   memset(out_pad, 0, 16);
   ASSERT(strlen(pthread->name) < 17);
   memcpy(out_pad, pthread->name, strlen(pthread->name));
   strcat(out_pad, "\n");
   sys_write(stdout_no, out_pad, strlen(out_pad));
   return false;	// 此处返回false是为了迎合主调函数list_traversal,只有回调函数返回false时才会继续调用此函数
}

 /* 打印任务列表 */
void sys_ps(void) {
   char* ps_title = "PID            PPID           STAT           TICKS          COMMAND\n";
   sys_write(stdout_no, ps_title, strlen(ps_title));
   list_traversal(&thread_all_list, elem2thread_info, 0);
}


