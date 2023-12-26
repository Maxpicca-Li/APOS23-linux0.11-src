/*
 *  linux/kernel/system_call.s
 *
 *  (C) 1991  Linus Torvalds
 */

/*
 *  system_call.s  contains the system-call low-level handling routines.
 * This also contains the timer-interrupt handler, as some of the code is
 * the same. The hd- and flopppy-interrupts are also here.
 *
 * NOTE: This code handles signal-recognition, which happens every time
 * after a timer-interrupt and after each system call. Ordinary interrupts
 * don't handle signal-recognition, as that would clutter them up totally
 * unnecessarily.
 *
 * Stack layout in 'ret_from_system_call':
 *
 *	 0(%esp) - %eax
 *	 4(%esp) - %ebx
 *	 8(%esp) - %ecx
 *	 C(%esp) - %edx
 *	10(%esp) - %fs
 *	14(%esp) - %es
 *	18(%esp) - %ds
 *	1C(%esp) - %eip
 *	20(%esp) - %cs
 *	24(%esp) - %eflags
 *	28(%esp) - %oldesp
 *	2C(%esp) - %oldss
 */

SIG_CHLD	= 17

EAX		= 0x00
EBX		= 0x04
ECX		= 0x08
EDX		= 0x0C
FS		= 0x10
ES		= 0x14
DS		= 0x18
EIP		= 0x1C # eip[0]
CS		= 0x20 # eip[1]
EFLAGS		= 0x24 # eip[2]
OLDESP		= 0x28 # eip[3] 当有特权级变化时
OLDSS		= 0x2C # eip[4] old stack segment

state	= 0		# these are offsets into the task-struct.
counter	= 4
priority = 8
signal	= 12
sigaction = 16		# MUST be 16 (=len of sigaction)
blocked = (33*16)

# offsets within sigaction
sa_handler = 0
sa_mask = 4
sa_flags = 8
sa_restorer = 12

# 一共有 72 个 __NR_##name 入口
nr_system_calls = 72

/*
 * Ok, I get parallel printer interrupts while using the floppy for some
 * strange reason. Urgel. Now I just ignore them.
 */
.globl _system_call,_sys_fork,_timer_interrupt,_sys_execve
.globl _hd_interrupt,_floppy_interrupt,_parallel_interrupt
.globl _device_not_available, _coprocessor_error

.align 2
bad_sys_call:
	movl $-1,%eax
	iret
.align 2
reschedule:
	pushl $ret_from_sys_call    # 到时候从 _schedule 返回的时候，就直接执行 ret_from_sys_call
	jmp _schedule
.align 2
_system_call:
	# 1: 判断数组是否越界；2: 拦截不确定性，防止不确定性的系统中断发生，避免其越权
	# 若 eax 超出范围的话就在 eax 中置 -1 并退出
	cmpl $nr_system_calls-1,%eax
	ja bad_sys_call
	push %ds
	push %es
	push %fs
	pushl %edx
	pushl %ecx              # push %ebx,%ecx,%edx as parameters --> --> 函数内用于初始化进程1的TSS
	pushl %ebx              # to the system call
	movl $0x10,%edx         # set up ds,es to kernel space $0x10 为内核数据段
	mov %dx,%ds
	mov %dx,%es
	movl $0x17,%edx         # fs points to local data space 进程0 数据段
	mov %dx,%fs
	# _sys_call_table + %eax * 4, 为 _sys_call_table[%eax] 的物理地址，因为每项4字节
	call _sys_call_table(,%eax,4)
	pushl %eax              # 这里是 sys_fork 中的返回值，此时 eax 是 sys_fork 中调用的 copy_process 中返回的 last_pid
	movl _current,%eax      # _current 进程 0, %%eax = _current，其中 current类型为struct task_struct *。
	cmpl $0,state(%eax)     # 即进程 0 的 task_struct state，0 就绪态
	jne reschedule          # 如果进程 0 未就绪，则进入「进程调度过程」
	cmpl $0,counter(%eax)   # counter，即进程 0 的时间片
	je reschedule           # 如果进程 0 时间片为0，即到时间了，则进入「进程调度过程」
ret_from_sys_call:          # 返回 sys_call
	movl _current,%eax		# task[0] cannot have signals
	cmpl _task,%eax         # task 数组首地址，即进程0 --> 判别当前任务是否是进程 0，如果是则不必对其进行信号量方面的处理，直接返回。
	je 3f
	cmpw $0x0f,CS(%esp)		# was old code segment supervisor ? -> 通过对原调用程序代码不在用户代码段中（例如任务 1），则退出
	jne 3f
	cmpw $0x17,OLDSS(%esp)		# was stack segment = 0x17 ? 如果原堆栈不在用户数据段中，则也退出
	jne 3f
	movl signal(%eax),%ebx
	movl blocked(%eax),%ecx
	notl %ecx
	andl %ebx,%ecx
	bsfl %ecx,%ecx
	je 3f
	btrl %ecx,%ebx
	movl %ebx,signal(%eax)
	incl %ecx
	pushl %ecx
	call _do_signal
	popl %eax
3:	popl %eax               # system_call 之前压的栈，popl 4字节，pop 2字节
	popl %ebx
	popl %ecx
	popl %edx
	pop %fs
	pop %es
	pop %ds
	iret                    # int80 iret，涉及到特权级转换 0->3

.align 2
_coprocessor_error:
	push %ds
	push %es
	push %fs
	pushl %edx
	pushl %ecx
	pushl %ebx
	pushl %eax
	movl $0x10,%eax
	mov %ax,%ds
	mov %ax,%es
	movl $0x17,%eax
	mov %ax,%fs
	pushl $ret_from_sys_call
	jmp _math_error

.align 2
_device_not_available:
	push %ds
	push %es
	push %fs
	pushl %edx
	pushl %ecx
	pushl %ebx
	pushl %eax
	movl $0x10,%eax
	mov %ax,%ds
	mov %ax,%es
	movl $0x17,%eax
	mov %ax,%fs
	pushl $ret_from_sys_call
	clts				# clear TS so that we can use math
	movl %cr0,%eax
	testl $0x4,%eax			# EM (math emulation bit)
	je _math_state_restore
	pushl %ebp
	pushl %esi
	pushl %edi
	call _math_emulate
	popl %edi
	popl %esi
	popl %ebp
	ret

.align 2
_timer_interrupt: # 时钟中断
	push %ds		# save ds,es and put kernel data space
	push %es		# into them. %fs is used by _system_call
	push %fs
	pushl %edx		# we save %eax,%ecx,%edx as gcc doesn't
	pushl %ecx		# save those across function calls. %ebx
	pushl %ebx		# is saved as we use that in ret_sys_call
	pushl %eax
	movl $0x10,%eax
	mov %ax,%ds
	mov %ax,%es
	movl $0x17,%eax
	mov %ax,%fs
	incl _jiffies
	movb $0x20,%al		# EOI to interrupt controller #1
	outb %al,$0x20
	movl CS(%esp),%eax
	andl $3,%eax		# %eax is CPL (0 or 3, 0=supervisor)
	pushl %eax
	call _do_timer		# 'do_timer(long CPL)' does everything from
	addl $4,%esp		# task switching to accounting ...
	jmp ret_from_sys_call

.align 2
_sys_execve:
	lea EIP(%esp),%eax # Load Effective Address
	pushl %eax # eax 压栈，把 EIP 值“所在栈空间的地址值”压栈 #FIXME lyq: 用处是？
	call _do_execve # 调用 do_execve()
	addl $4,%esp # 丢弃调用时压入栈的 EIP 值。
	ret

.align 2
_sys_fork:
# 首先调用 C 函数 find_empty_process()，取得一个进程号 pid。若返回负数则说明目前任务数组已满。若没有满则调用 copy_process()复制进程。
	call _find_empty_process
	testl %eax,%eax # %eax & %eax，检查 %eax 是否为0和负数
	js 1f # 如果 SF 被置位，即%%eax为负数，说明task[]已满。
	push %gs  # 为 copy_process 准备参数 --> 函数内用于初始化进程1的TSS
	pushl %esi
	pushl %edi
	pushl %ebp
	pushl %eax # find_empty_process 的返回值 --> new pid
	call _copy_process
	addl $20,%esp   # 加清栈，减压栈：20 = 4 * 5，5个数，把前面的 gs, esi, edi, ebp, eax丢弃掉，注意 gs 是 2 字节，但是".align 2"会让 gs 对齐增2字节，故总共20字节
1:	ret             # 普通的 ret，而不是 iret，因为没有翻转特权级, 0->0, 返回到 _system_call 的 call _sys_call_table(,%eax,4) 下一句

_hd_interrupt: # 中断会自动压栈 ss, esp, eflags, cs, eip
	pushl %eax # 保存CPU状态
	pushl %ecx
	pushl %edx
	push %ds
	push %es
	push %fs
	movl $0x10,%eax # ds,es 置为内核数据段
	mov %ax,%ds
	mov %ax,%es
	movl $0x17,%eax # fs 置为调用程序的局部数据段
	mov %ax,%fs
	movb $0x20,%al # 由于初始化中断控制芯片时没有采用自动 EOI，所以这里需要发指令结束该硬件中断 end of interrupt
	outb %al,$0xA0		# EOI to interrupt controller #1 发送 EOI 命令到 8259A（从）
	jmp 1f			# give port chance to breathe 延时作用
1:	jmp 1f # 延时作用
1:	xorl %edx,%edx     # 异或，清零
	xchgl _do_hd,%edx  # kernel/blk_drv/hd.c 中 do_hd = intr_addr; 【xchg 是 exchange 的缩写，表示交换两个操作数的值。后缀 l 表示操作的是长字（long），在 32 位架构中，长字通常是 32 位的。】
	testl %edx,%edx # 【testl %edx,%edx 经常用于检查寄存器的值是否为零，作为接下来的条件跳转指令（如 jz，跳转如果零）的依据。】
	jne 1f
	movl $_unexpected_hd_interrupt,%edx # 如果 _do_hd 函数为空，说明有问题
1:	outb %al,$0x20 # 接收 8259A（主） 发送的中断控制器 EOI 指令（结束硬件中断）。
	call *%edx		# "interesting" way of handling intr. --> 调用 intr_addr 函数(这里是 read_intr 函数)
	pop %fs # 弹栈
	pop %es
	pop %ds
	popl %edx
	popl %ecx
	popl %eax
	iret			# 回到进程0继续pause

_floppy_interrupt:
	pushl %eax
	pushl %ecx
	pushl %edx
	push %ds
	push %es
	push %fs
	movl $0x10,%eax
	mov %ax,%ds
	mov %ax,%es
	movl $0x17,%eax
	mov %ax,%fs
	movb $0x20,%al
	outb %al,$0x20		# EOI to interrupt controller #1
	xorl %eax,%eax
	xchgl _do_floppy,%eax
	testl %eax,%eax
	jne 1f
	movl $_unexpected_floppy_interrupt,%eax
1:	call *%eax		# "interesting" way of handling intr.
	pop %fs
	pop %es
	pop %ds
	popl %edx
	popl %ecx
	popl %eax
	iret

_parallel_interrupt:
	pushl %eax
	movb $0x20,%al
	outb %al,$0x20
	popl %eax
	iret
