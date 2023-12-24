/*
 *  linux/init/main.c
 *
 *  (C) 1991  Linus Torvalds
 */

#define __LIBRARY__
#include <unistd.h>
#include <time.h>

/*
 * we need this inline - forking from kernel space will result
 * in NO COPY ON WRITE (!!!), until an execve is executed. This
 * is no problem, but for the stack. This is handled by not letting
 * main() use the stack at all after fork(). Thus, no function
 * calls - which means inline code for fork too, as otherwise we
 * would use the stack upon exit from 'fork()'.
 *
 * Actually only pause and fork are needed inline, so that there
 * won't be any messing with the stack from main(), but we define
 * some others too.
 */
static inline _syscall0(int,fork)
static inline _syscall0(int,pause)
static inline _syscall1(int,setup,void *,BIOS)
static inline _syscall0(int,sync)

#include <linux/tty.h>
#include <linux/sched.h>
#include <linux/head.h>
#include <asm/system.h>
#include <asm/io.h>

#include <stddef.h>
#include <stdarg.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>

#include <linux/fs.h>

static char printbuf[1024];

extern int vsprintf();
extern void init(void);
extern void blk_dev_init(void);
extern void chr_dev_init(void);
extern void hd_init(void);
extern void floppy_init(void);
extern void mem_init(long start, long end);
extern long rd_init(long mem_start, int length);
extern long kernel_mktime(struct tm * tm);
extern long startup_time;

/*
 * This is set up by the setup-routine at boot-time
 * 放置机器系统数据，0x90002 表示扩展内存（系统从1MB开始的扩展内存数值/KB）
 */
#define EXT_MEM_K (*(unsigned short *)0x90002)
#define DRIVE_INFO (*(struct drive_info *)0x90080)
#define ORIG_ROOT_DEV (*(unsigned short *)0x901FC) // 根文件系统所在设备号

/*
 * Yeah, yeah, it's ugly, but I cannot find how to do this correctly
 * and this seems to work. I anybody has more info on the real-time
 * clock I'd be interested. Most of this was trial and error, and some
 * bios-listing reading. Urghh.
 */

#define CMOS_READ(addr) ({ \
outb_p(0x80|addr,0x70); \
inb_p(0x71); \
})

#define BCD_TO_BIN(val) ((val)=((val)&15) + ((val)>>4)*10)

static void time_init(void)
{
	struct tm time;

	do {
		time.tm_sec = CMOS_READ(0);
		time.tm_min = CMOS_READ(2);
		time.tm_hour = CMOS_READ(4);
		time.tm_mday = CMOS_READ(7);
		time.tm_mon = CMOS_READ(8);
		time.tm_year = CMOS_READ(9);
	} while (time.tm_sec != CMOS_READ(0));
	BCD_TO_BIN(time.tm_sec);
	BCD_TO_BIN(time.tm_min);
	BCD_TO_BIN(time.tm_hour);
	BCD_TO_BIN(time.tm_mday);
	BCD_TO_BIN(time.tm_mon);
	BCD_TO_BIN(time.tm_year);
	time.tm_mon--;
	startup_time = kernel_mktime(&time);
}

static long memory_end = 0;
static long buffer_memory_end = 0;
static long main_memory_start = 0;

struct drive_info { char dummy[32]; } drive_info;

void main(void)		/* This really IS void, no error here. */
{			/* The startup routine assumes (well, ...) this */
/*
 * Interrupts are still disabled. Do necessary setups, then
 * enable them
 */
 	ROOT_DEV = ORIG_ROOT_DEV; // 初始为软盘
 	drive_info = DRIVE_INFO;
	memory_end = (1<<20) + (EXT_MEM_K<<10);
	memory_end &= 0xfffff000;
	// 针对物理内存条的实际大小，对内存进行不同的规划
	if (memory_end > 16*1024*1024)
		memory_end = 16*1024*1024;
	if (memory_end > 12*1024*1024) 
		buffer_memory_end = 4*1024*1024; // 0x3FFFFF
	else if (memory_end > 6*1024*1024)
		buffer_memory_end = 2*1024*1024;
	else
		buffer_memory_end = 1*1024*1024;
	// ｜ 缓冲区 ｜ 主存开始+虚拟盘 ｜ ... | 物理内存末端
	// 缓冲区是硬盘和内存之间的代理
	// 虚拟盘：为了跑得快
	main_memory_start = buffer_memory_end;
#ifdef RAMDISK
	// AKA. rd 虚拟盘设置
	main_memory_start += rd_init(main_memory_start, RAMDISK*1024);
#endif
	mem_init(main_memory_start,memory_end);
	trap_init();
	blk_dev_init();
	chr_dev_init();     // 字符设备，标准输入输出
	tty_init();         // 电传打印机(Teleprinter)
	time_init();        // 系统时钟设置
	sched_init();       // 进程设置+系统调用相关【重要】
	buffer_init(buffer_memory_end); // 普通文件块设备的缓冲区->为了跑得更快
	hd_init();          // 初始化硬盘
	floppy_init();      // 初始化软盘
	sti();              // 因为在 setup.s line 109 关闭了中断
	move_to_user_mode();// 转换特权级 0->3，进程0开始执行，之后的代码均为进程0来执行
    // NOTE lyq: 考题，fork 这里面的 static inline _syscall0(int,fork) inline 去不去掉的区别
	if (!fork()) {		/* we count on this going ok 创建进程 */
		init();
	}
/*
 *   NOTE!!   For any other task 'pause()' would mean we have to get a
 * signal to awaken, but task0 is the sole exception (see 'schedule()')
 * as task 0 gets activated at every idle moment (when no other tasks
 * can run). For task0 'pause()' just means we go check if some other
 * task can run, and if not we return here.
 */
	for(;;) pause();
}

static int printf(const char *fmt, ...)
{
	va_list args;
	int i;

	va_start(args, fmt);
	write(1,printbuf,i=vsprintf(printbuf, fmt, args));
	va_end(args);
	return i;
}

static char * argv_rc[] = { "/bin/sh", NULL };
static char * envp_rc[] = { "HOME=/", NULL };

static char * argv[] = { "-/bin/sh",NULL };
static char * envp[] = { "HOME=/usr/root", NULL };

void init(void)
{
	int pid,i;

	setup((void *) &drive_info);
	(void) open("/dev/tty0",O_RDWR,0); // 创建标准输入设备 --> 调用 sys_open()的方法
	(void) dup(0); // 标准输出设备 --> 复制文件句柄的方法 sys_dup()
	(void) dup(0); // 标准错误输出设备
	printf("%d buffers = %d bytes buffer space\n\r",NR_BUFFERS,
		NR_BUFFERS*BLOCK_SIZE); // 重要！！首次使用 printf，在标准输出设备支持下，显示信息
	printf("Free mem: %d bytes\n\r",memory_end-main_memory_start);
	if (!(pid=fork())) { // NOTE lyq: 所有父进程创建子进程，子进程加载自己的文件的必备流程
		// 这里由子进程 进程2 执行。该子进程关闭了句柄0(stdin)、以只读方式打开 /etc/rc 文件，并使用 execve()函数将进程自身替换成 /bin/sh 程序(即 shell 程序)，然后执行 /bin/sh 程序
		// 函数_exit()退出时的出错码 1 – 操作未许可；2 -- 文件或目录不存在。
		close(0); // 关闭标准输入设备文件
		if (open("/etc/rc",O_RDONLY,0)) // 用 rc 文件替换该设备文件
			_exit(1);
		execve("/bin/sh",argv_rc,envp_rc); // 加载 shell 程序，该程序文件路径为 /bin/sh，argv_rc和envp_rc分别是参数及环境变量
		_exit(2);
	}
	if (pid>0)
		// 这里由父进程 进程1 执行
		// wait() 如果进程1有等待退出的子进程，就为该进程的退出做善后工作；如果有子进程，但并不等待退出，则进程切换；如果没有子进程，则函数返回。
		while (pid != wait(&i)) // pid=2, return flag=2
			/* nothing */;
	while (1) { // 重启 shell 进程
		if ((pid=fork())<0) { // 进程1创建进程4，因为lastpid为4，故pid为4；但申请到的task为task[2]
			printf("Fork failed in init\r\n");
			continue;
		}
		if (!pid) {
			// 子进程执行
			close(0);close(1);close(2); // 新的shell进程关闭所有打开的文件
			setsid(); // 创建新的会话
			(void) open("/dev/tty0",O_RDWR,0); // 重新打开标准输入文件
			(void) dup(0); // std out
			(void) dup(0); // std error
			_exit(execve("/bin/sh",argv,envp)); // 加载 shell 进程
		}
		while (1)
			// 进程1执行
			if (pid == wait(&i)) // 等待子进程退出
				break;
		printf("\n\rchild %d died with code %04x\n\r",pid,i);
		sync();
	}
	_exit(0);	/* NOTE! _exit, not exit() */
}
