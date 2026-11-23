/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_VHOST_REF_TRACE_H
#define _LINUX_VHOST_REF_TRACE_H

#include <linux/compiler.h>
#include <linux/types.h>

struct file;
struct task_struct;

#ifdef CONFIG_VHOST_FILE_REF_TRACE

/*
 * The traced file is the open vhost-vsock device file. The pointer holds no
 * reference; it is compared against and cleared in vhost_vsock_dev_release()
 * before the file is freed.
 */
extern struct file *vhost_ref_trace_file;

void vhost_ref_trace_note(struct file *file, const char *what, void *caller);
void vhost_ref_trace_vsock_open(struct file *file);
void vhost_ref_trace_vsock_release(struct file *file, u32 guest_cid);
void vhost_ref_trace_vsock_release_rcu(struct file *file);
void vhost_ref_trace_net_release(struct file *file);
void vhost_ref_trace_net_release_done(struct file *file);
void vhost_ref_trace_set_owner(const void *dev, int worker_pid, const void *mm);
void vhost_ref_trace_exit(struct task_struct *tsk, bool group_dead,
			  bool files_closed);

/* Logs a reference event when file is the traced file. */
static inline void vhost_ref_trace_ref(struct file *file, const char *what,
				       void *caller)
{
	if (unlikely(file && file == vhost_ref_trace_file))
		vhost_ref_trace_note(file, what, caller);
}

#else

static inline void vhost_ref_trace_ref(struct file *file, const char *what,
				       void *caller)
{
}

static inline void vhost_ref_trace_vsock_open(struct file *file)
{
}

static inline void vhost_ref_trace_vsock_release(struct file *file,
						 u32 guest_cid)
{
}

static inline void vhost_ref_trace_vsock_release_rcu(struct file *file)
{
}

static inline void vhost_ref_trace_net_release(struct file *file)
{
}

static inline void vhost_ref_trace_net_release_done(struct file *file)
{
}

static inline void vhost_ref_trace_set_owner(const void *dev, int worker_pid,
					     const void *mm)
{
}

static inline void vhost_ref_trace_exit(struct task_struct *tsk,
					bool group_dead, bool files_closed)
{
}

#endif /* CONFIG_VHOST_FILE_REF_TRACE */

#endif /* _LINUX_VHOST_REF_TRACE_H */
