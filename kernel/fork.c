/*
 *  linux/kernel/fork.c
 *
 *  (C) 1991  Linus Torvalds
 */

/*
 *  'fork.c' contains the help-routines for the 'fork' system call
 * (see also system_call.s), and some misc functions ('verify_area').
 * Fork is rather simple, once you get the hang of it, but the memory
 * management can be a bitch. See 'mm/mm.c': 'copy_page_tables()'
 */
#include <errno.h>

#include <linux/sched.h>
#include <linux/kernel.h>
#include <asm/segment.h>
#include <asm/system.h>

extern void write_verify(unsigned long address);

long last_pid=0;

void verify_area(void * addr,int size)
{
	unsigned long start;

	start = (unsigned long) addr;
	size += start & 0xfff;
	start &= 0xfffff000;
	start += get_base(current->ldt[2]);
	while (size>0) {
		size -= 4096;
		write_verify(start);
		start += 4096;
	}
}

int copy_mem(int nr,struct task_struct * p)
{
	unsigned long old_data_base,new_data_base,data_limit;
	unsigned long old_code_base,new_code_base,code_limit;

	code_limit=get_limit(0x0f); // 获得父进程的代码段限长 01 code seg.｜1 ldt｜11 3 pri.
	data_limit=get_limit(0x17); // 获得父进程的数据段限长 10 data seg.｜1 ldt｜11 3 pri.
	old_code_base = get_base(current->ldt[1]);
	old_data_base = get_base(current->ldt[2]);
	if (old_data_base != old_code_base)
		panic("We don't support separate I&D");
	if (data_limit < code_limit)
		panic("Bad data_limit");
	new_data_base = new_code_base = nr * 0x4000000; // 0b100 * 2^24 Byte = 0b1000000 * 2^20 = 64 MB
	p->start_code = new_code_base;
	set_base(p->ldt[1],new_code_base);
	set_base(p->ldt[2],new_data_base);
	if (copy_page_tables(old_data_base,new_data_base,data_limit)) {
		free_page_tables(new_data_base,data_limit);
		return -ENOMEM;
	}
	return 0;
}

/*
 *  Ok, this is the main fork-routine. It copies the system process
 * information (task[nr]) and sets up the necessary registers. It
 * also copies the data segment in it's entirety.
 */
// NOTE lyq: 核心代码+1! 所有进程创建都是 copy_process
// 参数右序进栈
int copy_process(
		// _sys_fork 调用__copy_process前的压栈，nr 是eax中的值，此时已经被复制为分配的pid的值，即1
		int nr,long ebp,long edi,long esi,long gs,
		// _system_call 调用__sys_fork前的压栈, 其中 long none 表示 call _sys_call_table(,%eax,4)，供 iret 返回确定位置
		long none,long ebx,long ecx,long edx, long fs,long es,long ds,
		// int $0x80 中断压栈
		long eip,long cs,long eflags,long esp,long ss)
{
	// 新进程的值
	struct task_struct *p;
	int i;
	struct file *f;

	// 获得一个空闲页，账本 mem_map -> 用于分配内存（如空闲页、共享页）
	// 强制类型转换，即把这个页当作 task_struct 使用
	p = (struct task_struct *) get_free_page();
	if (!p) // 结果检测
		return -EAGAIN;
	task[nr] = p;
	// 复制进程 0 的 task_struct 内容，此时 ldt & tss 也一样，为后面的 copy on write 做了准备 -> 开始的时候共享，当子进程write的时候，才开始加载
	*p = *current;	/* NOTE! this doesn't copy the supervisor stack */
	p->state = TASK_UNINTERRUPTIBLE; // 只有内核代码中明确表示将该进程设置为就绪状态才能被唤醒; 除此之外，没有任何办法将其唤醒
	// 进程自定义设置
	p->pid = last_pid;
	p->father = current->pid;
	p->counter = p->priority;
	p->signal = 0;
	p->alarm = 0;
	p->leader = 0;		/* process leadership doesn't inherit */
	p->utime = p->stime = 0;
	p->cutime = p->cstime = 0;
	p->start_time = jiffies;
	p->tss.back_link = 0;
	p->tss.esp0 = PAGE_SIZE + (long) p;//esp0是内核栈指针
	p->tss.ss0 = 0x10; //0x10就是10000，0特权级，GDT，数据段
	p->tss.eip = eip; //重要！就是参数的EIP，是int 0x80压栈的，指向的是 int 0x80 的下一行：if(__res >= 0)
	p->tss.eflags = eflags;
	p->tss.eax = 0;   //重要！决定main()函数中if (!fork())后面的分支走向 --> 所有的父进程创建子进程都是这样
	p->tss.ecx = ecx;
	p->tss.edx = edx;
	p->tss.ebx = ebx;
	p->tss.esp = esp;
	p->tss.ebp = ebp;
	p->tss.esi = esi;
	p->tss.edi = edi;
	p->tss.es = es & 0xffff;
	p->tss.cs = cs & 0xffff;
	p->tss.ss = ss & 0xffff;
	p->tss.ds = ds & 0xffff;
	p->tss.fs = fs & 0xffff;
	p->tss.gs = gs & 0xffff;
	p->tss.ldt = _LDT(nr);
	p->tss.trace_bitmap = 0x80000000;
	if (last_task_used_math == current)
		__asm__("clts ; fnsave %0"::"m" (p->tss.i387));
	if (copy_mem(nr,p)) {
		task[nr] = NULL;
		free_page((long) p);
		return -EAGAIN;
	}
	for (i=0; i<NR_OPEN;i++)
		if (f=p->filp[i])
			f->f_count++;
	if (current->pwd)
		current->pwd->i_count++;
	if (current->root)
		current->root->i_count++;
	if (current->executable)
		current->executable->i_count++;
	// 每次新建进程，都会设置 tss & ldt
	set_tss_desc(gdt+(nr<<1)+FIRST_TSS_ENTRY,&(p->tss));
	set_ldt_desc(gdt+(nr<<1)+FIRST_LDT_ENTRY,&(p->ldt));
	p->state = TASK_RUNNING;	/* do this last, just in case */
	return last_pid;
}

int find_empty_process(void)
{
	int i;

	repeat:
		if ((++last_pid)<0) last_pid=1;
		/* 理解
		++last_pid; // 每来一个 find_empty_process 就 +1, 之后都会保证 last_pid 为接下来可新分配的pid (注意 pid 从 0 开始算)
		if (last_pid<0) last_pid=1; // 防止溢出
		*/
		for(i=0 ; i<NR_TASKS ; i++)
			// 防止此时 last_pid 已经分配了，则需要 last_pid 再加 1，然后重新进行检查
			if (task[i] && task[i]->pid == last_pid) goto repeat;
	// 找空闲的 task
	for(i=1 ; i<NR_TASKS ; i++)
		if (!task[i])
			return i;
	return -EAGAIN;
}
