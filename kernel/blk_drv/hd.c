/*
 *  linux/kernel/hd.c
 *
 *  (C) 1991  Linus Torvalds
 */

/*
 * This is the low-level hd interrupt support. It traverses the
 * request-list, using interrupts to jump between functions. As
 * all the functions are called within interrupts, we may not
 * sleep. Special care is recommended.
 * 
 *  modified by Drew Eckhardt to check nr of hd's from the CMOS.
 */

#include <linux/config.h>
#include <linux/sched.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/hdreg.h>
#include <asm/system.h>
#include <asm/io.h>
#include <asm/segment.h>

#define MAJOR_NR 3
#include "blk.h"

#define CMOS_READ(addr) ({ \
outb_p(0x80|addr,0x70); \
inb_p(0x71); \
})

/* Max read/write errors/sector */
#define MAX_ERRORS	7
#define MAX_HD		2

static void recal_intr(void);

static int recalibrate = 1;
static int reset = 1;

/*
 *  This struct defines the HD's and their types.
 * 各字段分别是磁头数、每磁道扇区数、柱面数、写前预补偿柱面号、磁头着陆区柱面号、控制字节。
 */
struct hd_i_struct {
	int head,sect,cyl,wpcom,lzone,ctl;
	};
#ifdef HD_TYPE
struct hd_i_struct hd_info[] = { HD_TYPE };
#define NR_HD ((sizeof (hd_info))/(sizeof (struct hd_i_struct)))
#else
struct hd_i_struct hd_info[] = { {0,0,0,0,0,0},{0,0,0,0,0,0} };
static int NR_HD = 0;
#endif

static struct hd_struct {
	long start_sect; // 起始扇区号
	long nr_sects; // 总扇区数
} hd[5*MAX_HD]={{0,0},}; // 定义硬盘分区结构

#define port_read(port,buf,nr) \
__asm__("cld;rep;insw"::"d" (port),"D" (buf),"c" (nr):"cx","di")

#define port_write(port,buf,nr) \
__asm__("cld;rep;outsw"::"d" (port),"S" (buf),"c" (nr):"cx","si")

extern void hd_interrupt(void);
extern void rd_load(void);

/* This may be used only once, enforced by 'static int callable' */
int sys_setup(void * BIOS)
{
	// 硬盘设置
	static int callable = 1;
	int i,drive;
	unsigned char cmos_disks;
	struct partition *p;
	struct buffer_head * bh;

	if (!callable) // 控制只调用一次
		return -1;
	callable = 0;
#ifndef HD_TYPE
	for (drive=0 ; drive<2 ; drive++) { // 读取 drive_info，设置 hd_info, BIOS 就是 drive_info
		hd_info[drive].cyl = *(unsigned short *) BIOS; // 柱面数
		hd_info[drive].head = *(unsigned char *) (2+BIOS); // 磁头数
		hd_info[drive].wpcom = *(unsigned short *) (5+BIOS); // 写前预补偿柱面号
		hd_info[drive].ctl = *(unsigned char *) (8+BIOS); // 控制字节
		hd_info[drive].lzone = *(unsigned short *) (12+BIOS); // 磁头着陆区柱面号
		hd_info[drive].sect = *(unsigned char *) (14+BIOS); // 每磁道扇区数
		BIOS += 16;
	}
	if (hd_info[1].cyl) // 判断有几个磁盘 --> // TODO lyq: 如何根据柱面数判断磁盘数
		NR_HD=2;
	else
		NR_HD=1;
#endif
	for (i=0 ; i<NR_HD ; i++) { //一个物理硬盘最多可以分4个逻辑盘，故每个盘有5个分区，0是物理盘，1～4是逻辑盘；其中 5 的倍数处的项（例如 hd[0]和 hd[5]等）代表整个硬盘中的参数-->因为每个物理硬盘的0号块，为引导块，有分区信息
		hd[i*5].start_sect = 0;
		hd[i*5].nr_sects = hd_info[i].head* // 扇区数
				hd_info[i].sect*hd_info[i].cyl;
	}

	/*
		We querry CMOS about hard disks : it could be that 
		we have a SCSI/ESDI/etc controller that is BIOS
		compatable with ST-506, and thus showing up in our
		BIOS table, but not register compatable, and therefore
		not present in CMOS.

		Furthurmore, we will assume that our ST-506 drives
		<if any> are the primary drives in the system, and 
		the ones reflected as drive 1 or 2.

		The first drive is stored in the high nibble of CMOS
		byte 0x12, the second in the low nibble.  This will be
		either a 4 bit drive type or 0xf indicating use byte 0x19 
		for an 8 bit type, drive 1, 0x1a for drive 2 in CMOS.

		Needless to say, a non-zero value means we have 
		an AT controller hard disk for that drive.

		
	*/

	if ((cmos_disks = CMOS_READ(0x12)) & 0xf0)
		if (cmos_disks & 0x0f)
			NR_HD = 2;
		else
			NR_HD = 1;
	else
		NR_HD = 0;
	for (i = NR_HD ; i < 2 ; i++) {
		hd[i*5].start_sect = 0;
		hd[i*5].nr_sects = 0;
	}
	for (drive=0 ; drive<NR_HD ; drive++) { //第1个物理盘设备号是0x300，第2个是0x305，读每个物理硬盘的0号块，即引导块，有分区信息
		// kernel/blk_drv/ll_rw_blk.c
		// 0x300>>8 = 3，对应struct blk_dev_struct blk_dev[NR_BLK_DEV]中硬盘的编号
		// 0x300 HD; 0x200, FD; 0x100, RD
		if (!(bh = bread(0x300 + drive*5,0))) { // bread: block read, 读取引导块
			printk("Unable to read partition table of drive %d\n\r",
				drive);
			panic("");
		}
		if (bh->b_data[510] != 0x55 || (unsigned char)
		    bh->b_data[511] != 0xAA) { // 硬盘信息有效标志：第一扇区的最后两个字节为 0x55AA
			printk("Bad partition table on drive %d\n\r",drive);
			panic("");
		}
		p = 0x1BE + (void *)bh->b_data; // 分区表位于硬盘第 1 扇区的 0x1BE 处
		for (i=1;i<5;i++,p++) { // 将分区表信息放入硬盘分区数据结构 hd 中
			hd[i+5*drive].start_sect = p->start_sect; // FIXME lyq: 这里都不用类型转换的吗？黑人问号！！！
			hd[i+5*drive].nr_sects = p->nr_sects;
		}
		brelse(bh); // 释放引导块
	}
	if (NR_HD)
		printk("Partition table%s ok.\n\r",(NR_HD>1)?"s":"");
	rd_load(); // 虚拟盘代替软盘，是指称为根设备
	mount_root(); // 把虚拟文件的根设备加载到文件系统
	return (0);
}

static int controller_ready(void)
{
	int retries=10000;

	while (--retries && (inb_p(HD_STATUS)&0xc0)!=0x40);
	return (retries);
}

static int win_result(void)
{
	int i=inb_p(HD_STATUS);

	if ((i & (BUSY_STAT | READY_STAT | WRERR_STAT | SEEK_STAT | ERR_STAT))
		== (READY_STAT | SEEK_STAT))
		return(0); /* ok */
	if (i&1) i=inb(HD_ERROR);
	return (1);
}

static void hd_out(unsigned int drive,unsigned int nsect,unsigned int sect,
		unsigned int head,unsigned int cyl,unsigned int cmd,
		void (*intr_addr)(void))
{
	register int port asm("dx");

	if (drive>1 || head>15)
		panic("Trying to write bad sector");
	if (!controller_ready())
		panic("HD controller not ready"); // 如果等待一段时间后仍未就绪则出错，死机。
	do_hd = intr_addr; // read_intr / write_intr，读盘服务程序与硬盘中断操作程序相挂接，do_hd 函数指针将在硬盘中断程序中被调用。
	outb_p(hd_info[drive].ctl,HD_CMD); // outb_p设置参数: 向控制寄存器(0x3f6)输出控制字节。
	port=HD_DATA; // 置 dx 为数据寄存器端口(0x1f0)。
	outb_p(hd_info[drive].wpcom>>2,++port);
	outb_p(nsect,++port);
	outb_p(sect,++port);
	outb_p(cyl,++port);
	outb_p(cyl>>8,++port);
	outb_p(0xA0|(drive<<4)|head,++port);
	outb(cmd,++port); // 命令：发送硬盘控制命令到 8259A --> PIO & DMA 两种模式，本代码为 PIO 模式，一次拿一个扇区，一共需要2个扇区（make_request, req->nr_sectors=2），故一个命令跑两次
}

static int drive_busy(void)
{
	unsigned int i;

	for (i = 0; i < 10000; i++)
		if (READY_STAT == (inb_p(HD_STATUS) & (BUSY_STAT|READY_STAT)))
			break;
	i = inb(HD_STATUS);
	i &= BUSY_STAT | READY_STAT | SEEK_STAT;
	if (i == READY_STAT | SEEK_STAT)
		return(0);
	printk("HD controller times out\n\r");
	return(1);
}

static void reset_controller(void)
{
	int	i;

	outb(4,HD_CMD);
	for(i = 0; i < 100; i++) nop();
	outb(hd_info[0].ctl & 0x0f ,HD_CMD);
	if (drive_busy())
		printk("HD-controller still busy\n\r");
	if ((i = inb(HD_ERROR)) != 1)
		printk("HD-controller reset failed: %02x\n\r",i);
}

static void reset_hd(int nr)
{
	reset_controller();
	hd_out(nr,hd_info[nr].sect,hd_info[nr].sect,hd_info[nr].head-1,
		hd_info[nr].cyl,WIN_SPECIFY,&recal_intr);
}

void unexpected_hd_interrupt(void)
{
	printk("Unexpected HD interrupt\n\r");
}

static void bad_rw_intr(void)
{
	if (++CURRENT->errors >= MAX_ERRORS)
		end_request(0);
	if (CURRENT->errors > MAX_ERRORS/2)
		reset = 1;
}

static void read_intr(void)
{
	if (win_result()) { // 硬盘情况判断
		bad_rw_intr(); // 结束 request 或 重置
		do_hd_request(); // 重新发起请求，在INIT检查中结束
		return;
	}
	port_read(HD_DATA,CURRENT->buffer,256); // PIO 模式问答 -> 将数据从数据寄存器口读到请求结构缓冲区，每次读一字，即2B，共256*2B=512B
	CURRENT->errors = 0; // 清除错次数
	CURRENT->buffer += 512; // 调整缓冲区指针，指向新的空区
	CURRENT->sector++; // 扇区++
	if (--CURRENT->nr_sectors) { // 读了之后，需要读的扇区数--
		do_hd = &read_intr;      // 还有要读的内容，继续挂载 do_hd (因为之前do_hd被交换为NULL/0)
		return; // FIXME lyq: 硬盘端执行的命令被中断了，需要显示重启吗？这里好像没有这样做，即是硬盘的命令仍然继续，不需要显示重启？
	}
	end_request(1); // 更新 b_uptodate 并解锁
	do_hd_request(); // 重新发起请求，在INIT检查中结束
}

static void write_intr(void)
{
	if (win_result()) {
		bad_rw_intr();
		do_hd_request();
		return;
	}
	if (--CURRENT->nr_sectors) {
		CURRENT->sector++;
		CURRENT->buffer += 512;
		do_hd = &write_intr;
		port_write(HD_DATA,CURRENT->buffer,256);
		return;
	}
	end_request(1);
	do_hd_request();
}

static void recal_intr(void)
{
	if (win_result())
		bad_rw_intr();
	do_hd_request();
}

void do_hd_request(void)
{
	int i,r;
	unsigned int block,dev;
	unsigned int sec,head,cyl;
	unsigned int nsect;

	INIT_REQUEST; // 这里判断是否还有剩余的请求项
	dev = MINOR(CURRENT->dev); // 从请求中获取设备号 --> 即硬盘的哪个分区
	block = CURRENT->sector;   // 获取起始扇区
	if (dev >= 5*NR_HD || block+2 > hd[dev].nr_sects) { // 因为一次要求读写 2 个扇区（缓冲块1KB=512B*2），所以请求的扇区号不能大于分区中最后倒数第二个扇区号
		end_request(0);
		goto repeat;
	}
	block += hd[dev].start_sect;
	dev /= 5;
	__asm__("divl %4":"=a" (block),"=d" (sec):"0" (block),"1" (0),
		"r" (hd_info[dev].sect)); // 基于扇区数和磁头数，换算扇区号(sec)、所在柱面号(cyl)和磁头号(head)。
	__asm__("divl %4":"=a" (cyl),"=d" (head):"0" (block),"1" (0),
		"r" (hd_info[dev].head));
	sec++;
	nsect = CURRENT->nr_sectors;
	if (reset) {
		reset = 0; // 防止多次执行 if reset
		recalibrate = 1; // 防止多次执行 if recalibrate
		reset_hd(CURRENT_DEV); // 将通过调用 hd_out 向硬盘发送 WIN_SPECIFY 命令，建立硬盘读盘必要参数
		return;
	}
	if (recalibrate) { // recalibrate 重新校准
		recalibrate = 0;
		hd_out(dev,hd_info[CURRENT_DEV].sect,0,0,0,
			WIN_RESTORE,&recal_intr); // 向硬盘发送 WIN_RESTORE 命令，将磁头移动到 0 柱面，以便从硬盘上读取数据。
		return;
	}	
	if (CURRENT->cmd == WRITE) {
		hd_out(dev,nsect,sec,head,cyl,WIN_WRITE,&write_intr);
		for(i=0 ; i<3000 && !(r=inb_p(HD_STATUS)&DRQ_STAT) ; i++)
			/* nothing */ ;
		if (!r) {
			bad_rw_intr();
			goto repeat;
		}
		port_write(HD_DATA,CURRENT->buffer,256);
	} else if (CURRENT->cmd == READ) {
		hd_out(dev,nsect,sec,head,cyl,WIN_READ,&read_intr);
	} else
		panic("unknown hd-command");
}

void hd_init(void)
{
	blk_dev[MAJOR_NR].request_fn = DEVICE_REQUEST;
	set_intr_gate(0x2E,&hd_interrupt); // 中断服务程序挂载：设置硬盘中断
	outb_p(inb_p(0x21)&0xfb,0x21); // 允许 8259A 发出中断请求
	outb(inb_p(0xA1)&0xbf,0xA1); // 允许硬盘发出中断请求
}
