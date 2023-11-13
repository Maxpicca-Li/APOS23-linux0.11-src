#define move_to_user_mode() \
/*
eax=esp
这里的 %%esp 是 0、3 特权共用的栈，iret前是0特权的，iret后是3特权的，为进程0的用户栈。
push 5个值到 user_stack，模拟了3特权级的中断压栈过程。进程0和内核的段基址一致，所以这里的 push 最后 iret 后就进入进程0了
	ss:     0x17 = 0b10|1|11 = 数据段|ldt|3特权级，对应 ss，stack segment
	esp:    esp，之前eax=esp，已经赋值了（如果不提前存下 esp 的值，esp会随着 push 逐步减）
	eflags: pushfl
	cs:     0x0f = 0b 1|1|11 = 代码段|ldt|3特权级，对应 cs，code segment
	eip:    $1f: 1 forward，前面的1 label，相当于 eip（$1b: 1back，后面的1 label）
iret 中断返回，和 main 函数手法类似
	函数调用：时间已知，由编译器进行压栈
	中断调用：时间未知，由硬件机制进行压栈，压的内容由ISA决定
    此后，进程0开始执行，特权级 0->3
1: return 的地址，即之前 pushl $1f（eip）。这里进程0的代码开始对齐特权级
    eax = 0x17
    ds = ax
    es = ax
    fs = ax
    gs = ax
至此，进程0的状态数据完成，其中 sched_init 完成了 TSS、LDT、TR、LDTR 的加载，这里完成了用户模式的切换。

之所以此时是进程0的，是因为 sched_init 中 ldtr 和 tr 设置为了进程 0
*/
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

// TODO lyq: 这里门设置的含义是什么？
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
