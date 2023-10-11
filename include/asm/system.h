#define move_to_user_mode() \
__asm__ ("movl %%esp,%%eax\n\t" \
	"pushl $0x17\n\t" \
	"pushl %%eax\n\t" \
	"pushfl\n\t" \
	"pushl $0x0f\n\t" \
	"pushl $1f\n\t" \
	"iret\n" \
	"1:\tmovl $0x17,%%eax\n\t" \
	"movw %%ax,%%ds\n\t" \
	"movw %%ax,%%es\n\t" \
	"movw %%ax,%%fs\n\t" \
	"movw %%ax,%%gs" \
	:::"ax")

#define sti() __asm__ ("sti"::) // 开中断
#define cli() __asm__ ("cli"::) // 关中断
#define nop() __asm__ ("nop"::)

#define iret() __asm__ ("iret"::)

/*
gcc 嵌入汇编：
    gate_addr，对应中断号在中断描述符表中的地址；
    type，中断描述符的类型；
    dpl，特权级；
    addr，中断服务程序地址
%%ax = %%dx; %%dx = %0 后的结果如下：
    addr[31:16] -> edx h
    1000 1111 0000 0000 -> dx
    段选择子 0x0008 -> eax h
    addr[15:0] -> ax
%1 = %%eax; %2 = %%edx 即把寄存器的结果写入到IDT对应的位置中：
    gate_addr: eax -> [31: 0]
               edx -> [63:32]
(*((char *) (gate_addr)))：(解引用(指针))，即该内存地址对应的数据
*/
#define _set_gate(gate_addr,type,dpl,addr) \
__asm__ ("movw %%dx,%%ax\n\t" \
	"movw %0,%%dx\n\t" \
	"movl %%eax,%1\n\t" \
	"movl %%edx,%2" \
	: \
	: "i" ((short) (0x8000+(dpl<<13)+(type<<8))), \
	"o" (*((char *) (gate_addr))), \
	"o" (*(4+(char *) (gate_addr))), \
	"d" ((char *) (addr)),"a" (0x00080000))
// (char *) 指针也有类型，当时只找到字节指针，所以用的char *，但是后来用 (void *) 来代表指针类型

// 中断门：硬件或软件异常/中断
#define set_intr_gate(n,addr) \
	_set_gate(&idt[n],14,0,addr)

// 陷阱门：相关指令执行
#define set_trap_gate(n,addr) \
	_set_gate(&idt[n],15,0,addr)

// 任务门：进程调用/系统调用等
#define set_system_gate(n,addr) \
	_set_gate(&idt[n],15,3,addr)

#define _set_seg_desc(gate_addr,type,dpl,base,limit) {\
	*(gate_addr) = ((base) & 0xff000000) | \
		(((base) & 0x00ff0000)>>16) | \
		((limit) & 0xf0000) | \
		((dpl)<<13) | \
		(0x00408000) | \
		((type)<<8); \
	*((gate_addr)+1) = (((base) & 0x0000ffff)<<16) | \
		((limit) & 0x0ffff); }

#define _set_tssldt_desc(n,addr,type) \
__asm__ ("movw $104,%1\n\t" \
	"movw %%ax,%2\n\t" \
	"rorl $16,%%eax\n\t" \
	"movb %%al,%3\n\t" \
	"movb $" type ",%4\n\t" \
	"movb $0x00,%5\n\t" \
	"movb %%ah,%6\n\t" \
	"rorl $16,%%eax" \
	::"a" (addr), "m" (*(n)), "m" (*(n+2)), "m" (*(n+4)), \
	 "m" (*(n+5)), "m" (*(n+6)), "m" (*(n+7)) \
	)

#define set_tss_desc(n,addr) _set_tssldt_desc(((char *) (n)),addr,"0x89")
#define set_ldt_desc(n,addr) _set_tssldt_desc(((char *) (n)),addr,"0x82")
