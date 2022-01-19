#include "print.h"
#include "init.h"
void main(void) {
    put_str("I am kernel!\n");
    init_all();
    asm volatile("sti");
    put_int(0x12345);
    while(1);
}