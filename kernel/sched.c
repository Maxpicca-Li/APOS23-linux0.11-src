/*
 *  linux/kernel/sched.c
 *
 *  (C) 1991  Linus Torvalds
 */

/*
 * 'sched.c' is the main kernel file. It contains scheduling primitives
 * (sleep_on, wakeup, schedule etc) as well as a number of simple system
 * call functions (type getpid(), which just extracts a field from
 * current-task
 */
#include <linux/sched.h>
#include <linux/kernel.h>
#include <linux/sys.h>
#include <linux/fdreg.h>
#include <asm/system.h>
#include <asm/io.h>
#include <asm/segment.h>

#include <signal.h>

#define _S(nr) (1<<((nr)-1))
#define _BLOCKABLE (~(_S(SIGKILL) | _S(SIGSTOP)))

void show_task(int nr,struct task_struct * p)
{
	int i,j = 4096-sizeof(struct task_struct);

	printk("%d: pid=%d, state=%d, ",nr,p->pid,p->state);
	i=0;
	while (i<j && !((char *)(p+1))[i])
		i++;
	printk("%d (of %d) chars free in kernel stack\n\r",i,j);
}

void show_stat(void)
{
	int i;

	for (i=0;i<NR_TASKS;i++)
		if (task[i])
			show_task(i,task[i]);
}

#define LATCH (1193180/HZ)

extern void mem_use(void);

extern int timer_interrupt(void);
extern int system_call(void);

// 两项共用体，一个 task_union 一个页，4KB
union task_union {
	struct task_struct task; // 顶多占到一页
	char stack[PAGE_SIZE];   // 进程的内核栈，精心测算，内核代码压栈绝对不会覆盖 task_struct
};

// NOTE lyq:静态全局变量，static 赋予内部链接，即其只能在定义它的源文件中访问
static union task_union init_task = {INIT_TASK,};

long volatile jiffies=0; // 从开机开始算起的滴答数（10ms/滴答）
long startup_time=0;
// 所以 *current 指的进程0的task_struct
struct task_struct *current = &(init_task.task);
struct task_struct *last_task_used_math = NULL;

struct task_struct * task[NR_TASKS] = {&(init_task.task), };

// 进程 0 的用户栈
long user_stack [ PAGE_SIZE>>2 ] ;

struct {
	long * a;
	short b;
	} stack_start = { & user_stack [PAGE_SIZE>>2] , 0x10 };
/*
 *  'math_state_restore()' saves the current math information in the
 * old math state array, and gets the new ones from the current task
 */
void math_state_restore()
{
	if (last_task_used_math == current)
		return;
	__asm__("fwait");
	if (last_task_used_math) {
		__asm__("fnsave %0"::"m" (last_task_used_math->tss.i387));
	}
	last_task_used_math=current;
	if (current->used_math) {
		__asm__("frstor %0"::"m" (current->tss.i387));
	} else {
		__asm__("fninit"::);
		current->used_math=1;
	}
}

/*
 *  'schedule()' is the scheduler function. This is GOOD CODE! There
 * probably won't be any reason to change this, as it should work well
 * in all circumstances (ie gives IO-bound processes good response etc).
 * The one thing you might take a look at is the signal-handler code here.
 *
 *   NOTE!!  Task 0 is the 'idle' task, which gets called when no other
 * tasks can run. It can not be killed, and it cannot sleep. The 'state'
 * information in task[0] is never used.
 *   NOTE lyq: 进程调度，启动！
 */
void schedule(void)
{
	int i,next,c;
    struct task_struct ** p;    // 指向指针的指针

/* check alarm, wake up any interruptible tasks that have got a signal */

	for(p = &LAST_TASK ; p > &FIRST_TASK ; --p)
		if (*p) {
			if ((*p)->alarm && (*p)->alarm < jiffies) { // 设置了定时且定时已过
					(*p)->signal |= (1<<(SIGALRM-1)); // 设置信号量
					(*p)->alarm = 0; // 关闭警报
				}
			if (((*p)->signal & ~(_BLOCKABLE & (*p)->blocked)) &&
			(*p)->state==TASK_INTERRUPTIBLE) // 信号量中除被阻塞的信号外还有其它信号，且进程处于可中断状态
				(*p)->state=TASK_RUNNING; // 设置就绪态 --> 先处理 signal 的事情
		}

/* this is the scheduler proper: */

	while (1) {
		c = -1;                 // c = 0xFFFFFFFF
		next = 0;               // 指向下一个进程；默认进程0 【业界称进程0为怠速进程，我称其为劳模进程】
		i = NR_TASKS;           // i = 64
		p = &task[NR_TASKS];
		while (--i) {           // 高往低遍历；找就绪态，时间片最多的
			if (!*--p)
				continue;
			if ((*p)->state == TASK_RUNNING && (*p)->counter > c)
				c = (*p)->counter, next = i;
		}
		if (c) break;           // 注意 c 为 counter；如果一个都没找到，c 还是 -1，next 为 0，break 切换到0
		// 若找到的时间片为0，则重置所有进程的时间片，继续找
		for(p = &LAST_TASK ; p > &FIRST_TASK ; --p) // 高往低遍历
			if (*p)             // 优先级设置：该系统不单列优先级，折成时间片
				(*p)->counter = ((*p)->counter >> 1) +
						(*p)->priority;
	}
	switch_to(next); // 找到进程后进行切换
}

int sys_pause(void)     // 做进程调度，目前是**current 进程的内核态**在跑
{
	current->state = TASK_INTERRUPTIBLE;    // 目前可能是进程 0（如果创建进程1的时候，进程0没有state/counter，就会被调度走）
	schedule();
	return 0;
}

/* 
等待这个共享 buffer 的所有进程，利用各个等待进程的内核栈（tmp 局部变量存储到内核栈），构建一个缓冲块进程等待队列，sleep_on 每次回去
 */
void sleep_on(struct task_struct **p) // FIXME lyq: 为啥是从 task 数组角度考虑的呢？
{
	struct task_struct *tmp;

	if (!p)
		return;
	if (current == &(init_task.task))
		panic("task[0] trying to sleep");
	tmp = *p; // tmp 存“上一个 b_wait 进程”, p 指向 b_wait 进程，buffer 这边第一次调用时为 NULL; 下一步 p 将指向进程1的task_struct
	*p = current; // p 指向当前需要 buffer 的进程
	current->state = TASK_UNINTERRUPTIBLE; // 开始让 current 执行；进程1在这里被挂起；进程0在上面的 sys_pause 中被挂起 --> 【全部被挂起】
	schedule();
	if (tmp)
		tmp->state=0; // 0 即 TASK_RUNNING
}

void interruptible_sleep_on(struct task_struct **p)
{
	struct task_struct *tmp;

	if (!p)
		return;
	if (current == &(init_task.task))
		panic("task[0] trying to sleep");
	tmp=*p;
	*p=current;
repeat:	current->state = TASK_INTERRUPTIBLE;
	schedule();
	if (*p && *p != current) {
		(**p).state=0;
		goto repeat;
	}
	*p=NULL;
	if (tmp)
		tmp->state=0;
}

void wake_up(struct task_struct **p)
{
	if (p && *p) { // p: 指向 b_wait 的指针；*p b_wait 的值；**p b_wait指向的task_struct
		(**p).state=0; // 0 即 runnable，TASKRUNNING，改为就绪态
		*p=NULL; // [按理说，应该是 *p=tmp; --> 显示用队列唤醒上一个]，[*p=NULL, 隐式唤醒，等待调度]
	}
}

/*
 * OK, here are some floppy things that shouldn't be in the kernel
 * proper. They are here because the floppy needs a timer, and this
 * was the easiest way of doing it.
 */
static struct task_struct * wait_motor[4] = {NULL,NULL,NULL,NULL};
static int  mon_timer[4]={0,0,0,0};
static int moff_timer[4]={0,0,0,0};
unsigned char current_DOR = 0x0C;

int ticks_to_floppy_on(unsigned int nr)
{
	extern unsigned char selected;
	unsigned char mask = 0x10 << nr;

	if (nr>3)
		panic("floppy_on: nr>3");
	moff_timer[nr]=10000;		/* 100 s = very big :-) */
	cli();				/* use floppy_off to turn it off */
	mask |= current_DOR;
	if (!selected) {
		mask &= 0xFC;
		mask |= nr;
	}
	if (mask != current_DOR) {
		outb(mask,FD_DOR);
		if ((mask ^ current_DOR) & 0xf0)
			mon_timer[nr] = HZ/2;
		else if (mon_timer[nr] < 2)
			mon_timer[nr] = 2;
		current_DOR = mask;
	}
	sti();
	return mon_timer[nr];
}

void floppy_on(unsigned int nr)
{
	cli();
	while (ticks_to_floppy_on(nr))
		sleep_on(nr+wait_motor);
	sti();
}

void floppy_off(unsigned int nr)
{
	moff_timer[nr]=3*HZ;
}

void do_floppy_timer(void)
{
	int i;
	unsigned char mask = 0x10;

	for (i=0 ; i<4 ; i++,mask <<= 1) {
		if (!(mask & current_DOR))
			continue;
		if (mon_timer[i]) {
			if (!--mon_timer[i])
				wake_up(i+wait_motor);
		} else if (!moff_timer[i]) {
			current_DOR &= ~mask;
			outb(current_DOR,FD_DOR);
		} else
			moff_timer[i]--;
	}
}

#define TIME_REQUESTS 64

static struct timer_list {
	long jiffies;
	void (*fn)();
	struct timer_list * next;
} timer_list[TIME_REQUESTS], * next_timer = NULL;

void add_timer(long jiffies, void (*fn)(void))
{
	struct timer_list * p;

	if (!fn)
		return;
	cli();
	if (jiffies <= 0)
		(fn)();
	else {
		for (p = timer_list ; p < timer_list + TIME_REQUESTS ; p++)
			if (!p->fn)
				break;
		if (p >= timer_list + TIME_REQUESTS)
			panic("No more time requests free");
		p->fn = fn;
		p->jiffies = jiffies;
		p->next = next_timer;
		next_timer = p;
		while (p->next && p->next->jiffies < p->jiffies) {
			p->jiffies -= p->next->jiffies;
			fn = p->fn;
			p->fn = p->next->fn;
			p->next->fn = fn;
			jiffies = p->jiffies;
			p->jiffies = p->next->jiffies;
			p->next->jiffies = jiffies;
			p = p->next;
		}
	}
	sti();
}

void do_timer(long cpl)
{
	extern int beepcount;
	extern void sysbeepstop(void);

	if (beepcount)
		if (!--beepcount)
			sysbeepstop();

	if (cpl)
		current->utime++;
	else
		current->stime++;

	if (next_timer) {
		next_timer->jiffies--;
		while (next_timer && next_timer->jiffies <= 0) {
			void (*fn)(void);
			
			fn = next_timer->fn;
			next_timer->fn = NULL;
			next_timer = next_timer->next;
			(fn)();
		}
	}
	if (current_DOR & 0xf0)
		do_floppy_timer();
	if ((--current->counter)>0) return; // 判断时间片是否消减为0
	current->counter=0;
	if (!cpl) return; // current privilege level 只有在3特权级下才能时钟中断切换，0特权级下不能切换
	schedule();
}

int sys_alarm(long seconds)
{
	int old = current->alarm;

	if (old)
		old = (old - jiffies) / HZ;
	current->alarm = (seconds>0)?(jiffies+HZ*seconds):0;
	return (old);
}

int sys_getpid(void)
{
	return current->pid;
}

int sys_getppid(void)
{
	return current->father;
}

int sys_getuid(void)
{
	return current->uid;
}

int sys_geteuid(void)
{
	return current->euid;
}

int sys_getgid(void)
{
	return current->gid;
}

int sys_getegid(void)
{
	return current->egid;
}

int sys_nice(long increment)
{
	if (current->priority-increment>0)
		current->priority -= increment;
	return 0;
}

void sched_init(void)
{
	int i;
	struct desc_struct * p;

	if (sizeof(struct sigaction) != 16)
		// printk
		panic("Struct sigaction MUST be 16 bytes");
	// 设置第一个 TSS0（task state segment） 和 LDT0 ==> 和用户进程开始相关
	set_tss_desc(gdt+FIRST_TSS_ENTRY,&(init_task.task.tss));
	set_ldt_desc(gdt+FIRST_LDT_ENTRY,&(init_task.task.ldt));
	p = gdt+2+FIRST_TSS_ENTRY; // TSS1
	// LDT1\TSS1 之后的63个进程全部初始化
	for(i=1;i<NR_TASKS;i++) {
		task[i] = NULL;
		p->a=p->b=0; // TSS 1..63
		p++;
		p->a=p->b=0; // LDT 1..63
		p++;
	}
/* Clear NT, so that we won't have troubles with that later on */
	__asm__("pushfl ; andl $0xffffbfff,(%esp) ; popfl");
	ltr(0);		// 重要！将TSS挂接到TR寄存器   load task register，指向当前的 tss
	lldt(0);	// 重要！将LDT挂接到LDTR寄存器 load ldt
	outb_p(0x36,0x43);		/* binary, mode 3, LSB/MSB, ch 0 ==> 设置定时器 */
	outb_p(LATCH & 0xff , 0x40);	/* LSB => LATCH：每10ms一次始终中断 */
	outb(LATCH >> 8 , 0x40);	/* MSB */
	set_intr_gate(0x20,&timer_interrupt); //重要！设置时钟中断，进程调度的基础
	outb(inb_p(0x21)&~0x01,0x21); // 允许时钟中断（打开时钟中断相关屏蔽码）
	set_system_gate(0x80,&system_call); // 重要！设置系统调用总入口，进程与内核交互的途径
}
