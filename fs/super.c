/*
 *  linux/fs/super.c
 *
 *  (C) 1991  Linus Torvalds
 */

/*
 * super.c contains code to handle the super-block tables.
 */
#include <linux/config.h>
#include <linux/sched.h>
#include <linux/kernel.h>
#include <asm/system.h>

#include <errno.h>
#include <sys/stat.h>

int sync_dev(int dev);
void wait_for_keypress(void);

/* set_bit uses setb, as gas doesn't recognize setc */
#define set_bit(bitnr,addr) ({ \
register int __res __asm__("ax"); \
__asm__("bt %2,%3;setb %%al":"=a" (__res):"a" (0),"r" (bitnr),"m" (*(addr))); \
__res; })

struct super_block super_block[NR_SUPER];
/* this is initialized in init/main.c */
int ROOT_DEV = 0;

static void lock_super(struct super_block * sb)
{
	cli();
	while (sb->s_lock)
		sleep_on(&(sb->s_wait));
	sb->s_lock = 1;
	sti();
}

static void free_super(struct super_block * sb)
{
	cli();
	sb->s_lock = 0;
	wake_up(&(sb->s_wait));
	sti();
}

static void wait_on_super(struct super_block * sb)
{
	cli();
	while (sb->s_lock)
		sleep_on(&(sb->s_wait));
	sti();
}

struct super_block * get_super(int dev)
{
	struct super_block * s;

	if (!dev)
		return NULL;
	s = 0+super_block;
	while (s < NR_SUPER+super_block)
		if (s->s_dev == dev) {
			wait_on_super(s);
			if (s->s_dev == dev)
				return s;
			s = 0+super_block;
		} else
			s++;
	return NULL;
}

void put_super(int dev)
{
	struct super_block * sb;
	struct m_inode * inode;
	int i;

	if (dev == ROOT_DEV) {
		printk("root diskette changed: prepare for armageddon\n\r");
		return;
	}
	if (!(sb = get_super(dev)))
		return;
	if (sb->s_imount) {
		printk("Mounted disk changed - tssk, tssk\n\r");
		return;
	}
	lock_super(sb);
	sb->s_dev = 0;
	for(i=0;i<I_MAP_SLOTS;i++)
		brelse(sb->s_imap[i]);
	for(i=0;i<Z_MAP_SLOTS;i++)
		brelse(sb->s_zmap[i]);
	free_super(sb);
	return;
}

static struct super_block * read_super(int dev)
{
	struct super_block * s;
	struct buffer_head * bh;
	int i,block;

	if (!dev)
		return NULL;
	check_disk_change(dev); // 检查是否换过盘。如果更换过盘，则高速缓冲区有关该设备的所有缓冲块均失效，需要进行失效处理（释放原来加载的文件系统）
	if (s = get_super(dev)) // 寻找现有或空闲的超级块
		return s;
	for (s = 0+super_block ;; s++) { // 再次检查是否空闲（笑死，程序互不信任，突然懂了：安全课的信任链，剧里的“信任才需要理由，不信任是天经地义”。）
		if (s >= NR_SUPER+super_block)
			return NULL;
		if (!s->s_dev) // 超级块中还没有分配 dev --> 空闲超级块
			break;
	}
	// 初始化超级块并加锁
	s->s_dev = dev;
	s->s_isup = NULL;
	s->s_imount = NULL;
	s->s_time = 0;
	s->s_rd_only = 0;
	s->s_dirt = 0;
	lock_super(s); // 加锁，之后会涉及读缓冲块
	if (!(bh = bread(dev,1))) { // bread 读超级块到缓冲区，超级块在该设备的第2项（索引1，细节）
		s->s_dev=0;
		free_super(s);
		return NULL;
	}
	// 先备份再释放是个好习惯。节约紧张的缓冲区资源，又不影响自己的使用。
	*((struct d_super_block *) s) =
		*((struct d_super_block *) bh->b_data);
	brelse(bh);
	if (s->s_magic != SUPER_MAGIC) { // 魔数检测，确定设备的文件系统是否可用
		s->s_dev = 0;
		free_super(s);
		return NULL;
	}
	for (i=0;i<I_MAP_SLOTS;i++) // 初始化 s_imap 和 s_zmap
		s->s_imap[i] = NULL;
	for (i=0;i<Z_MAP_SLOTS;i++)
		s->s_zmap[i] = NULL;
	block=2;
	for (i=0 ; i < s->s_imap_blocks ; i++)
		if (s->s_imap[i]=bread(dev,block)) // 读并挂载：细节，inode 位点图在第3个扇区
			block++;
		else
			break;
	for (i=0 ; i < s->s_zmap_blocks ; i++)
		if (s->s_zmap[i]=bread(dev,block))
			block++;
		else
			break;
	if (block != 2+s->s_imap_blocks+s->s_zmap_blocks) { // 挂载出现问题，释放缓冲块
		for(i=0;i<I_MAP_SLOTS;i++)
			brelse(s->s_imap[i]);
		for(i=0;i<Z_MAP_SLOTS;i++)
			brelse(s->s_zmap[i]);
		s->s_dev=0;
		free_super(s);
		return NULL;
	}
	// 对于申请空闲 i 节点的函数来讲，如果设备上所有的 i 节点已经全被使用，则查找函数会返回 0 值。因此 0 号 i 节点是不能用的，所以这里将位图中的最低位设置为 1，以防止文件系统分配 0 号 i 节点。同样的道理，也将逻辑块位图的最低位设置为 1。
	s->s_imap[0]->b_data[0] |= 1;
	s->s_zmap[0]->b_data[0] |= 1;
	free_super(s); // 操作完毕，解锁该超级块
	return s;
}

int sys_umount(char * dev_name)
{
	struct m_inode * inode;
	struct super_block * sb;
	int dev;

	if (!(inode=namei(dev_name)))
		return -ENOENT;
	dev = inode->i_zone[0];
	if (!S_ISBLK(inode->i_mode)) {
		iput(inode);
		return -ENOTBLK;
	}
	iput(inode);
	if (dev==ROOT_DEV)
		return -EBUSY;
	if (!(sb=get_super(dev)) || !(sb->s_imount))
		return -ENOENT;
	if (!sb->s_imount->i_mount)
		printk("Mounted inode has i_mount=0\n");
	for (inode=inode_table+0 ; inode<inode_table+NR_INODE ; inode++)
		if (inode->i_dev==dev && inode->i_count)
				return -EBUSY;
	sb->s_imount->i_mount=0;
	iput(sb->s_imount);
	sb->s_imount = NULL;
	iput(sb->s_isup);
	sb->s_isup = NULL;
	put_super(dev);
	sync_dev(dev);
	return 0;
}

// dev--mount-->dir，这里的 mount 挂载可以理解为将设备 dev【安装】到目录 dir 中
int sys_mount(char * dev_name, char * dir_name, int rw_flag)
{
	struct m_inode * dev_i, * dir_i;
	struct super_block * sb;
	int dev;

	if (!(dev_i=namei(dev_name))) // step1: 获取设备(一般是hd1)文件的i节点
		return -ENOENT;
	dev = dev_i->i_zone[0]; // step2: 获取设备号
	if (!S_ISBLK(dev_i->i_mode)) { // 判断是否是设备文件
		iput(dev_i);
		return -EPERM;
	}
	iput(dev_i); // step3: 释放设备 inode --> 用时放入 inode_table[32]，用完移除 inode_table[32]
	if (!(dir_i=namei(dir_name))) // step4: 获取mount的目录地址的i节点
		return -ENOENT;
	if (dir_i->i_count != 1 || dir_i->i_num == ROOT_INO) { // ROOT_INO ROOT 的 i_num，即判断挂载的目录是否是根inode
		iput(dir_i);
		return -EBUSY;
	}
	if (!S_ISDIR(dir_i->i_mode)) { // 判断是不是文件
		iput(dir_i);
		return -EPERM;
	}
	if (!(sb=read_super(dev))) { // 读取需要挂载的设备的超级块 --> 会载入到 super_block 中
		iput(dir_i);
		return -EBUSY;
	}
	if (sb->s_imount) { // 判断这个超级块是否被挂载（如果已经挂载了，你还来 sys_mount 干嘛！）
		iput(dir_i);
		return -EBUSY;
	}
	if (dir_i->i_mount) { // 判断要挂载的目录，是否被挂载（如果已经被挂载了，就不能将我们的dev挂载到dir上了）
		iput(dir_i);
		return -EPERM;
	}
	sb->s_imount=dir_i; // 挂载时，赋值 dir 的 inode (m_inode类型)
	dir_i->i_mount=1; // 挂载后置1 (char类型)
	dir_i->i_dirt=1; // 修改后，dir置1/* NOTE! we don't iput(dir_i) */
	return 0;			/* we do that in umount */
}

// 加载根文件系统：mount_root；加载普通文件系统: sys_mount
void mount_root(void)
{
	int i,free;
	struct super_block * p;
	struct m_inode * mi;

	if (32 != sizeof (struct d_inode))
		panic("bad i-node size");
	for(i=0;i<NR_FILE;i++) // 初始化文件引用计数
		file_table[i].f_count=0;
	if (MAJOR(ROOT_DEV) == 2) { // 根设备现在是虚拟盘
		printk("Insert root floppy and press ENTER");
		wait_for_keypress();
	}
	for(p = &super_block[0] ; p < &super_block[NR_SUPER] ; p++) { // 初始化超级块，设备、锁定标志、等待进程
		p->s_dev = 0;
		p->s_lock = 0;
		p->s_wait = NULL;
	}
	if (!(p=read_super(ROOT_DEV))) // 此时 ROOT_DEV MAJOR = 1，读超级块
		panic("Unable to mount root");
	if (!(mi=iget(ROOT_DEV,ROOT_INO))) // ROOT_INO: 根 i 节点的 number 为 1 --> 获取虚拟盘上的根i节点
		panic("Unable to read root i-node");
	mi->i_count += 3 ;	/* NOTE! it is logically used 4 times, not 1, inode_table + super_block + task_struct，共3次 */
	p->s_isup = p->s_imount = mi; /* 重要！加载根文件系统标志性动作。根i节点本身的isup和imount就是他自己本身，由此根文件系统加载完毕 */
	current->pwd = mi; // pwd 当前位置(Print Working Directory)，current目前为进程1 // NOTE lyq: 当前找到的虚拟盘文件系统在根文件系统的位置，提前存下来。可以用于相对路径的使用 --> 从进程1开始有文件系统功能
	current->root = mi; // fs/open.c 中 sys_chroot 和 sys_chdir 可以修改 root 和 pwd
	free=0;
	i=p->s_nzones; // p的逻辑块数
	while (-- i >= 0)
		if (!set_bit(i&8191,p->s_zmap[i>>13]->b_data)) // 逻辑块位图 0x1FFF=8191=8*1024-1, 因为0块被限制。
			free++;
	printk("%d/%d free blocks\n\r",free,p->s_nzones);
	free=0;
	i=p->s_ninodes+1; // p 的i节点数
	while (-- i >= 0)
		if (!set_bit(i&8191,p->s_imap[i>>13]->b_data)) // i节点位图 0x1FFF
			free++;
	printk("%d/%d free inodes\n\r",free,p->s_ninodes);
}
