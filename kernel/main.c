#include "print.h"
#include "init.h"
#include "debug.h"
#include "memory.h"

int main(){
	put_str("\nI am kernel\n");
	init_all();
	
	//asm volatile("sti");	//开启中断
	
	void* addr = get_kernel_pages(5);
	put_str("\n get_kernel_page start vaddr is:");
	put_int((uint32_t)addr);
	put_str("\n");


	while(1);
	return 0;
}
