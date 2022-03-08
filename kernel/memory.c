#include "memory.h" 
#include "bitmap.h"
#include "stdint.h"
#include "print.h" 
#include "global.h"
#include "debug.h"
#include "string.h"
#include "thread.h"
#include "sync.h"

#define PG_SIZE 4096

#define PDE_IDX(addr) ((addr & 0xffc00000) >> 22)
#define PTE_IDX(addr) ((addr & 0x003ff000) >> 12)
int page_table_add_num = 0;
/* 位图地址 */
//因为 0xc009f000是内核主线程栈顶， 0xc009e000是内核主线程的pcb。
//一个页框大小的位图可表示128MB内存，位图位置安排在地址0xc009a000,这样本系统最大支持4个页框的位图， 即512MB
#define MEM_BITMAP_BASE 0xc009a000

//0xc0000000是内核从虚拟地址3G 起。0x100000意指跨过低端1MB内存，使虚拟地址在逻辑上连续
#define K_HEAP_START 0xc0100000

//内存池结构，生成两个实例用于管理内核内存池和用户内存池
struct pool{
	struct bitmap pool_bitmap;	//本内存池用到的位图结构， 用于管理物理内存
	uint32_t phy_addr_start;	//本内存池所管理物理内存的起始地址
	uint32_t pool_size;			//本内存池字节容量
	struct lock lock; 			//申请内存时互斥
};

struct pool kernel_pool, user_pool;	//生成内核内存池和用户内存池
struct virtual_addr kernel_vaddr; 	//此结构用来给内核分配虚拟地址

//初始化内存池
static void mem_pool_init(uint32_t all_mem) { 
	put_str("mem_pool_init start\n");
	uint32_t page_table_size = PG_SIZE * 256;	//记录页目录表和页表占用的字节大小
	//页表大小 ＝ 1页的页目录表 ＋第 0 和第 768 个页目录项指向同一个页表,之前创建页表的时候，挨着页目录表创建了768-1022总共255个页表+上页目录的1页大小，就是256
	//第 769~1022 个页目录项共指向 254 个页表，共 256 个页框
		
	uint32_t used_mem = page_table_size + 0x100000; //当前已经使用的内存字节数，1M部分已经使用了，1M往上是页表所占用的空间
	uint32_t free_mem = all_mem - used_mem;		//剩余可用内存字节数
	uint16_t all_free_pages = free_mem / PG_SIZE; 	//所有可用的页
	// 1页为 4KB, 不管总内存是不是 4k 的倍数,对于以页为单位的内存分配策略， 不足 1 页的内存不用考虑了
	
	uint16_t kernel_free_pages = all_free_pages / 2;
	uint16_t user_free_pages = all_free_pages - kernel_free_pages;

	//为简化位图操作，余数不处理，坏处是这样做会丢内存。好处是不用做内存的越界检查，因为位图表示的内存少于实际物理内存。
	uint32_t kbm_length = kernel_free_pages / 8;			//Kernel Bitmap的长度，位图中的一位表示一页，以字节为单位，也就是8页表示1字节的位图
	uint32_t ubm_length = user_free_pages /8;

	uint32_t kp_start = used_mem;					//kernel pool start,内核内存池起始地址
	uint32_t up_start = kp_start + kernel_free_pages * PG_SIZE;	//内核已使用的+没使用的，就是分配给内核的全部内存，剩下给用户

	kernel_pool.phy_addr_start = kp_start;
	user_pool.phy_addr_start = up_start;

	kernel_pool.pool_size = kernel_free_pages * PG_SIZE;		//内存池里存放的是空闲的内存，所以用可用内存大小填充
	user_pool.pool_size = user_free_pages * PG_SIZE;

	kernel_pool.pool_bitmap.btmp_bytes_len = kbm_length;
	user_pool.pool_bitmap.btmp_bytes_len = ubm_length;

	//内核内存池和用户内存池位图
	//位图是全局的数据，长度不固定。
	//全局或静态的数组需要在编译时知道其长度，而我们需要根据总内存 大小算出需要多少字节，所以改为指定一块内存来生成位图。
	//内核使用的最高地址是Oxc009f000, 这是主线程的栈地址，32MB内存占用的位图是2KB
	//内核内存池的位图先定在 MEM_BITMAP_BASE(Oxc009a000)处
	
	kernel_pool.pool_bitmap.bits = (void*)MEM_BITMAP_BASE;
	user_pool.pool_bitmap.bits = (void*)(MEM_BITMAP_BASE + kbm_length);

	//输出内存池信息
	put_str("   kernel_pool_bitmap_start:"); 
	put_int((int)kernel_pool.pool_bitmap.bits); 

	put_str("   kernel_pool_phy_addr_start: "); 
	put_int(kernel_pool.phy_addr_start); 

	put_str ("\n");

	put_str ("    user_pool_bitmap_start: ");
	put_int ((int) user_pool.pool_bitmap.bits);

	put_str ("    user_pool_phy_addr_start: ");
	put_int(user_pool.phy_addr_start);

	put_str ("\n");

	// 将位图置 0
	bitmap_init(&kernel_pool.pool_bitmap);
	bitmap_init(&user_pool.pool_bitmap);

	lock_init(&kernel_pool.lock);
    lock_init(&user_pool.lock);

	//下面初始化内核虚拟地址的位图，按实际物理内存大小生成数组。
	kernel_vaddr.vaddr_bitmap.btmp_bytes_len = kbm_length; 
	//用干维护内核堆的虚拟地址，所以要和内核内存池大小一致
	
	//位图的数组指向一块没用的内存，目前定位在内核内存池和用户内存池之外
	kernel_vaddr.vaddr_bitmap.bits = (void*)(MEM_BITMAP_BASE + kbm_length + ubm_length);

	kernel_vaddr.vaddr_start = K_HEAP_START;
	bitmap_init(&kernel_vaddr.vaddr_bitmap);
	
	put_str("    mem_pool_init done \n"); 
}

//在 pf 表示的虚拟内存池中申请 pg_cnt 个虚拟页，成功则返回虚拟页的起始地址，失败则返回 NULL
static void* vaddr_get(enum pool_flags pf, uint32_t pg_cnt){
	int vaddr_start = 0, bit_idx_start = -1;
	uint32_t cnt = 0;
	if(pf == PF_KERNEL){
		bit_idx_start = bitmap_scan(&kernel_vaddr.vaddr_bitmap, pg_cnt);	//获取虚拟页的位起始值
		if(bit_idx_start == -1){
			return NULL;
		}
		while(cnt < pg_cnt){
			bitmap_set(&kernel_vaddr.vaddr_bitmap, bit_idx_start + cnt++, 1);	//将位起始值开始连续置1，直到设置完需要的页位置
		}
		vaddr_start = kernel_vaddr.vaddr_start + bit_idx_start * PG_SIZE;
    } else { // 用户内存池
        struct task_struct* cur = running_thread();
        bit_idx_start = bitmap_scan(&cur->userprog_vaddr.vaddr_bitmap, pg_cnt);
        if(bit_idx_start == -1) {
            return NULL;
        }
        while(cnt < pg_cnt) {
            bitmap_set(&cur->userprog_vaddr.vaddr_bitmap, bit_idx_start+cnt++, 1);
        }
        vaddr_start = cur->userprog_vaddr.vaddr_start + bit_idx_start * PG_SIZE;
        ASSERT((uint32_t)vaddr_start < (0xc0000000 - PG_SIZE));
	}
	return (void*)vaddr_start;
}

//得到虚拟地址 vaddr 对应的 pte 指针
uint32_t* pte_ptr(uint32_t vaddr){
	// 先访问到页表自己
	// 再用页目录项 pde（页目录内页表的索引）作为pte的索引访问到页表
	// 再用pte的索引作为页内偏移
	uint32_t* pte = (uint32_t*)(0xffc00000 + ((vaddr & 0xffc00000) >> 10) + PTE_IDX(vaddr) * 4);
	//第一步：0xffc00000 是取出第1023个页目录项进行索引，其实就是页目录表的物理地址
	//第二步：((vaddr & 0xffc00000) >> 10) 是将原来vaddr的前10位取出，放在中间10位的位置上，用来获取 pte 的
	//第三步：PTE_IDX(vaddr) * 4 会被当作物理偏移直接加上，而不会像其前面10位会被cpu自动*4再加上，所以这里手动*4，获取PTE索引，得到PTE物理地址
	return pte;
}

//得到虚拟地址vaddr对应的pde的指针 
uint32_t* pde_ptr(uint32_t vaddr){
	//0xfffff 用来访问到页表本身所在的地址
	//前10位是1023，是页目录表的物理地址
	//中10位是1023，索引到的还是页目录表的物理地址
	//后12位是addr的前10位*4，也就是页目录表的索引
	uint32_t* pde = (uint32_t*)((0xfffff000) + PDE_IDX(vaddr) * 4);
	return pde;
}

//在m_pool指向的物理内存池中分配1个物理页，成功则返回页框的物理地址，失败则返回NULL
static void* palloc(struct pool* m_pool){
	//扫描或设置位图要保证原子操作
	int bit_idx = bitmap_scan(&m_pool->pool_bitmap, 1);	//找一个物理页面,位图中1位表示实际1页地址
	if(bit_idx == -1){
		return NULL;
	}
	bitmap_set(&m_pool->pool_bitmap, bit_idx, 1);		//将此位的bit_idx置1
	uint32_t page_phyaddr = ((bit_idx * PG_SIZE) + m_pool->phy_addr_start);	//物理内存池起始地址 + 页偏移 = 页地址
	return (void*)page_phyaddr;
}

//页表中添加虚拟地址_vaddr与物理地址_page_phyaddr的映射
static void page_table_add(void* _vaddr, void* _page_phyaddr){
	uint32_t vaddr = (uint32_t)_vaddr;
	uint32_t page_phyaddr = (uint32_t)_page_phyaddr;
	uint32_t* pde = pde_ptr(vaddr);
	uint32_t* pte = pte_ptr(vaddr);
	//console_put_str("page_table_add");console_put_int(++page_table_add_num);console_put_str("\n");

	//执行*pte，会访问到空的 pde。所以确保pde创建完成后才能执行*pte,否则 会引发page_fault。 
	//因此在 *pde为0时，pte只能出现在下面 else语句块中的*pde后面。
	//总之，就是创建pte之前必须先创建好pde才行，不然没法通过pde访问到pte

	//先在页目录内判断目录项的p位， 若为1, 则表示该表已存在
	if(*pde & 0x00000001){
		//页目录项和页表项的第0位为P, 此处判断目录项是否存在
		ASSERT(!(*pte & 0x00000001));	//此时pte应该不存在
		if(!(*pte & 0x00000001)){	//只要是创建页表，pte就应该不存在，多判断一下放心
			*pte = (page_phyaddr | PG_US_U | PG_RW_W | PG_P_1); //创建pte
		}else{				//目前执行不到这里
			PANIC("pte repeat");
			*pte = (page_phyaddr | PG_US_U | PG_RW_W | PG_P_1);
		}
	}else{
		//页表中用到的页框一律从内核空间分配 
		uint32_t pde_phyaddr = (uint32_t)palloc(&kernel_pool);
		*pde = (pde_phyaddr | PG_US_U | PG_RW_W | PG_P_1);
		//分配到的物理页地址pde_phyaddr对应的物理 内存清0
		//避免里面的陈旧数据变成了页表项，从而让页表混乱。
		//访间到 pde对应的物理地址，用 pte取高20位便可。
		//因为pte基千该pde对应的物理地址内再寻址，把低12位置0便是该pde对应的物理页的起始。
		memset((void*)((int)pte & 0xfffff000), 0, PG_SIZE);

		ASSERT(!(*pte & 0x00000001));
		*pte = (page_phyaddr | PG_US_U | PG_RW_W | PG_P_1);
	}
}


// 分配 pg_cnt 个页空间， 成功则返回起始虚拟地址，失败时返回 NULL
void* malloc_page(enum pool_flags pf, uint32_t pg_cnt){
	ASSERT(pg_cnt > 0 && pg_cnt < 3840);
	
	//malloc_page 的原理是三个动作的合成：
	//1 通过 vaddr_get在虚拟内存池中申请虚拟地址
	//2 通过 palloc在物理内存池中申请物理页
	//3 通过 page_table_add 将以上得到的虚拟地址和物理地址在页表中完成映射
	
	void* vaddr_start = vaddr_get(pf, pg_cnt);

	if(vaddr_start == NULL){
		return NULL;
	}

	uint32_t vaddr = (uint32_t)vaddr_start;
	uint32_t cnt = pg_cnt;
	struct pool* mem_pool = pf & PF_KERNEL ? &kernel_pool : &user_pool;
	
	//因为虚拟地址是连续的，但物理地址不连续，所以逐个映射
	while(cnt-- > 0){
		void* page_phyaddr = palloc(mem_pool);
		if(page_phyaddr == NULL){
			//失败时要将曾经已申请的虚拟地址和
			//物理页全部回滚，在将来完成内存回收时再补充
			return NULL;
		}
		page_table_add((void*)vaddr, page_phyaddr);	//在表中逐个做映射
		vaddr += PG_SIZE;
	}
	return vaddr_start;
}

// 从内核物理内存池中申请内存，成功返回虚拟地址，失败返回NULL
void* get_kernel_pages(uint32_t pg_cnt){
	lock_acquire(&kernel_pool.lock);
	void* vaddr = malloc_page(PF_KERNEL, pg_cnt);
	if(vaddr != NULL){
		memset(vaddr, 0, pg_cnt * PG_SIZE);
	}
    lock_release(&kernel_pool.lock);
	return vaddr;
}

// 在用户空间中申请 4k 内存, 并返回其虚拟地址
void* get_user_pages(uint32_t pg_cnt) {
    lock_acquire(&user_pool.lock);
    void* vaddr = malloc_page(PF_USER, pg_cnt);
    memset(vaddr, 0, pg_cnt * PG_SIZE);
    lock_release(&user_pool.lock);
    return vaddr;
}

// 将地址 vaddr 与 pf 池中的物理地址关联, 仅支持一页空间分配
void* get_a_page(enum pool_flags pf, uint32_t vaddr) {
    struct pool* mem_pool = pf & PF_KERNEL ? &kernel_pool : &user_pool;
    lock_acquire(&mem_pool->lock);
    // 先将虚拟地址对应的位图置 1
    struct task_struct* cur = running_thread();
    int32_t bit_idx = -1;
    
    if(cur->pgdir != NULL && pf == PF_USER) {
        // 若当前是用户进程申请用户内存, 就修改用户进程自己的虚拟地址位图
        bit_idx = (vaddr - cur->userprog_vaddr.vaddr_start) / PG_SIZE;
        ASSERT(bit_idx > 0);
        bitmap_set(&cur->userprog_vaddr.vaddr_bitmap, bit_idx, 1);
    } else if(cur->pgdir == NULL && pf == PF_KERNEL) {
        // 如果是内核线程申请内核内存, 就修改 kernel_vaddr
        bit_idx = (vaddr - kernel_vaddr.vaddr_start) / PG_SIZE;
        ASSERT(bit_idx > 0);
        bitmap_set(&kernel_vaddr.vaddr_bitmap, bit_idx, 1);
    } else {
        PANIC("get_a_page: not allow kernel alloc userspace or user alloc kernelspace by get_a_page");
    }

    void* page_phyaddr = palloc(mem_pool);
    if(page_phyaddr == NULL) {
        return NULL;
    }
    page_table_add((void*)vaddr, page_phyaddr);
    lock_release(&mem_pool->lock);
    return (void*)vaddr;
}

// 得到虚拟地址映射到的物理地址
uint32_t addr_v2p(uint32_t vaddr) {
    uint32_t* pte = pte_ptr(vaddr);
    return ((*pte & 0xfffff000) + (vaddr & 0x00000fff));
}


//内存管理部分初始化入口
void mem_init(){
	put_str("mem_init start\n");
	uint32_t mem_bytes_total = (*(uint32_t*)(0xb00)); 
	mem_pool_init(mem_bytes_total);
	put_str("mem_init done\n"); 
}
