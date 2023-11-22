/*
 *  linux/kernel/blk_dev/ll_rw.c
 *
 * (C) 1991 Linus Torvalds
 */

/*
 * This handles all read/write requests to block devices
 */
#include <errno.h>
#include <linux/sched.h>
#include <linux/kernel.h>
#include <asm/system.h>

#include "blk.h"

/*
 * The request-struct contains all necessary data
 * to load a nr of sectors into memory
 * 请求结构包含将扇区加载到内存中的所有必要数据
 */
struct request request[NR_REQUEST];

/*
 * used to wait on when there are no free requests
 */
struct task_struct * wait_for_request = NULL;

/* blk_dev_struct is:
 *	do_request-address
 *	next-request
 *  NR_BLK_DEV 0-6
 */
struct blk_dev_struct blk_dev[NR_BLK_DEV] = {
	{ NULL, NULL },		/* no_dev 无设备*/
	{ NULL, NULL },		/* dev mem 内存 ramdisk */
	{ NULL, NULL },		/* dev fd 软驱设备 */
	{ NULL, NULL },		/* dev hd 硬盘设备 */
	{ NULL, NULL },		/* dev ttyx */
	{ NULL, NULL },		/* dev tty */
	{ NULL, NULL }		/* dev lp 打印机设备 */
};

static inline void lock_buffer(struct buffer_head * bh)
{
	cli(); // 关掉的是这个进程的中断，而不是整个计算机的中断
	// NOTE lyq: 这里是 while，思考为什么不是 if? 可能时间很短，硬盘数据还没有读到缓冲区，又切换进程了；另一方面，不止一个在等待硬盘到缓冲区的读取，所以一次 sleep_on 不一定能把事情做完。
	while (bh->b_lock)
		sleep_on(&bh->b_wait); // b_wait 是指 buffer 等待进程队列；【等待所有共享这个 buffer 的进程】
	bh->b_lock=1;
	sti(); // 防止硬件中断，如时钟中断
}

static inline void unlock_buffer(struct buffer_head * bh)
{
	if (!bh->b_lock)
		printk("ll_rw_block.c: buffer not locked\n\r");
	bh->b_lock = 0;
	wake_up(&bh->b_wait);
}

/*
 * add-request adds a request to the linked list.
 * It disables interrupts so that it can muck with the
 * request-lists in peace. req 会加入到链表 dev->current_request 中
 */
static void add_request(struct blk_dev_struct * dev, struct request * req)
{
	struct request * tmp;

	req->next = NULL;
	cli(); // 原子操作，防止写读竞争【防止硬件中断】
	if (req->bh)
		req->bh->b_dirt = 0; // 清脏位，说明 dirty=0 & lock=1，说明这个 request 至少上路了 --> FIXME lyq: 不写回这个 dirty 的 block 吗？
	if (!(tmp = dev->current_request)) { // 如果 current_request 是 NULL，全0 --> kernel/blk_drv/blk.h blk_dev[NR_BLK_DEV] 初始化为 NULL
		dev->current_request = req;
		sti();
		(dev->request_fn)(); // 调用硬盘请求项处理函数，这里是 do_hd_request() 去给硬盘发送读盘命令
		return;
	}
	for ( ; tmp->next ; tmp=tmp->next) // 电梯算法的作用是让磁盘磁头的移动距离最小 --> 有点像冒泡排序
		if ((IN_ORDER(tmp,req) ||
		    !IN_ORDER(tmp,tmp->next)) &&
		    IN_ORDER(req,tmp->next))
			break;
	req->next=tmp->next;
	tmp->next=req;
	sti();
}

static void make_request(int major,int rw, struct buffer_head * bh)
{
	struct request * req;
	int rw_ahead;

/* WRITEA/READA is special case - it is not really needed, so if the */
/* buffer is locked, we just forget about it, else it's a normal read */
// 读比写更迫切【计算机体系结构经典结论：读优先】
	if (rw_ahead = (rw == READA || rw == WRITEA)) { // READA, read ahead, 预读写
		if (bh->b_lock) // bh 加了锁
			return;
		if (rw == READA) // 放弃预读写，改为普通读写
			rw = READ;
		else
			rw = WRITE;
	}
	if (rw!=READ && rw!=WRITE)
		panic("Bad block dev command, must be R/W/RA/WA");
	lock_buffer(bh); // 先加锁，避免被挪作他用 --> 存在写读的竞争/生产者和消费者竞争，就需要加 lock
	if ((rw == WRITE && !bh->b_dirt) || (rw == READ && bh->b_uptodate)) { // 写但非脏，即不需要写hd；读但已经读过了，即不需要再读hd --> 提前终止 buffer->hd req
		unlock_buffer(bh);
		return;
	}
repeat:
/* we don't allow the write-requests to fill up the queue completely:
 * we want some room for reads: they take precedence (n. 领先优先). The last third
 * of the requests are only for reads.
 */
	if (rw == READ)
		req = request+NR_REQUEST; // 读从尾端开始，2/3 - 1 都是读
	else
		req = request+((NR_REQUEST*2)/3); // 写从 2/3 处开始， 0-2/3 写和读
/* find an empty request */
	while (--req >= request) // 从后向前搜索空闲请求项，在 blk_dev_init 中，dev 初始化为-1，即空闲
		if (req->dev<0) // 找到空闲请求项 kernel/blk_drv/blk.h line 27: -1 没有 request
			break;
/* if none found, sleep on new requests: check for rw_ahead */
	if (req < request) {
		if (rw_ahead) { // 预读写 -> 就直接不管了
			unlock_buffer(bh);
			return;
		}
		sleep_on(&wait_for_request); // 非预读写，还是需要等待 buffer request 的 --> 【等待整个 buffer 请求项】
		goto repeat;
	}
/* fill up the request-info, and add it to the queue --> 直接就在 32 个请求项数组中做的，因为之前赋值了 req = request+NR_REQUEST... */
	req->dev = bh->b_dev; // 设置 dev
	req->cmd = rw;
	req->errors=0;
	req->sector = bh->b_blocknr<<1;
	req->nr_sectors = 2; // 512 B * 2 = 1024 B = 1KB
	req->buffer = bh->b_data;
	req->waiting = NULL;
	req->bh = bh;
	req->next = NULL;
	add_request(major+blk_dev,req); // 加载请求项，blk_dev 是那个数组，blk_dev+major 刚好是 hd 那项
}

void ll_rw_block(int rw, struct buffer_head * bh) // 底层（low layer）块设备操作
{
	unsigned int major;

	if ((major=MAJOR(bh->b_dev)) >= NR_BLK_DEV || // 低 8 位对齐并获取高位, 主设备号是 0-6，>=7意味着不存在
	!(blk_dev[major].request_fn)) { // 请求项, hd_init kernel/blk_drv/hd.c 中挂载的是 do_hd_request
		printk("Trying to read nonexistent block-device\n\r"); // print kernel
		return;
	}
	make_request(major,rw,bh);
}

void blk_dev_init(void)
{
	int i;

	for (i=0 ; i<NR_REQUEST ; i++) {
		request[i].dev = -1; // 初始化，全部 no request
		request[i].next = NULL;
	}
}
