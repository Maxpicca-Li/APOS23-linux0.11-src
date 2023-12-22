/*
 *  linux/mm/memory.c
 *
 *  (C) 1991  Linus Torvalds
 */

/*
 * demand-loading started 01.12.91 - seems it is high on the list of
 * things wanted, and it should be easy to implement. - Linus
 */

/*
 * Ok, demand-loading was easy, shared pages a little bit tricker. Shared
 * pages started 02.12.91, seems to work. - Linus.
 *
 * Tested sharing by executing about 30 /bin/sh: under the old kernel it
 * would have taken more than the 6M I have free, but it worked well as
 * far as I could see.
 *
 * Also corrected some "invalidate()"s - I wasn't doing enough of them.
 */

#include <signal.h>

#include <asm/system.h>

#include <linux/sched.h>
#include <linux/head.h>
#include <linux/kernel.h>

volatile void do_exit(long code);

static inline volatile void oom(void)
{
	printk("out of memory\n\r");
	do_exit(SIGSEGV);
}

// 刷新页变换高速缓冲宏函数。 
// 为了提高地址转换的效率，CPU 将最近使用的页表数据存放在芯片中高速缓冲中。在修改过页表信息之后，就需要刷新该缓冲区。这里使用重新加载页目录基址寄存器 cr3 的方法来进行刷新。下面 eax = 0，是页目录的基址。
#define invalidate() \
__asm__("movl %%eax,%%cr3"::"a" (0)) // "a" 赋值 eax = 0

/* these are not to be changed without changing head.s etc */
#define LOW_MEM 0x100000 // 扩展内存对应物理地址的开始地址（多于1MB的内存为扩展内存）
#define PAGING_MEMORY (15*1024*1024)
#define PAGING_PAGES (PAGING_MEMORY>>12) // 0xFFFF00
#define MAP_NR(addr) (((addr)-LOW_MEM)>>12)
#define USED 100

#define CODE_SPACE(addr) ((((addr)+4095)&~4095) < \
current->start_code + current->end_code)

static long HIGH_MEMORY = 0;

#define copy_page(from,to) \
__asm__("cld ; rep ; movsl"::"S" (from),"D" (to),"c" (1024):"cx","di","si")

static unsigned char mem_map [ PAGING_PAGES ] = {0,}; // 物理地址空间，以页为单位进行管理，记录引用计数

/*
 * Get physical address of first (actually last :-) free page, and mark it
 * used. If no free pages left, return 0.
 */
// memmap 从高往低找，内存顶头16MB的往低看，0特权，物理页
unsigned long get_free_page(void)
{
register unsigned long __res asm("ax");

__asm__("std ; repne ; scasb\n\t" // 方向位置位，用于反响扫描 mem_map[], al(0)与di不等则重复（找引用计数为0的项）
	"jne 1f\n\t" // 如果没有等于 0 的字节，即找不到空闲项，则跳转结束（返回 0）。
	"movb $1,1(%%edi)\n\t" // 1->edi+1的内存位置，即mem_map[]中找到的0项，引用计数置1
	"sall $12,%%ecx\n\t" // Shift Arithmetic Left Long，页面数*4K = 相对页面起始地址 --> 相对于 mem_map。
	"addl %2,%%ecx\n\t" // 再加上低端内存地址 LOW_MEM，即获得页面实际物理起始地址。
	"movl %%ecx,%%edx\n\t" // 将页面实际起始地址->edx 寄存器。
	"movl $1024,%%ecx\n\t" // 寄存器 ecx 置计数值 1024。
	"leal 4092(%%edx),%%edi\n\t" // 将 4092+edx 的位置->edi(该页面的末端)。
	"rep ; stosl\n\t" // 将 edi 所指内存清零（反方向，也即将该页面清零）。
	"movl %%edx,%%eax\n" // 将页面起始地址->eax（返回值）
	"1:"
	:"=a" (__res)
	:"0" (0),"i" (LOW_MEM),"c" (PAGING_PAGES),
	"D" (mem_map+PAGING_PAGES-1) // mem_map[]的最后一个元素赋值给edx
	:"di","cx","dx"); // 第3个冒号后是程序中改变过的量
return __res; // 返回空闲页面地址（如果无空闲则返回 0）。
}

/*
 * Free a page of memory at physical address 'addr'. Used by
 * 'free_page_tables()'
 */
void free_page(unsigned long addr)
{
	if (addr < LOW_MEM) return;
	if (addr >= HIGH_MEMORY)
		panic("trying to free nonexistent page");
	addr -= LOW_MEM;
	addr >>= 12;
	if (mem_map[addr]--) return;
	mem_map[addr]=0;
	panic("trying to free free page");
}

/*
 * This function frees a continuos block of page tables, as needed
 * by 'exit()'. As does copy_page_tables(), this handles only 4Mb blocks.
 */
int free_page_tables(unsigned long from,unsigned long size)
{
	unsigned long *pg_table;
	unsigned long * dir, nr;

	if (from & 0x3fffff)
		panic("free_page_tables called with wrong alignment");
	if (!from)
		panic("Trying to free up swapper memory space");
	size = (size + 0x3fffff) >> 22;
	dir = (unsigned long *) ((from>>20) & 0xffc); /* _pg_dir = 0 */
	for ( ; size-->0 ; dir++) {
		if (!(1 & *dir))
			continue;
		pg_table = (unsigned long *) (0xfffff000 & *dir);
		for (nr=0 ; nr<1024 ; nr++) {
			if (1 & *pg_table)
				free_page(0xfffff000 & *pg_table);
			*pg_table = 0;
			pg_table++;
		}
		free_page(0xfffff000 & *dir);
		*dir = 0;
	}
	invalidate();
	return 0;
}

/*
 *  Well, here is one of the most complicated functions in mm. It
 * copies a range of linerar addresses by copying only the pages.
 * Let's hope this is bug-free, 'cause this one I don't want to debug :-)
 *
 * Note! We don't copy just any chunks of memory - addresses have to
 * be divisible by 4Mb (one page-directory entry), as this makes the
 * function easier. It's used only by fork anyway.
 *
 * NOTE 2!! When from==0 we are copying kernel space for the first
 * fork(). Then we DONT want to copy a full page-directory entry, as
 * that would lead to some serious memory waste - we just copy the
 * first 160 pages - 640kB. Even that is more than we need, but it
 * doesn't take any more memory - we don't copy-on-write in the low
 * 1 Mb-range, so the pages can be shared with the kernel. Thus the
 * special case for nr=xxxx.
 */
// NOTE lyq: 必考题，复制页表
int copy_page_tables(unsigned long from,unsigned long to,long size)
{
	unsigned long * from_page_table;
	unsigned long * to_page_table;
	unsigned long this_page;
	unsigned long * from_dir, * to_dir;
	unsigned long nr;

	// 20+2 = 22, 4MB地址空间的最后一个地址，这个大小刚好是页目录表的一个表项的管辖范围 / 一张页表的管辖范围
	// 保证 from/to 的低 22 位全为 0, 即保证地址 4MB 对齐 --> CPU 要求页表对齐
	if ((from&0x3fffff) || (to&0x3fffff))
		panic("copy_page_tables called with wrong alignment");
	// 获取页目录地址 0xffc, 0b1111-1111-1100，即抹掉这12位的低2位，因为一个页目录表项是4B，故这里的 from_dir 表示页目录表项的位置
	from_dir = (unsigned long *) ((from>>20) & 0xffc); /* _pg_dir = 0, pg dir base 为 0*/
	to_dir = (unsigned long *) ((to>>20) & 0xffc);
	// size + 0x3fffff 向上取整； >> 22 得到 size 需要分配多少个页表项，不满一个页表项，按1个页表项算
	size = ((unsigned) (size+0x3fffff)) >> 22;
	/* 页表项 */
	for( ; size-->0 ; from_dir++,to_dir++) {
		// 判断 to_dir 的低 1 位，该位表示存在位 valid
		if (1 & *to_dir) // to_dir 存在，但这时不应该存在，因为子项还没有分配
			panic("copy_page_tables: already exist");
		// 判断父进程是否给自己分配内存
		if (!(1 & *from_dir)) // from_dir 不存在
			continue;
		// 赋值父进程表项
		from_page_table = (unsigned long *) (0xfffff000 & *from_dir);
		// get_free_page，申请页，用于存放页表
		if (!(to_page_table = (unsigned long *) get_free_page()))
			return -1;	/* Out of memory, see freeing */
		*to_dir = ((unsigned long) to_page_table) | 7;  // 7 (user/rw/存在) 确定权限
		nr = (from==0)?0xA0:1024; // 如果为进程0(data_base=0x0)，则分配 160项，640KB；如果不是，则分配 1024项，4 MB --> 因为进程0限长 160；不是的话，就会把一整张页表都给子进程
		/* 页表：循环空间：每一个页表 4KB 的地址拷贝 */
		for ( ; nr-- > 0 ; from_page_table++,to_page_table++) {
			this_page = *from_page_table; // 赋值父进程的页表项
			if (!(1 & this_page)) // present位为1，存在则拷贝
				continue;
			// 因为所有的共享，非数据所有者，只有只读权限，所以pte[1]=r/w=0。如果要写，就只能 copy on write,写时复制 COW
			this_page &= ~2;                        // 除了010即第二位，其他全部保留, 即 this page 只读
			*to_page_table = this_page;             // 赋值给子进程
			// NOTE lyq: 这里进程 0 page 640KB，没有到 1MB，所以进程 0 不会进入 if 内部，仍然拥有写权限。
			if (this_page > LOW_MEM) {              // 如果 this_page < LOW_MEM, 即1MB以内的内存，不参与mem_map管理，因为 mem_map只管理扩展内存
				*from_page_table = this_page;       // 对于共享的内存，无论是父进程还是子进程，都不再拥有写权限（要不然父进程写了，会改变子进程的数据）
				this_page -= LOW_MEM;               // 从 LOW_MEM 开始算起，即从 1MB 以外的内存从0开始计数
				this_page >>= 12;                   // mem_map，以页为单位，所以 >>=12
				mem_map[this_page]++;
			}
		}
	}
	// 刷新 CR3 页目录项寄存器--> 刷新TLB
	invalidate();
	return 0;
}

/*
 * This function puts a page in memory at the wanted address.
 * It returns the physical address of the page gotten, 0 if
 * out of memory (either when trying to access page-table or
 * page.)
 */
unsigned long put_page(unsigned long page,unsigned long address)
{
	unsigned long tmp, *page_table;

/* NOTE !!! This uses the fact that _pg_dir=0 */

	if (page < LOW_MEM || page >= HIGH_MEMORY)
		printk("Trying to put page %p at %p\n",page,address);
	if (mem_map[(page-LOW_MEM)>>12] != 1)
		printk("mem_map disagrees with %p at %p\n",page,address);
	page_table = (unsigned long *) ((address>>20) & 0xffc);
	if ((*page_table)&1)
		page_table = (unsigned long *) (0xfffff000 & *page_table);
	else {
		if (!(tmp=get_free_page()))
			return 0;
		*page_table = tmp|7;
		page_table = (unsigned long *) tmp;
	}
	page_table[(address>>12) & 0x3ff] = page | 7;
/* no need for invalidate */
	return page;
}

void un_wp_page(unsigned long * table_entry)
{
	unsigned long old_page,new_page;

	old_page = 0xfffff000 & *table_entry;
	if (old_page >= LOW_MEM && mem_map[MAP_NR(old_page)]==1) {
		*table_entry |= 2;
		invalidate();
		return;
	}
	if (!(new_page=get_free_page()))
		oom();
	if (old_page >= LOW_MEM)
		mem_map[MAP_NR(old_page)]--;
	*table_entry = new_page | 7;
	invalidate();
	copy_page(old_page,new_page);
}	

/*
 * This routine handles present pages, when users try to write
 * to a shared page. It is done by copying the page to a new address
 * and decrementing the shared-page counter for the old page.
 *
 * If it's in code space we exit with a segment error.
 */
void do_wp_page(unsigned long error_code,unsigned long address)
{
#if 0
/* we cannot do this yet: the estdio library writes to code space */
/* stupid, stupid. I really want the libc.a from GNU */
	if (CODE_SPACE(address))
		do_exit(SIGSEGV);
#endif
	un_wp_page((unsigned long *)
		(((address>>10) & 0xffc) + (0xfffff000 &
		*((unsigned long *) ((address>>20) &0xffc)))));

}

void write_verify(unsigned long address)
{
	unsigned long page;

	if (!( (page = *((unsigned long *) ((address>>20) & 0xffc)) )&1))
		return;
	page &= 0xfffff000;
	page += ((address>>10) & 0xffc);
	if ((3 & *(unsigned long *) page) == 1)  /* non-writeable, present */
		un_wp_page((unsigned long *) page);
	return;
}

void get_empty_page(unsigned long address)
{
	unsigned long tmp;

	if (!(tmp=get_free_page()) || !put_page(tmp,address)) {
		free_page(tmp);		/* 0 is ok - ignored */
		oom();
	}
}

/*
 * try_to_share() checks the page at address "address" in the task "p",
 * to see if it exists, and if it is clean. If so, share it with the current
 * task.
 *
 * NOTE! This assumes we have checked that p != current, and that they
 * share the same executable.
 */
static int try_to_share(unsigned long address, struct task_struct * p)
{
	unsigned long from;
	unsigned long to;
	unsigned long from_page;
	unsigned long to_page;
	unsigned long phys_addr;

	from_page = to_page = ((address>>20) & 0xffc);
	from_page += ((p->start_code>>20) & 0xffc);
	to_page += ((current->start_code>>20) & 0xffc);
/* is there a page-directory at from? */
	from = *(unsigned long *) from_page;
	if (!(from & 1))
		return 0;
	from &= 0xfffff000;
	from_page = from + ((address>>10) & 0xffc);
	phys_addr = *(unsigned long *) from_page;
/* is the page clean and present? */
	if ((phys_addr & 0x41) != 0x01)
		return 0;
	phys_addr &= 0xfffff000;
	if (phys_addr >= HIGH_MEMORY || phys_addr < LOW_MEM)
		return 0;
	to = *(unsigned long *) to_page;
	if (!(to & 1))
		if (to = get_free_page())
			*(unsigned long *) to_page = to | 7;
		else
			oom();
	to &= 0xfffff000;
	to_page = to + ((address>>10) & 0xffc);
	if (1 & *(unsigned long *) to_page)
		panic("try_to_share: to_page already exists");
/* share them: write-protect */
	*(unsigned long *) from_page &= ~2;
	*(unsigned long *) to_page = *(unsigned long *) from_page;
	invalidate();
	phys_addr -= LOW_MEM;
	phys_addr >>= 12;
	mem_map[phys_addr]++;
	return 1;
}

/*
 * share_page() tries to find a process that could share a page with
 * the current one. Address is the address of the wanted page relative
 * to the current data space.
 *
 * We first check if it is at all feasible by checking executable->i_count.
 * It should be >1 if there are other tasks sharing this inode.
 */
static int share_page(unsigned long address)
{
	struct task_struct ** p;

	if (!current->executable)
		return 0;
	if (current->executable->i_count < 2)
		return 0;
	for (p = &LAST_TASK ; p > &FIRST_TASK ; --p) {
		if (!*p)
			continue;
		if (current == *p)
			continue;
		if ((*p)->executable != current->executable)
			continue;
		if (try_to_share(address,*p))
			return 1;
	}
	return 0;
}

void do_no_page(unsigned long error_code,unsigned long address)
{
	int nr[4];
	unsigned long tmp;
	unsigned long page;
	int block,i;

	address &= 0xfffff000; // 取其所在页的起始地址
	tmp = address - current->start_code; // 相较于代码段起始地址的偏移
	if (!current->executable || tmp >= current->end_data) {
		get_empty_page(address);
		return;
	}
	if (share_page(tmp))
		return;
	if (!(page = get_free_page()))
		oom(); // out of memory 内存不够用
/* remember that 1 block is used for header */
	block = 1 + tmp/BLOCK_SIZE; // 块号对应，之所以加一，是之前文件头读了一个数据块，start_code是在读了这个数据块之后才设置的
	for (i=0 ; i<4 ; block++,i++) // 1块 1KB，1页 4KB
		nr[i] = bmap(current->executable,block); // 获取 inode 开头第 block 的块数据
	bread_page(page,current->executable->i_dev,nr);
	i = tmp + 4096 - current->end_data;
	tmp = page + 4096;
	while (i-- > 0) {
		tmp--;
		*(char *)tmp = 0;
	}
	if (put_page(page,address))
		return;
	free_page(page);
	oom();
	// 2023.12.18 17:20 完结撒花
}

void mem_init(long start_mem, long end_mem)
{
	int i;

	HIGH_MEMORY = end_mem;
	for (i=0 ; i<PAGING_PAGES ; i++)
		mem_map[i] = USED; // 使用的数量，USED 默认设置为100，一般进程有 64，不可能到100，说明这块数据不允许人再申请
	i = MAP_NR(start_mem);
	end_mem -= start_mem;
	end_mem >>= 12; // end_mem 目前表示 page 的数量
	while (end_mem-->0)
		mem_map[i++]=0;
}

void calc_mem(void)
{
	int i,j,k,free=0;
	long * pg_tbl;

	for(i=0 ; i<PAGING_PAGES ; i++)
		if (!mem_map[i]) free++;
	printk("%d pages free (of %d)\n\r",free,PAGING_PAGES);
	for(i=2 ; i<1024 ; i++) {
		if (1&pg_dir[i]) {
			pg_tbl=(long *) (0xfffff000 & pg_dir[i]);
			for(j=k=0 ; j<1024 ; j++)
				if (pg_tbl[j]&1)
					k++;
			printk("Pg-dir[%d] uses %d pages\n",i,k);
		}
	}
}
