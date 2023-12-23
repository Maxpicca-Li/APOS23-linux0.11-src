/*
 *  linux/kernel/blk_drv/ramdisk.c
 *
 *  Written by Theodore Ts'o, 12/2/91
 */

#include <string.h>

#include <linux/config.h>
#include <linux/sched.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <asm/system.h>
#include <asm/segment.h>
#include <asm/memory.h>

#define MAJOR_NR 1
#include "blk.h"

char	*rd_start;
int	rd_length = 0;

void do_rd_request(void)
{
	int	len;
	char	*addr;

	INIT_REQUEST;
	addr = rd_start + (CURRENT->sector << 9);
	len = CURRENT->nr_sectors << 9;
	if ((MINOR(CURRENT->dev) != 1) || (addr+len > rd_start+rd_length)) {
		end_request(0);
		goto repeat;
	}
	if (CURRENT-> cmd == WRITE) {
		(void ) memcpy(addr,
			      CURRENT->buffer,
			      len);
	} else if (CURRENT->cmd == READ) {
		(void) memcpy(CURRENT->buffer, 
			      addr,
			      len);
	} else
		panic("unknown ramdisk-command");
	end_request(1);
	goto repeat;
}

/*
 * Returns amount of memory which needs to be reserved.
 */
long rd_init(long mem_start, int length)
{
	int	i;
	char	*cp;

	// blk_dev[1] 对应 ramdisk, 将 do rd request 函数服务程序，挂载到函数指针
	blk_dev[MAJOR_NR].request_fn = DEVICE_REQUEST;
	// memory ramdisk 清零
	rd_start = (char *) mem_start;
	rd_length = length;
	cp = rd_start;
	for (i=0; i < length; i++)
		*cp++ = '\0';
	return(length);
}

/*
 * If the root device is the ram disk, try to load it.
 * In order to do this, the root device is originally set to the
 * floppy, and we later change it to be ram disk.
 */
void rd_load(void)
{
	// step1: 把整个软盘复制到/映射到虚拟盘
	struct buffer_head *bh;
	struct super_block	s;
	int		block = 256;	/* Start at block 256 ， 第256个扇区存放格式化虚拟盘的信息*/
	int		i = 1;
	int		nblocks;
	char		*cp;		/* Move pointer */
	
	if (!rd_length)
		return;
	printk("Ram disk: %d bytes, starting at 0x%x\n", rd_length,
		(int) rd_start);
	if (MAJOR(ROOT_DEV) != 2) // 软盘
		return;
	bh = breada(ROOT_DEV,block+1,block,block+2,-1); // 从软盘上预读，block 引导块；block+1 超级块；block+2 inode 位点图
	if (!bh) {
		printk("Disk error while looking for ramdisk!\n");
		return;
	}
	// FIXME lyq: 引导块不用，你怎么知道我返回的 bh 是超级块？应该是引导块才对。除非引导块的数据类型也是d_super_block
	// NOTE lyq: 备份再及时释放缓冲块的好习惯
	*((struct d_super_block *) &s) = *((struct d_super_block *) bh->b_data); // 备份超级块
	brelse(bh);
	if (s.s_magic != SUPER_MAGIC) // 如果不等，则不是 minix 文件系统
		/* No ram disk image present, assume normal floppy boot */
		return;
	nblocks = s.s_nzones << s.s_log_zone_size; // 根文件系统的数据块数
	if (nblocks > (rd_length >> BLOCK_SIZE_BITS)) { // 数据块数 > 虚拟盘块数
		printk("Ram disk image too big!  (%d blocks, %d avail)\n", 
			nblocks, rd_length >> BLOCK_SIZE_BITS);
		return;
	}
	printk("Loading %d bytes into ram disk... 0000k", 
		nblocks << BLOCK_SIZE_BITS);
	cp = rd_start;
	// 复制数据
	while (nblocks) {
		if (nblocks > 2) 
			bh = breada(ROOT_DEV, block, block+1, block+2, -1);
		else
			bh = bread(ROOT_DEV, block);
		if (!bh) {
			printk("I/O error on block %d, aborting load\n", 
				block);
			return;
		}
		(void) memcpy(cp, bh->b_data, BLOCK_SIZE);
		brelse(bh);
		printk("\010\010\010\010\010%4dk",i);
		cp += BLOCK_SIZE;
		block++;
		nblocks--;
		i++;
	}
	printk("\010\010\010\010\010done \n");
	// step2: 设置设备号，MAJOR(ROOT_DEV) 从 2 变到 1
	ROOT_DEV=0x0101;
}
