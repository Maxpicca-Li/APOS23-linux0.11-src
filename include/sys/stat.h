#ifndef _SYS_STAT_H
#define _SYS_STAT_H

#include <sys/types.h>

struct stat {
	dev_t	st_dev;
	ino_t	st_ino;
	umode_t	st_mode;
	nlink_t	st_nlink;
	uid_t	st_uid;
	gid_t	st_gid;
	dev_t	st_rdev;
	off_t	st_size;
	time_t	st_atime;
	time_t	st_mtime;
	time_t	st_ctime;
};

/* 用于识别 inode 种类 */
#define S_IFMT  00170000 // 0b1111000000000000 文件类型（8 进制表示）
#define S_IFREG  0100000 // 0b1000000000000000 常规文件
#define S_IFBLK  0060000 // 0b0110000000000000 块特殊（设备）文件，如磁盘 dev/fd0
#define S_IFDIR  0040000 // 0b0100000000000000 目录文件
#define S_IFCHR  0020000 // 0b0010000000000000 字符设备文件
#define S_IFIFO  0010000 // 0b0001000000000000 FIFO 特殊文件
// 文件属性位：
// S_ISUID 用于测试文件的 Set-User-ID 标志是否置位。若该标志置位，则当执行该文件时，进程的
// 有效用户 ID 将被设置为该文件宿主的用户 ID。S_ISGID 则是针对组 ID 进行相同处理。
#define S_ISUID  0004000 // 执行时设置用户 ID（set-user-ID）
#define S_ISGID  0002000 // 执行时设置组 ID（set-group-ID）
#define S_ISVTX  0001000 // 对于目录，受限删除标志

#define S_ISREG(m)	(((m) & S_IFMT) == S_IFREG)
#define S_ISDIR(m)	(((m) & S_IFMT) == S_IFDIR)
#define S_ISCHR(m)	(((m) & S_IFMT) == S_IFCHR)
#define S_ISBLK(m)	(((m) & S_IFMT) == S_IFBLK)
#define S_ISFIFO(m)	(((m) & S_IFMT) == S_IFIFO)

#define S_IRWXU 00700
#define S_IRUSR 00400
#define S_IWUSR 00200
#define S_IXUSR 00100

#define S_IRWXG 00070
#define S_IRGRP 00040
#define S_IWGRP 00020
#define S_IXGRP 00010

#define S_IRWXO 00007
#define S_IROTH 00004
#define S_IWOTH 00002
#define S_IXOTH 00001

extern int chmod(const char *_path, mode_t mode);
extern int fstat(int fildes, struct stat *stat_buf);
extern int mkdir(const char *_path, mode_t mode);
extern int mkfifo(const char *_path, mode_t mode);
extern int stat(const char *filename, struct stat *stat_buf);
extern mode_t umask(mode_t mask);

#endif
