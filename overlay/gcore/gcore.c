// SPDX-License-Identifier: GPL-2.0
/*
 * gcore - kernel memory engine for userspace payloads.
 * Read/write another task's memory without ptrace or /proc/pid/mem,
 * and publish exec events to a root daemon.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/sched.h>
#include <linux/sched/mm.h>
#include <linux/mm.h>
#include <linux/cred.h>
#include <linux/string.h>
#include <linux/kfifo.h>
#include <linux/wait.h>
#include <linux/poll.h>
#include <linux/spinlock.h>

#include "gcore_io.h"

#define GCORE_MAX_RW (1U << 20)

static DEFINE_KFIFO(gcore_events, struct gc_exec_event, 256);
static DEFINE_SPINLOCK(gcore_lock);
static DECLARE_WAIT_QUEUE_HEAD(gcore_wait);
static bool gcore_armed;

void gcore_on_exec(const char *path)
{
	struct gc_exec_event ev;
	unsigned long flags;

	if (!gcore_armed || !path)
		return;

	memset(&ev, 0, sizeof(ev));
	ev.pid = current->pid;
	ev.uid = current_uid().val;
	get_task_comm(ev.comm, current);
	strscpy(ev.path, path, sizeof(ev.path));

	spin_lock_irqsave(&gcore_lock, flags);
	kfifo_in(&gcore_events, &ev, sizeof(ev));
	spin_unlock_irqrestore(&gcore_lock, flags);
	wake_up_interruptible(&gcore_wait);
}

static struct task_struct *gcore_task(pid_t pid)
{
	struct task_struct *t;

	rcu_read_lock();
	t = find_task_by_vpid(pid);
	if (t)
		get_task_struct(t);
	rcu_read_unlock();
	return t;
}

static int gcore_rw_op(struct gc_rw *r)
{
	struct task_struct *t;
	void *kb;
	int ret;

	if (!r->len || r->len > GCORE_MAX_RW)
		return -EINVAL;
	if (r->op != GC_OP_READ && r->op != GC_OP_WRITE)
		return -EINVAL;

	t = gcore_task(r->pid);
	if (!t)
		return -ESRCH;

	kb = kmalloc(r->len, GFP_KERNEL);
	if (!kb) {
		put_task_struct(t);
		return -ENOMEM;
	}

	if (r->op == GC_OP_WRITE &&
	    copy_from_user(kb, (void __user *)(uintptr_t)r->buf, r->len)) {
		ret = -EFAULT;
		goto out;
	}

	ret = access_process_vm(t, r->addr, kb, r->len,
				FOLL_FORCE |
				(r->op == GC_OP_WRITE ? FOLL_WRITE : 0));
	if (ret > 0 && r->op == GC_OP_READ &&
	    copy_to_user((void __user *)(uintptr_t)r->buf, kb, ret))
		ret = -EFAULT;

out:
	kfree(kb);
	put_task_struct(t);
	return ret;
}

static int gcore_maps_op(struct gc_maps *m, void __user *uarg)
{
	struct task_struct *t;
	struct mm_struct *mm;
	struct vm_area_struct *v;
	struct gc_vma rec;
	__u32 n = 0;

	if (!m->max)
		return -EINVAL;

	t = gcore_task(m->pid);
	if (!t)
		return -ESRCH;

	mm = get_task_mm(t);
	if (!mm) {
		put_task_struct(t);
		return -EINVAL;
	}

	down_read(&mm->mmap_sem);
	for (v = mm->mmap; v && n < m->max; v = v->vm_next) {
		memset(&rec, 0, sizeof(rec));
		rec.start = v->vm_start;
		rec.end = v->vm_end;
		rec.flags = v->vm_flags;
		if (v->vm_file)
			strscpy(rec.name,
				v->vm_file->f_path.dentry->d_name.name,
				sizeof(rec.name));
		if (copy_to_user((void __user *)(uintptr_t)
				 (m->buf + (u64)n * sizeof(rec)),
				 &rec, sizeof(rec))) {
			up_read(&mm->mmap_sem);
			mmput(mm);
			put_task_struct(t);
			return -EFAULT;
		}
		n++;
	}
	up_read(&mm->mmap_sem);
	mmput(mm);
	put_task_struct(t);

	m->count = n;
	if (copy_to_user(uarg, m, sizeof(*m)))
		return -EFAULT;
	return 0;
}

static ssize_t gcore_read(struct file *f, char __user *buf, size_t count,
			  loff_t *ppos)
{
	struct gc_exec_event ev;
	unsigned long flags;
	unsigned int got;

	if (count < sizeof(ev))
		return -EINVAL;

	for (;;) {
		if (kfifo_is_empty(&gcore_events)) {
			if (f->f_flags & O_NONBLOCK)
				return -EAGAIN;
			if (wait_event_interruptible(gcore_wait,
						     !kfifo_is_empty(&gcore_events)))
				return -ERESTARTSYS;
		}
		spin_lock_irqsave(&gcore_lock, flags);
		got = kfifo_out(&gcore_events, &ev, sizeof(ev));
		spin_unlock_irqrestore(&gcore_lock, flags);
		if (got)
			break;
	}

	if (copy_to_user(buf, &ev, sizeof(ev)))
		return -EFAULT;
	return sizeof(ev);
}

static unsigned int gcore_poll(struct file *f, poll_table *wait)
{
	unsigned int mask = 0;

	poll_wait(f, &gcore_wait, wait);
	spin_lock(&gcore_lock);
	if (!kfifo_is_empty(&gcore_events))
		mask |= POLLIN | POLLRDNORM;
	spin_unlock(&gcore_lock);
	return mask;
}

static long gcore_ioctl(struct file *f, unsigned int cmd, unsigned long arg)
{
	struct gc_rw r;
	struct gc_maps m;

	switch (cmd) {
	case GC_IOC_RW:
		if (copy_from_user(&r, (void __user *)arg, sizeof(r)))
			return -EFAULT;
		return gcore_rw_op(&r);
	case GC_IOC_MAPS:
		if (copy_from_user(&m, (void __user *)arg, sizeof(m)))
			return -EFAULT;
		return gcore_maps_op(&m, (void __user *)arg);
	default:
		return -ENOTTY;
	}
}

static const struct file_operations gcore_fops = {
	.owner = THIS_MODULE,
	.read = gcore_read,
	.poll = gcore_poll,
	.unlocked_ioctl = gcore_ioctl,
	.llseek = noop_llseek,
};

static struct miscdevice gcore_dev = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "gcore",
	.fops = &gcore_fops,
};

static int __init gcore_init(void)
{
	int ret;

	ret = misc_register(&gcore_dev);
	if (ret)
		return ret;

	gcore_armed = true;
	pr_info("gcore: engine ready\n");
	return 0;
}

static void __exit gcore_exit(void)
{
	gcore_armed = false;
	misc_deregister(&gcore_dev);
}

device_initcall(gcore_init);
module_exit(gcore_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("gcore kernel memory engine");
