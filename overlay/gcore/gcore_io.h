/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _GCORE_IO_H
#define _GCORE_IO_H

#include <linux/types.h>
#include <linux/ioctl.h>

#define GCORE_IOC_MAGIC 'G'

#define GC_OP_READ  0
#define GC_OP_WRITE 1

struct gc_rw {
	__s32 pid;
	__u32 op;
	__u64 addr;
	__u64 buf;
	__u32 len;
	__u32 reserved;
};

#define GC_VMA_NAME 64

struct gc_vma {
	__u64 start;
	__u64 end;
	__u64 flags;
	char name[GC_VMA_NAME];
};

struct gc_maps {
	__s32 pid;
	__u32 max;
	__u64 buf;
	__u32 count;
	__u32 reserved;
};

struct gc_exec_event {
	__s32 pid;
	__u32 uid;
	char comm[16];
	char path[192];
};

#define GC_IOC_RW	_IOWR(GCORE_IOC_MAGIC, 1, struct gc_rw)
#define GC_IOC_MAPS	_IOWR(GCORE_IOC_MAGIC, 2, struct gc_maps)

#endif
