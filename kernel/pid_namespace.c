/*
 * Pid namespaces
 *
 * Authors:
 *    (C) 2007 Pavel Emelyanov <xemul@openvz.org>, OpenVZ, SWsoft Inc.
 *    (C) 2007 Sukadev Bhattiprolu <sukadev@us.ibm.com>, IBM
 *     Many thanks to Oleg Nesterov for comments and help
 *
 */

#include <linux/pid.h>
#include <linux/pid_namespace.h>
#include <linux/syscalls.h>
#include <linux/err.h>
#include <linux/acct.h>
#include <linux/slab.h>
#include <linux/proc_fs.h>

#define BITS_PER_PAGE		(PAGE_SIZE*8)

struct pid_cache {
	int nr_ids;
	char name[16];
	struct kmem_cache *cachep;
	struct list_head list;
};

static LIST_HEAD(pid_caches_lh);
static DEFINE_MUTEX(pid_caches_mutex);
static struct kmem_cache *pid_ns_cachep;

/*
 * creates the kmem cache to allocate pids from.
 * @nr_ids: the number of numerical ids this pid will have to carry
 */

static struct kmem_cache *create_pid_cachep(int nr_ids)
{
	struct pid_cache *pcache;
	struct kmem_cache *cachep;

	mutex_lock(&pid_caches_mutex);
	list_for_each_entry(pcache, &pid_caches_lh, list)
		if (pcache->nr_ids == nr_ids)
			goto out;

	pcache = kmalloc(sizeof(struct pid_cache), GFP_KERNEL);
	if (pcache == NULL)
		goto err_alloc;

	snprintf(pcache->name, sizeof(pcache->name), "pid_%d", nr_ids);
	cachep = kmem_cache_create(pcache->name,
			sizeof(struct pid) + (nr_ids - 1) * sizeof(struct upid),
			0, SLAB_HWCACHE_ALIGN, NULL);
	if (cachep == NULL)
		goto err_cachep;

	pcache->nr_ids = nr_ids;
	pcache->cachep = cachep;
	list_add(&pcache->list, &pid_caches_lh);
out:
	mutex_unlock(&pid_caches_mutex);
	return pcache->cachep;

err_cachep:
	kfree(pcache);
err_alloc:
	mutex_unlock(&pid_caches_mutex);
	return NULL;
}

static struct pid_namespace *create_pid_namespace(struct pid_namespace *parent_pid_ns)
{
	struct pid_namespace *ns;
	unsigned int level = parent_pid_ns->level + 1;
	int i, err = -ENOMEM;

	ns = kmem_cache_zalloc(pid_ns_cachep, GFP_KERNEL);
	if (ns == NULL)
		goto out;

	ns->pidmap[0].page = kzalloc(PAGE_SIZE, GFP_KERNEL);
	if (!ns->pidmap[0].page)
		goto out_free;

	ns->pid_cachep = create_pid_cachep(level + 1);
	if (ns->pid_cachep == NULL)
		goto out_free_map;

	kref_init(&ns->kref);
	ns->level = level;
	ns->parent = get_pid_ns(parent_pid_ns);

	set_bit(0, ns->pidmap[0].page);
	atomic_set(&ns->pidmap[0].nr_free, BITS_PER_PAGE - 1);

	for (i = 1; i < PIDMAP_ENTRIES; i++)
		atomic_set(&ns->pidmap[i].nr_free, BITS_PER_PAGE);

	err = proc_alloc_inum(&ns->proc_inum);
	if (err)
		goto out_put_parent_pid_ns;

	err = pid_ns_prepare_proc(ns);
	if (err)
		goto out_free_proc_inum;

	return ns;

out_free_proc_inum:
	proc_free_inum(ns->proc_inum);
out_put_parent_pid_ns:
	put_pid_ns(parent_pid_ns);
out_free_map:
	kfree(ns->pidmap[0].page);
out_free:
	kmem_cache_free(pid_ns_cachep, ns);
out:
	return ERR_PTR(err);
}

static void destroy_pid_namespace(struct pid_namespace *ns)
{
	int i;

	proc_free_inum(ns->proc_inum);
	for (i = 0; i < PIDMAP_ENTRIES; i++)
		kfree(ns->pidmap[i].page);
	kmem_cache_free(pid_ns_cachep, ns);
}

struct pid_namespace *copy_pid_ns(unsigned long flags,
	struct pid_namespace *default_ns, struct pid_namespace *active_ns)
{
	if (!(flags & CLONE_NEWPID))
		return get_pid_ns(default_ns);
	if (flags & (CLONE_THREAD|CLONE_PARENT))
		return ERR_PTR(-EINVAL);
	return create_pid_namespace(active_ns);
}

void free_pid_ns(struct kref *kref)
{
	struct pid_namespace *ns, *parent;

	ns = container_of(kref, struct pid_namespace, kref);

	parent = ns->parent;
	destroy_pid_namespace(ns);

	if (parent != NULL)
		put_pid_ns(parent);
}

void zap_pid_ns_processes(struct pid_namespace *pid_ns)
{
	struct task_struct *me = current;
	int nr;
	int rc;
	struct task_struct *task;

	/*
	 * The last task in the pid namespace-init thread group is terminating.
	 * Find remaining pids in the namespace, signal and wait for them
	 * to to be reaped.
	 *
	 * By waiting for all of the tasks to be reaped before init is reaped
	 * we provide the invariant that no task can escape the pid namespace.
	 *
	 * Note:  This signals each task in the namespace - even those that
	 * 	  belong to the same thread group, To avoid this, we would have
	 * 	  to walk the entire tasklist looking a processes in this
	 * 	  namespace, but that could be unnecessarily expensive if the
	 * 	  pid namespace has just a few processes. Or we need to
	 * 	  maintain a tasklist for each pid namespace.
	 *
	 */
	read_lock(&tasklist_lock);
	pid_ns->dead = 1;
	for (nr = next_pidmap(pid_ns, 0); nr > 0; nr = next_pidmap(pid_ns, nr)) {

		/*
		 * Any nested-container's init processes won't ignore the
		 * SEND_SIG_NOINFO signal, see send_signal()->si_fromuser().
		 */
		rcu_read_lock();
		task = pid_task(find_pid_ns(nr, pid_ns), PIDTYPE_PID);
		if (task && !same_thread_group(task, me))
			send_sig_info(SIGKILL, SEND_SIG_NOINFO, task);
		rcu_read_unlock();
	}
	read_unlock(&tasklist_lock);

	/* Nicely reap all of the remaining children in the namespace */
	do {
		clear_thread_flag(TIF_SIGPENDING);
		rc = sys_wait4(-1, NULL, __WALL, NULL);
	} while (rc != -ECHILD);
       

repeat:
	/* Brute force wait for any remaining tasks to pass unhash_process
	 * in release_task.  Once a task has passed unhash_process there
	 * is no pid_namespace state left and they can be safely ignored.
	 */
	for (nr = next_pidmap(pid_ns, 1); nr > 0; nr = next_pidmap(pid_ns, nr)) {
		int found;

		/* Are there any tasks alive in this pid namespace */
		rcu_read_lock();
		task = pid_task(find_pid_ns(nr, pid_ns), PIDTYPE_PID);
		found = task && !same_thread_group(task, me);
		rcu_read_unlock();
		if (found) {
			clear_thread_flag(TIF_SIGPENDING);
			schedule_timeout_interruptible(HZ/10);
			goto repeat;
		}
	}
	/* At this point there are at most two tasks in the pid namespace.
	 * These tasks are our current task, and if we aren't pid 1 the zombie
	 * of pid 1. In either case pid 1 will be the final task reaped in this
	 * pid namespace, as non-leader threads are self reaping and leaders
	 * cannot be reaped until all of their siblings have been reaped.
	 */

	acct_exit_ns(pid_ns);
	return;
}

static void *pidns_get(struct task_struct *task)
{
	struct pid_namespace *ns;

	rcu_read_lock();
	ns = get_pid_ns(task_active_pid_ns(task));
	rcu_read_unlock();

	return ns;
}

static void pidns_put(void *ns)
{
	put_pid_ns(ns);
}

static int pidns_install(struct nsproxy *nsproxy, void *ns)
{
	return -EINVAL;
#ifdef notyet
	put_pid_ns(nsproxy->pid_ns);
	nsproxy->pid_ns = get_pid_ns(ns);
	return 0;
#endif
}

static unsigned int pidns_inum(void *vns)
{
	struct pid_namespace *ns = vns;
	return ns->proc_inum;
}

const struct proc_ns_operations pidns_operations = {
	.name		= "pid",
	.type		= CLONE_NEWPID,
	.get		= pidns_get,
	.put		= pidns_put,
	.install	= pidns_install,
	.inum		= pidns_inum,
};

static __init int pid_namespaces_init(void)
{
	pid_ns_cachep = KMEM_CACHE(pid_namespace, SLAB_PANIC);
	return 0;
}

__initcall(pid_namespaces_init);
