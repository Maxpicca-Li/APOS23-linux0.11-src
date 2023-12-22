#ifndef _SCHED_H
#define _SCHED_H

#define NR_TASKS 64
#define HZ 100

#define FIRST_TASK task[0]
#define LAST_TASK task[NR_TASKS-1]

#include <linux/head.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include <signal.h>

#if (NR_OPEN > 32)
#error "Currently the close-on-exec-flags are in one word, max 32 files/proc"
#endif

#define TASK_RUNNING		0
#define TASK_INTERRUPTIBLE	1
#define TASK_UNINTERRUPTIBLE	2
#define TASK_ZOMBIE		3
#define TASK_STOPPED		4

#ifndef NULL
#define NULL ((void *) 0)
#endif

extern int copy_page_tables(unsigned long from, unsigned long to, long size);
extern int free_page_tables(unsigned long from, unsigned long size);

extern void sched_init(void);
extern void schedule(void);
extern void trap_init(void);
extern void panic(const char * str);
extern int tty_write(unsigned minor,char * buf,int count);

typedef int (*fn_ptr)();

struct i387_struct {
	long	cwd;
	long	swd;
	long	twd;
	long	fip;
	long	fcs;
	long	foo;
	long	fos;
	long	st_space[20];	/* 8*10 bytes for each FP-reg = 80 bytes */
};

struct tss_struct {
	long	back_link;	/* 16 high bits zero */
	long	esp0; // esp0, esp1, esp2, esp, 对应不同特权级的栈顶位置
	long	ss0;		/* 16 high bits zero */
	long	esp1;
	long	ss1;		/* 16 high bits zero */
	long	esp2;
	long	ss2;		/* 16 high bits zero */
	long	cr3;
	long	eip;
	long	eflags;
	long	eax,ecx,edx,ebx;
	long	esp;
	long	ebp;
	long	esi;
	long	edi;
	long	es;		/* 16 high bits zero */
	long	cs;		/* 16 high bits zero */
	long	ss;		/* 16 high bits zero */
	long	ds;		/* 16 high bits zero */
	long	fs;		/* 16 high bits zero */
	long	gs;		/* 16 high bits zero */
	long	ldt;		/* 16 high bits zero */
	long	trace_bitmap;	/* bits: trace 0, bitmap 16-31 */
	struct i387_struct i387;
};

// NOTE!!!一个进程一个
struct task_struct {
/* these are hardcoded - don't touch */
	long state;	/* -1 unrunnable, 0 runnable, >0 stopped */
	long counter;
	long priority;
	long signal; // 信号量
	struct sigaction sigaction[32];
	long blocked;	/* bitmap of masked signals */
/* various fields */
	int exit_code;
	unsigned long start_code,end_code,end_data,brk,start_stack;
	long pid,father,pgrp,session,leader;
	unsigned short uid,euid,suid;
	unsigned short gid,egid,sgid;
	long alarm; // 报警定时值（滴答数）
	long utime,stime,cutime,cstime,start_time;
	unsigned short used_math;
/* file system info */
	int tty;		/* -1 if no tty, so it must be signed */
	unsigned short umask;
	struct m_inode * pwd;
	struct m_inode * root;
	struct m_inode * executable;
	unsigned long close_on_exec;
	struct file * filp[NR_OPEN]; // 最多打开 20 个文件，进程的文件管理结构，filp[i] 对应一个文件管理表 file_table[i]，指向一个 i 节点管理表项 --> 允许重复打开一个文件
/* ldt for this task 0 - zero 1 - cs 2 - ds&ss */
	struct desc_struct ldt[3]; // 0-空，1-代码段 cs，2-数据和堆栈段 ds&ss。
/* tss for this task */
	struct tss_struct tss;
};

/*
 *  INIT_TASK is used to set up the first task table, touch at
 * your own risk!. Base=0, limit=0x9ffff (=640kB)
 * 进程0，进程的起源，手写完成
 *  -> 源于 Unix 的进程创建原则，即父进程创建子进程 -> 保证操作系统的绝对管理地位
 *  -> 道生一，一生二，二生三，三生万物
 * 面向对象 -> 模子原则，即类型创建对象
 * // {0x9f,0xc0fa00} 高 0x00c0fa00, 低 0x0000009f, base = 0x0000-0000，limit = 009f , 大小 limit+1 = 0xA0 = 160, 160*4KB
 * 
 */
#define INIT_TASK \
/* state etc */	{ 0,15,15, /* 就绪态，15个时间片，15优先级 */ \
/* signals */	0,{{},},0, \
/* ec,brk... */	0,0,0,0,0,0, \
/* pid etc.. */	0,-1,0,0,0, /* 进程号为0 */ \
/* uid etc */	0,0,0,0,0,0, \
/* alarm */	0,0,0,0,0,0, \
/* math */	0, \
/* fs info */	-1,0022,NULL,NULL,NULL,0, /* 进程0 的 pwd, root, executable 都是 NULL, 即进程0不挂载任何文件系统 */\
/* filp */	{NULL,}, \
/* ldt */	{ \
		{0,0}, \
		{0x9f,0xc0fa00}, \
		{0x9f,0xc0f200}, \
	}, \
/*tss*/	{0,PAGE_SIZE+(long)&,0x10,0,0,0,0,(long)&pg_dir,\
	 0,0,0,0,0,0,0,0, /* 这行第二个0是EFLAGS，赋值为0，也关了中断（决定了cli这类指令只能在0特权级使用）, IF为0，IOPL也为0 */\
	 0,0,0x17,0x17,0x17,0x17,0x17,0x17, \
	 _LDT(0),0x80000000, \
		{} \
	}, \
}

extern struct task_struct *task[NR_TASKS];
extern struct task_struct *last_task_used_math;
extern struct task_struct *current;
extern long volatile jiffies;
extern long startup_time;

#define CURRENT_TIME (startup_time+jiffies/HZ)

extern void add_timer(long jiffies, void (*fn)(void));
extern void sleep_on(struct task_struct ** p);
extern void interruptible_sleep_on(struct task_struct ** p);
extern void wake_up(struct task_struct ** p);

/*
 * Entry into gdt where to find first TSS. 0-nul, 1-cs, 2-ds, 3-syscall
 * 4-TSS0, 5-LDT0, 6-TSS1 etc ...
 */
#define FIRST_TSS_ENTRY 4
#define FIRST_LDT_ENTRY (FIRST_TSS_ENTRY+1)
// _TSS(1)=0b110000, 110（6,GDT中TSS0的下标）|0 GDT|00 特权级
#define _TSS(n) ((((unsigned long) n)<<4)+(FIRST_TSS_ENTRY<<3))
#define _LDT(n) ((((unsigned long) n)<<4)+(FIRST_LDT_ENTRY<<3))
#define ltr(n) __asm__("ltr %%ax"::"a" (_TSS(n)))
#define lldt(n) __asm__("lldt %%ax"::"a" (_LDT(n)))
#define str(n) \
__asm__("str %%ax\n\t" \
	"subl %2,%%eax\n\t" \
	"shrl $4,%%eax" \
	:"=a" (n) \
	:"a" (0),"i" (FIRST_TSS_ENTRY<<3))
/*
 *	switch_to(n) should switch tasks to task nr n, first
 * checking that n isn't the current task, in which case it does nothing.
 * This also clears the TS-flag if the task we switched to has used
 * tha math co-processor latest.
 */
// 进程切换，类似于任务门切换，tss 段的切换
#define switch_to(n) {\
struct {long a,b;} __tmp; /* 为ljmp的CS、EIP准备的数据结构 */\
__asm__("cmpl %%ecx,_current\n\t" /* 如果 n 为当前进程，没必要切换，直接退出 */\
	"je 1f\n\t" \
	"movw %%dx,%1\n\t" /* 段选择符 -> b */\
	"xchgl %%ecx,_current\n\t" /* _current 和 %%ecx 交换 */\
	"ljmp %0\n\t" /* long jmp %0,即参数中的段选择子，即先保存下条指令地址即eip+4到进程0的tss，恢复进程1的所有 tss 数据，无需偏移量。在此之前是进程0的内核态，在此之后是进程1的用户态，下列的代码不执行，由此产生任务切换。此时 sys pause->schedule->switch_to，还没有清栈，待到切换回进程0时，基于保存的 eip 调用下列代码，返回所有函数并清栈*/\
	"cmpl %%ecx,_last_task_used_math\n\t" /* 重要！进程0再次被调度时，回到的地方。判断原任务使用过协处理器吗，没有则跳转 */\
	"jne 1f\n\t" \
	"clts\n" /* 清 cr0 的 TS 标志，task swap */\
	"1:" \
	::"m" (*&__tmp.a),"m" (*&__tmp.b), /* %0 a是偏移量EIP，%1 b是段选择子CS，这里只赋值了b, %0默认为0 */\
	"d" (_TSS(n)),"c" ((long) task[n])); /* dx=_TSS(n)，即TSS n的索引号+权限，即其段选择符； ecx=task[n] */\
}

#define PAGE_ALIGN(n) (((n)+0xfff)&0xfffff000)

#define _set_base(addr,base) \
__asm__("movw %%dx,%0\n\t" /* base[15:0] */\
	"rorl $16,%%edx\n\t" \
	"movb %%dl,%1\n\t" /* base[23:16] */\
	"movb %%dh,%2" /* base[31:24] */\
	::"m" (*((addr)+2)), \
	  "m" (*((addr)+4)), \
	  "m" (*((addr)+7)), \
	  "d" (base) \
	:"dx")

#define _set_limit(addr,limit) \
__asm__("movw %%dx,%0\n\t" \
	"rorl $16,%%edx\n\t" \
	"movb %1,%%dh\n\t" \
	"andb $0xf0,%%dh\n\t" \
	"orb %%dh,%%dl\n\t" \
	"movb %%dl,%1" \
	::"m" (*(addr)), \
	  "m" (*((addr)+6)), \
	  "d" (limit) \
	:"dx")

#define set_base(ldt,base) _set_base( ((char *)&(ldt)) , base )
#define set_limit(ldt,limit) _set_limit( ((char *)&(ldt)) , (limit-1)>>12 )

// 获得 ldt 描述符的 base 和 limit
#define _get_base(addr) ({\
unsigned long __base; \
__asm__("movb %3,%%dh\n\t" \
	"movb %2,%%dl\n\t" \
	"shll $16,%%edx\n\t" \
	"movw %1,%%dx" \
	:"=d" (__base) \
	:"m" (*((addr)+2)), \
	 "m" (*((addr)+4)), \
	 "m" (*((addr)+7))); \
__base;})

#define get_base(ldt) _get_base( ((char *)&(ldt)) )

#define get_limit(segment) ({ \
unsigned long __limit; \
__asm__("lsll %1,%0\n\tincl %0":"=r" (__limit):"r" (segment)); \
__limit;})

#endif
