/*
 *  linux/fs/buffer.c
 *
 *  (C) 1991  Linus Torvalds
 */

/*
 *  'buffer.c' implements the buffer-cache functions. Race-conditions have
 * been avoided by NEVER letting a interrupt change a buffer (except for the
 * data, of course), but instead letting the caller do it. NOTE! As interrupts
 * can wake up a caller, some cli-sti sequences are needed to check for
 * sleep-on-calls. These should be extremely quick, though (I hope).
 */

/*
 * NOTE! There is one discordant note here: checking floppies for
 * disk change. This is where it fits best, I think, as it should
 * invalidate changed floppy-disk-caches.
 */

#include <stdarg.h>
 
#include <linux/config.h>
#include <linux/sched.h>
#include <linux/kernel.h>
#include <asm/system.h>
#include <asm/io.h>

extern int end; // 内核代码末端地址（在内核模块连接期间设置）
// 管理 buffer
struct buffer_head * start_buffer = (struct buffer_head *) &end;
// 本身 buffer 给了 307 项
struct buffer_head * hash_table[NR_HASH];
// 记录 free 的 buffer
static struct buffer_head * free_list;
static struct task_struct * buffer_wait = NULL;
int NR_BUFFERS = 0;

static inline void wait_on_buffer(struct buffer_head * bh)
{
	cli(); // 原子操作
	while (bh->b_lock) // 重要！！可能考：为什么这里用 while 而不是 if：可能出现很多进程都在等待一个缓冲块。在缓冲块同步完毕，唤醒各等待进程到轮转到某一进程的过程中，很有可能此时的缓冲块又被其它进程所占用，并被加上了锁。此时如果用if()，则此进程会从之前被挂起的地方继续执行，不会再判断是否缓冲块已被占用而直接使用，就会出现错误；而如果用while()，则此进程会再次确认缓冲块是否已被占用，在确认未被占用后，才会使用，这样就不会发生之前那样的错误。【在 make_request 处加的锁】
		sleep_on(&bh->b_wait); // b_wait 目前等的进程
	sti();
}

int sys_sync(void)
{
	int i;
	struct buffer_head * bh;

	sync_inodes();		/* write out inodes into buffers */
	bh = start_buffer;
	for (i=0 ; i<NR_BUFFERS ; i++,bh++) {
		wait_on_buffer(bh);
		if (bh->b_dirt)
			ll_rw_block(WRITE,bh); // 写设备
	}
	return 0;
}

int sync_dev(int dev)
{
	int i;
	struct buffer_head * bh;

	bh = start_buffer;
	for (i=0 ; i<NR_BUFFERS ; i++,bh++) {
		if (bh->b_dev != dev)
			continue;
		wait_on_buffer(bh);
		if (bh->b_dev == dev && bh->b_dirt)
			ll_rw_block(WRITE,bh);
	}
	sync_inodes();
	bh = start_buffer;
	for (i=0 ; i<NR_BUFFERS ; i++,bh++) {
		if (bh->b_dev != dev)
			continue;
		wait_on_buffer(bh);
		if (bh->b_dev == dev && bh->b_dirt)
			ll_rw_block(WRITE,bh);
	}
	return 0;
}

void inline invalidate_buffers(int dev)
{
	int i;
	struct buffer_head * bh;

	bh = start_buffer;
	for (i=0 ; i<NR_BUFFERS ; i++,bh++) {
		if (bh->b_dev != dev)
			continue;
		wait_on_buffer(bh);
		if (bh->b_dev == dev)
			bh->b_uptodate = bh->b_dirt = 0;
	}
}

/*
 * This routine checks whether a floppy has been changed, and
 * invalidates all buffer-cache-entries in that case. This
 * is a relatively slow routine, so we have to try to minimize using
 * it. Thus it is called only upon a 'mount' or 'open'. This
 * is the best way of combining speed and utility, I think.
 * People changing diskettes in the middle of an operation deserve
 * to loose :-)
 *
 * NOTE! Although currently this is only for floppies, the idea is
 * that any additional removable block-device will use this routine,
 * and that mount/open needn't know that floppies/whatever are
 * special.
 */
void check_disk_change(int dev)
{
	int i;

	if (MAJOR(dev) != 2)
		return;
	if (!floppy_change(dev & 0x03))
		return;
	for (i=0 ; i<NR_SUPER ; i++)
		if (super_block[i].s_dev == dev)
			put_super(super_block[i].s_dev);
	invalidate_inodes(dev);
	invalidate_buffers(dev);
}

#define _hashfn(dev,block) (((unsigned)(dev^block))%NR_HASH) // 哈希函数
#define hash(dev,block) hash_table[_hashfn(dev,block)]

static inline void remove_from_queues(struct buffer_head * bh) // queue 数据结构，脱钩操作，主要是 prev 和 next 的变化
{
/* remove from hash-queue */
	if (bh->b_next)
		bh->b_next->b_prev = bh->b_prev;
	if (bh->b_prev)
		bh->b_prev->b_next = bh->b_next;
	if (hash(bh->b_dev,bh->b_blocknr) == bh)
		hash(bh->b_dev,bh->b_blocknr) = bh->b_next; // 注意，这里的 b_dev是上家，b_blocknr也是上家，因为 bh要去做空闲块了，如果之前在hash_table[]里面，则需要交代后事
/* remove from free list */
	if (!(bh->b_prev_free) || !(bh->b_next_free))
		panic("Free block list corrupted");
	bh->b_prev_free->b_next_free = bh->b_next_free;
	bh->b_next_free->b_prev_free = bh->b_prev_free;
	if (free_list == bh) // 头部处理
		free_list = bh->b_next_free;
}

static inline void insert_into_queues(struct buffer_head * bh) // queue 数据结构，插入操作，主要是 prev 和 next 的变化
{
/* put at end of free list */
	bh->b_next_free = free_list;
	bh->b_prev_free = free_list->b_prev_free;
	free_list->b_prev_free->b_next_free = bh;
	free_list->b_prev_free = bh;
/* put the buffer in new hash-queue if it has a device */
	bh->b_prev = NULL;
	bh->b_next = NULL;
	if (!bh->b_dev)
		return;
	bh->b_next = hash(bh->b_dev,bh->b_blocknr);
	hash(bh->b_dev,bh->b_blocknr) = bh;
	bh->b_next->b_prev = bh;
}

static struct buffer_head * find_buffer(int dev, int block)
{		
	struct buffer_head * tmp;

	for (tmp = hash(dev,block) ; tmp != NULL ; tmp = tmp->b_next)
		if (tmp->b_dev==dev && tmp->b_blocknr==block) // 查找缓冲区中是否有指定设备号、块号的缓冲块。如果能找到指定缓冲块，就直接用。
			return tmp;
	return NULL;
}

/*
 * Why like this, I hear you say... The reason is race-conditions.
 * As we don't lock buffers (unless we are readint them, that is),
 * something might happen to it while we sleep (ie a read-error
 * will force it bad). This shouldn't really happen currently, but
 * the code is ready.
 * 只看红叉部分
 */
struct buffer_head * get_hash_table(int dev, int block)
{
	struct buffer_head * bh;

	for (;;) {
		if (!(bh=find_buffer(dev,block)))
			return NULL;
		bh->b_count++; // 引用计数
		wait_on_buffer(bh);
		if (bh->b_dev == dev && bh->b_blocknr == block)
			return bh;
		bh->b_count--;
	}
}

/*
 * Ok, this is getblk, and it isn't very clear, again to hinder
 * race-conditions. Most of the code is seldom used, (ie repeating),
 * so it should be much more efficient than it looks.
 *
 * The algoritm is changed: hopefully better, and an elusive bug removed.
 */
#define BADNESS(bh) (((bh)->b_dirt<<1)+(bh)->b_lock) // dirty 的权重更大，更 Bad
struct buffer_head * getblk(int dev,int block)
{
	struct buffer_head * tmp, * bh;

repeat:
	if (bh = get_hash_table(dev,block)) // 先找现有的：查找哈希表，检索此前是否有程序把现在要读的硬盘逻辑块（相同的设备号和块号）已经读到缓冲区
		return bh;
	tmp = free_list; // buffer_head linked list 表头
	do {
		if (tmp->b_count) // 找 count 为 0 的 --> 即空闲块
			continue;
		// dirty + lock时，优先选择只有 lock，因为 lock 是指 queue 正在操作，等待的时间更短；dirty还不知道什么时候操作 
		if (!bh || BADNESS(tmp)<BADNESS(bh)) {
			bh = tmp; // 1. 初始化生成 // 2. 新的 freelist --> TODO: 没有挂 dev&block number，没有 count + 1，也没有按照 dev/block number 的 hash value 挂到 hash_table 上
			if (!BADNESS(tmp))
				break;
		}
/* and repeat until we find something good */
	} while ((tmp = tmp->b_next_free) != free_list); // 环链表，判尾
	if (!bh) { // 如果 bh 还是 NULL，只有sleep_on了
		sleep_on(&buffer_wait);
		goto repeat;
	}
	wait_on_buffer(bh); //等待解锁
	if (bh->b_count) // 再次检查
		goto repeat;
	while (bh->b_dirt) { // 修改过，就要同步 bh 到其 (dev, block)上，然后再使用这个新块 -> 类似cache miss replace drity --> 找到空闲缓冲块，但b_dirt为1，则缓冲区无可用缓冲块，需要同步腾空
		sync_dev(bh->b_dev);
		wait_on_buffer(bh);
		if (bh->b_count)
			goto repeat;
	}
/* NOTE!! While we slept waiting for this block, somebody else might */
/* already have added "this" block to the cache. check it */
	if (find_buffer(dev,block)) // 因为以上只是拿到了一个空闲缓冲区，以下就要去那硬盘数据(dev, block)了 严谨
		goto repeat;
/* OK, FINALLY we know that this buffer is the only one of it's kind, */
/* and that it's unused (b_count=0), unlocked (b_lock=0), and clean */
	bh->b_count=1; // 更新 buffer 信息
	bh->b_dirt=0;
	bh->b_uptodate=0;
	remove_from_queues(bh); // remove from free list
	bh->b_dev=dev;
	bh->b_blocknr=block;
	insert_into_queues(bh); // insert into hash table
	return bh;
}

// 释放引导块，只减少引用计数，不清除数据，以备将来可能的使用
void brelse(struct buffer_head * buf)
{
	if (!buf)
		return;
	wait_on_buffer(buf);
	if (!(buf->b_count--))
		panic("Trying to free free buffer");
	wake_up(&buffer_wait);
}

/*
 * bread() reads a specified block and returns the buffer that contains
 * it. It returns NULL if the block was unreadable.
 * 读指定 dev, block, 第一块硬盘的 dev 是 0x300, block 是 0
 */
struct buffer_head * bread(int dev,int block) //返回 buffer head
{
	struct buffer_head * bh;

	if (!(bh=getblk(dev,block))) // 在缓冲区中得到与 dev, block 相符合或空闲的缓冲块
		panic("bread: getblk returned NULL\n"); // 使用缓冲区，不可能没有空闲块，没有它会一直等着
	if (bh->b_uptodate)
		return bh;
	// ⬆ 缓冲区 || ⬇ 请求项
	ll_rw_block(READ,bh); // low floor
	wait_on_buffer(bh); // 等待缓冲块解锁的进程挂起
	if (bh->b_uptodate)   // 读完了后，返回 bh
		return bh;
	brelse(bh);
	return NULL;
}

#define COPYBLK(from,to) \
__asm__("cld\n\t" \
	"rep\n\t" \
	"movsl\n\t" \
	::"c" (BLOCK_SIZE/4),"S" (from),"D" (to) \
	:"cx","di","si")

/*
 * bread_page reads four buffers into memory at the desired address. It's
 * a function of its own, as there is some speed to be got by reading them
 * all at the same time, not waiting for one to be read, and then another
 * etc.
 */
void bread_page(unsigned long address,int dev,int b[4])
{
	struct buffer_head * bh[4];
	int i;

	for (i=0 ; i<4 ; i++)
		if (b[i]) {
			if (bh[i] = getblk(dev,b[i]))
				if (!bh[i]->b_uptodate)
					ll_rw_block(READ,bh[i]);
		} else
			bh[i] = NULL;
	for (i=0 ; i<4 ; i++,address += BLOCK_SIZE)
		if (bh[i]) {
			wait_on_buffer(bh[i]);
			if (bh[i]->b_uptodate)
				COPYBLK((unsigned long) bh[i]->b_data,address);
			brelse(bh[i]);
		}
}

/*
 * Ok, breada can be used as bread, but additionally to mark other
 * blocks for reading as well. End the argument list with a negative
 * number.
 */
struct buffer_head * breada(int dev,int first, ...)
{
	va_list args;
	struct buffer_head * bh, *tmp;

	va_start(args,first);
	if (!(bh=getblk(dev,first)))
		panic("bread: getblk returned NULL\n");
	if (!bh->b_uptodate)
		ll_rw_block(READ,bh);
	// 然后顺序取可变参数表中其它预读块号，并作与上面同样处理，但不引用（即随时可以被替换，预取块这么没有地位吗，哭唧唧）。注意第二个 ll_rw_block 有一个 bug。 其中的 bh 应该是 tmp。这个 bug 直到在 0.96 版的内核代码中才被纠正过来【预取内容不考】
	while ((first=va_arg(args,int))>=0) {
		tmp=getblk(dev,first);
		if (tmp) {
			if (!tmp->b_uptodate)
				ll_rw_block(READA,bh); 
			tmp->b_count--;
		}
	}
	// 可变参数表中所有参数处理完毕。等待第 1 个缓冲区解锁（如果已被上锁）。【预取内容不考】
	va_end(args);
	wait_on_buffer(bh);
	if (bh->b_uptodate)
		return bh;
	brelse(bh);
	return (NULL);
}

void buffer_init(long buffer_end)
{
	struct buffer_head * h = start_buffer; // head
	void * b; // behind
	int i;

	if (buffer_end == 1<<20) // 1MB 0x9FFFF~0xFFFFF 为 ROMBIOS & VGA数据
		b = (void *) (640*1024); // 0xA0000, 0x9FFFF+1 = 0xA0000, b指向缓冲区外边缘
	else
		b = (void *) buffer_end;
	while ( (b -= BLOCK_SIZE)  >= ((void *) (h+1)) ) { // 每次处理一对（buffer_head, 缓冲块），忽略剩余不足一对的空间
		h->b_dev = 0;
		h->b_dirt = 0;
		h->b_count = 0;
		h->b_lock = 0;
		h->b_uptodate = 0;
		h->b_wait = NULL;
		h->b_next = NULL; // next, prev 后续将与hash_table挂接
		h->b_prev = NULL;
		h->b_data = (char *) b; // 建立数据指向
		// 设置 hash_table 前后项的环链
		h->b_prev_free = h-1; // bufferIdx - 1，与前一个buffer_head挂接
		h->b_next_free = h+1; // bufferIdx + 1，与后一个buffer_head挂接，形成双向链表
		h++;
		NR_BUFFERS++;
		if (b == (void *) 0x100000) // 同开头的判断
			b = (void *) 0xA0000;
	}
	h--; // 刚好是最后一个
	free_list = start_buffer;
	free_list->b_prev_free = h; // 指向最后一个
	h->b_next_free = free_list; // 指向第一个，二者组成双向环链表
	for (i=0;i<NR_HASH;i++)
		hash_table[i]=NULL; // 初始化 hash_table
}	
