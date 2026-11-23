// SPDX-License-Identifier: GPL-2.0
/*
 * Reference tracing for the open vhost-vsock device file.
 *
 * Each get_file(), fget and fput() on the traced file logs the resulting
 * f_count with the caller, and the vhost-net and vhost-vsock release paths
 * and the exit of a crosvm task log their steps, so a file that outlives its
 * last user shows which reference stayed.
 */

#include <linux/fs.h>
#include <linux/sched.h>
#include <linux/string.h>
#include <linux/printk.h>
#include <linux/export.h>
#include <linux/vhost_ref_trace.h>

struct file *vhost_ref_trace_file;
EXPORT_SYMBOL_GPL(vhost_ref_trace_file);

noinline void vhost_ref_trace_note(struct file *file, const char *what,
				   void *caller)
{
	pr_info("vhost-probe: %s count=%ld pid=%d comm=%s caller=%pS\n", what,
		file_count(file), current->pid, current->comm, caller);
}
EXPORT_SYMBOL_GPL(vhost_ref_trace_note);

void vhost_ref_trace_vsock_open(struct file *file)
{
	vhost_ref_trace_file = file;
	pr_info("vhost-probe: vsock open file=%p count=%ld pid=%d comm=%s\n",
		file, file_count(file), current->pid, current->comm);
}
EXPORT_SYMBOL_GPL(vhost_ref_trace_vsock_open);

void vhost_ref_trace_vsock_release(struct file *file, u32 guest_cid)
{
	pr_info("vhost-probe: vsock release enter file=%p cid=%u pid=%d comm=%s\n",
		file, guest_cid, current->pid, current->comm);
	if (vhost_ref_trace_file == file)
		vhost_ref_trace_file = NULL;
}
EXPORT_SYMBOL_GPL(vhost_ref_trace_vsock_release);

void vhost_ref_trace_vsock_release_rcu(struct file *file)
{
	pr_info("vhost-probe: vsock release past rcu file=%p\n", file);
}
EXPORT_SYMBOL_GPL(vhost_ref_trace_vsock_release_rcu);

void vhost_ref_trace_net_release(struct file *file)
{
	pr_info("vhost-probe: net release enter file=%p pid=%d comm=%s\n", file,
		current->pid, current->comm);
}
EXPORT_SYMBOL_GPL(vhost_ref_trace_net_release);

void vhost_ref_trace_net_release_done(struct file *file)
{
	pr_info("vhost-probe: net release cleanup done file=%p\n", file);
}
EXPORT_SYMBOL_GPL(vhost_ref_trace_net_release_done);

void vhost_ref_trace_set_owner(const void *dev, int worker_pid, const void *mm)
{
	pr_info("vhost-probe: set_owner dev=%p worker=%d owner=%d comm=%s mm=%p\n",
		dev, worker_pid, current->pid, current->comm, mm);
}
EXPORT_SYMBOL_GPL(vhost_ref_trace_set_owner);

/* Called around exit_files(); only tasks named crosvm are reported. */
void vhost_ref_trace_exit(struct task_struct *tsk, bool group_dead,
			  bool files_closed)
{
	struct file *file = vhost_ref_trace_file;

	if (!file || strncmp(tsk->comm, "crosvm", 6))
		return;

	if (files_closed)
		pr_info("vhost-probe: after exit_files pid=%d file_count=%ld\n",
			tsk->pid, file_count(file));
	else
		pr_info("vhost-probe: do_exit pid=%d tgid=%d comm=%s group_dead=%d files_count=%d file_count=%ld\n",
			tsk->pid, tsk->tgid, tsk->comm, group_dead,
			tsk->files ? atomic_read(&tsk->files->count) : -1,
			file_count(file));
}
