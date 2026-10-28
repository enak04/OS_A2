// SPDX-License-Identifier: GPL-2.0
#include <linux/fork_guard.h>
#include <linux/rcupdate.h>
#include <linux/signal.h>
#include <linux/uaccess.h>
#include <linux/capability.h>

#include <linux/cred.h>
#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/kthread.h>
#include <linux/limits.h>
#include <linux/pid.h>
#include <linux/printk.h>
#include <linux/proc_fs.h>
#include <linux/sched.h>
#include <linux/sched/signal.h>
#include <linux/seq_file.h>
#include <linux/spinlock.h>
#include <linux/sysctl.h>
#include <linux/string.h>
#include <linux/timekeeping.h>
#include <linux/uidgid.h>
#include <linux/user_namespace.h>
#include <linux/workqueue.h>

#define FG_MAX_UIDS 256

enum fg_stage {
	FG_STAGE_NORMAL = 0,
	FG_STAGE_THROTTLED,
	FG_STAGE_FROZEN,
	FG_STAGE_REAPED,
};

struct fg_uid_state {
	bool used;
	kuid_t uid;
	unsigned int window_forks;
	unsigned long window_start;
	enum fg_stage stage;
	unsigned long stage_start;
	u64 total_forks;
	u64 rejected_forks;
	u64 incidents;
};

/*
 * Process groups stopped by Fork Guard.
 * Each entry owns a reference to its process-group PID.
 */
#define FG_MAX_GROUPS 256

struct fg_stopped_group {
	bool used;
	kuid_t uid;
	struct pid *pgrp;
	unsigned long stopped_at;
};

static DEFINE_SPINLOCK(fg_group_lock);
static struct fg_stopped_group fg_groups[FG_MAX_GROUPS];

static DEFINE_SPINLOCK(fg_lock);
static struct fg_uid_state fg_states[FG_MAX_UIDS];
#define FG_MAX_TIERS 256

struct fg_tier_entry {
	bool used;
	kuid_t uid;
	enum fg_tier tier;
};

static DEFINE_SPINLOCK(fg_tier_lock);
static struct fg_tier_entry fg_tiers[FG_MAX_TIERS];

/* Tunables; thresholds are intentionally conservative defaults. */
static unsigned int fg_window_ms = 1000;
static unsigned int fg_throttle_threshold = 20;
static unsigned int fg_freeze_threshold = 40;
static unsigned int fg_reap_threshold = 80;
static unsigned int fg_throttle_delay_ms = 100;
static unsigned int fg_freeze_ms = 3000;
static unsigned int fg_cooldown_ms = 5000;

static struct ctl_table_header *fg_sysctl_header;
static struct delayed_work fg_recovery_work;
static void fg_recovery_worker(struct work_struct *work);


static unsigned int fg_min_value;
static unsigned int fg_max_window_ms = 60000;
static unsigned int fg_max_threshold = 100000;
static unsigned int fg_max_delay_ms = 60000;

static struct ctl_table fg_sysctl_table[] = {
	{
		.procname = "window_ms",
		.data = &fg_window_ms,
		.maxlen = sizeof(fg_window_ms),
		.mode = 0644,
		.proc_handler = proc_douintvec_minmax,
		.extra1 = &fg_min_value,
		.extra2 = &fg_max_window_ms,
	},
	{
		.procname = "throttle_threshold",
		.data = &fg_throttle_threshold,
		.maxlen = sizeof(fg_throttle_threshold),
		.mode = 0644,
		.proc_handler = proc_douintvec_minmax,
		.extra1 = &fg_min_value,
		.extra2 = &fg_max_threshold,
	},
	{
		.procname = "freeze_threshold",
		.data = &fg_freeze_threshold,
		.maxlen = sizeof(fg_freeze_threshold),
		.mode = 0644,
		.proc_handler = proc_douintvec_minmax,
		.extra1 = &fg_min_value,
		.extra2 = &fg_max_threshold,
	},
	{
		.procname = "reap_threshold",
		.data = &fg_reap_threshold,
		.maxlen = sizeof(fg_reap_threshold),
		.mode = 0644,
		.proc_handler = proc_douintvec_minmax,
		.extra1 = &fg_min_value,
		.extra2 = &fg_max_threshold,
	},
	{
		.procname = "throttle_delay_ms",
		.data = &fg_throttle_delay_ms,
		.maxlen = sizeof(fg_throttle_delay_ms),
		.mode = 0644,
		.proc_handler = proc_douintvec_minmax,
		.extra1 = &fg_min_value,
		.extra2 = &fg_max_delay_ms,
	},
	{
		.procname = "freeze_ms",
		.data = &fg_freeze_ms,
		.maxlen = sizeof(fg_freeze_ms),
		.mode = 0644,
		.proc_handler = proc_douintvec_minmax,
		.extra1 = &fg_min_value,
		.extra2 = &fg_max_delay_ms,
	},
	{
		.procname = "cooldown_ms",
		.data = &fg_cooldown_ms,
		.maxlen = sizeof(fg_cooldown_ms),
		.mode = 0644,
		.proc_handler = proc_douintvec_minmax,
		.extra1 = &fg_min_value,
		.extra2 = &fg_max_window_ms,
	},
	{ }
};

static struct fg_uid_state *fg_find_uid_locked(kuid_t uid)
{
	int i;

	for (i = 0; i < FG_MAX_UIDS; i++) {
		if (fg_states[i].used &&
		    uid_eq(fg_states[i].uid, uid))
			return &fg_states[i];
	}

	for (i = 0; i < FG_MAX_UIDS; i++) {
		if (!fg_states[i].used) {
			fg_states[i].used = true;
			fg_states[i].uid = uid;
			fg_states[i].window_start = jiffies;
			fg_states[i].stage = FG_STAGE_NORMAL;
			return &fg_states[i];
		}
	}

	return NULL;
}
/*
 * Record a process group stopped by Fork Guard.
 * Caller must hold an appropriate reference to pgrp.
 */
static bool fg_record_stopped_group(kuid_t uid, struct pid *pgrp)
{
	unsigned long flags;
	int i, free_slot = -1;

	spin_lock_irqsave(&fg_group_lock, flags);

	for (i = 0; i < FG_MAX_GROUPS; i++) {
		if (fg_groups[i].used && fg_groups[i].pgrp == pgrp) {
			spin_unlock_irqrestore(&fg_group_lock, flags);
			return false;
		}
		if (!fg_groups[i].used && free_slot < 0)
			free_slot = i;
	}

	if (free_slot >= 0) {
		fg_groups[free_slot].used = true;
		fg_groups[free_slot].uid = uid;
		fg_groups[free_slot].pgrp = get_pid(pgrp);
		fg_groups[free_slot].stopped_at = jiffies;
	}

	spin_unlock_irqrestore(&fg_group_lock, flags);
	return free_slot >= 0;
}

static void fg_release_uid_groups(kuid_t uid, int signal)
{
	struct pid **groups;
	unsigned long flags;
	int i, n = 0;

	groups = kcalloc(FG_MAX_GROUPS, sizeof(*groups), GFP_KERNEL);
	if (!groups)
		return;

	spin_lock_irqsave(&fg_group_lock, flags);
	for (i = 0; i < FG_MAX_GROUPS; i++) {
		if (fg_groups[i].used && uid_eq(fg_groups[i].uid, uid)) {
			groups[n++] = fg_groups[i].pgrp;
			fg_groups[i].used = false;
			fg_groups[i].pgrp = NULL;
		}
	}
	spin_unlock_irqrestore(&fg_group_lock, flags);

	for (i = 0; i < n; i++) {
		kill_pgrp(groups[i], signal, 1);
		put_pid(groups[i]);
	}

	kfree(groups);
}

static bool fg_uid_is_root(kuid_t uid)
{
	return uid_eq(uid, GLOBAL_ROOT_UID);
}

int fork_guard_check(void)
{
	struct fg_uid_state *s;
	struct pid *pgrp = NULL;
	unsigned long flags;
	unsigned long now = jiffies;
	kuid_t uid = current_uid();
	enum fg_stage stage;
	unsigned int count;
	bool delay = false;
	bool freeze_group = false;
	bool reap_group = false;
	int ret = 0;

	if (fg_uid_is_root(uid))
		return 0;

	/*
	 * Capture the caller's process group while protected by RCU.
	 * Keep a reference before leaving the read-side critical section.
	 */
	rcu_read_lock();
	pgrp = get_pid(task_pgrp(current));
	rcu_read_unlock();

	spin_lock_irqsave(&fg_lock, flags);
	s = fg_find_uid_locked(uid);
	if (!s) {
		spin_unlock_irqrestore(&fg_lock, flags);
		put_pid(pgrp);
		return 0;
	}

	if (time_after_eq(now, s->window_start +
			  msecs_to_jiffies(fg_window_ms))) {
		s->window_start = now;
		s->window_forks = 0;
		if (s->stage == FG_STAGE_NORMAL)
			s->stage = FG_STAGE_NORMAL;
	}

	s->window_forks++;
	s->total_forks++;
	count = s->window_forks;

	if (count >= fg_reap_threshold) {
		if (s->stage != FG_STAGE_REAPED) {
			s->stage = FG_STAGE_REAPED;
			s->stage_start = now;
			s->incidents++;
			reap_group = true;
		}
		ret = -EAGAIN;
	} else if (count >= fg_freeze_threshold) {
		if (s->stage < FG_STAGE_FROZEN) {
			s->stage = FG_STAGE_FROZEN;
			s->stage_start = now;
			s->incidents++;
			freeze_group = true;
		}
		ret = -EAGAIN;
	} else if (count >= fg_throttle_threshold) {
		if (s->stage < FG_STAGE_THROTTLED) {
			s->stage = FG_STAGE_THROTTLED;
			s->stage_start = now;
			s->incidents++;
		}
		delay = true;
	}
	stage = s->stage;

	if (ret)
		s->rejected_forks++;

	spin_unlock_irqrestore(&fg_lock, flags);

	if (freeze_group && fg_record_stopped_group(uid, pgrp))
		kill_pgrp(pgrp, SIGSTOP, 1);

	if (reap_group)
		fg_release_uid_groups(uid, SIGKILL);

	put_pid(pgrp);

	if (delay)
		msleep(fg_throttle_delay_ms);

	if (ret)
		pr_warn_ratelimited(
			"fork_guard: fork denied for uid %u at stage %u (window count %u)\n",
			__kuid_val(uid), stage, count);

	return ret;
}
static void fg_recovery_worker(struct work_struct *work)
{
	unsigned long flags;
	unsigned long now = jiffies;
	int i;

	for (i = 0; i < FG_MAX_UIDS; i++) {
		kuid_t resume_uid = INVALID_UID;
		struct fg_uid_state *s;

		spin_lock_irqsave(&fg_lock, flags);
		s = &fg_states[i];

		if (!s->used || s->stage == FG_STAGE_NORMAL) {
			spin_unlock_irqrestore(&fg_lock, flags);
			continue;
		}

		if (s->stage == FG_STAGE_THROTTLED &&
		    time_after_eq(now, s->stage_start +
				  msecs_to_jiffies(fg_cooldown_ms))) {
			s->stage = FG_STAGE_NORMAL;
			s->window_forks = 0;
			s->window_start = now;
		} else if (s->stage == FG_STAGE_FROZEN &&
			   time_after_eq(now, s->stage_start +
					 msecs_to_jiffies(fg_freeze_ms))) {
			resume_uid = s->uid;
			s->stage = FG_STAGE_NORMAL;
			s->window_forks = 0;
			s->window_start = now;
		} else if (s->stage == FG_STAGE_REAPED &&
			   time_after_eq(now, s->stage_start +
					 msecs_to_jiffies(fg_cooldown_ms))) {
			s->stage = FG_STAGE_NORMAL;
			s->window_forks = 0;
			s->window_start = now;
		}
		spin_unlock_irqrestore(&fg_lock, flags);

		if (uid_valid(resume_uid))
			fg_release_uid_groups(resume_uid, SIGCONT);
	}

	schedule_delayed_work(&fg_recovery_work, msecs_to_jiffies(250));
}

static int fg_status_show(struct seq_file *m, void *v)
{
	unsigned long flags;
	int i;

	seq_puts(m, "uid stage window_forks total_forks rejected_forks incidents\n");

	spin_lock_irqsave(&fg_lock, flags);
	for (i = 0; i < FG_MAX_UIDS; i++) {
		struct fg_uid_state *s = &fg_states[i];

		if (!s->used)
			continue;

		seq_printf(m, "%u %u %u %llu %llu %llu\n",
			   __kuid_val(s->uid), s->stage, s->window_forks,
			   (unsigned long long)s->total_forks,
			   (unsigned long long)s->rejected_forks,
			   (unsigned long long)s->incidents);
	}
	spin_unlock_irqrestore(&fg_lock, flags);

	seq_printf(m,
		   "tunables: window_ms=%u throttle_threshold=%u freeze_threshold=%u reap_threshold=%u throttle_delay_ms=%u freeze_ms=%u cooldown_ms=%u\n",
		   fg_window_ms, fg_throttle_threshold,
		   fg_freeze_threshold, fg_reap_threshold,
		   fg_throttle_delay_ms, fg_freeze_ms, fg_cooldown_ms);
	return 0;
}

static int fg_status_open(struct inode *inode, struct file *file)
{
	return single_open(file, fg_status_show, NULL);
}

static const struct proc_ops fg_status_ops = {
	.proc_open = fg_status_open,
	.proc_read = seq_read,
	.proc_lseek = seq_lseek,
	.proc_release = single_release,
};
static ssize_t fg_control_write(struct file *file,
				const char __user *buffer,
				size_t count, loff_t *ppos)
{
	char input[128];
	char tier_name[16];
	unsigned int uid_num;
	kuid_t uid;
	enum fg_tier tier;
	int parsed;

	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;

	if (!count || count >= sizeof(input))
		return -EINVAL;

	if (copy_from_user(input, buffer, count))
		return -EFAULT;

	input[count] = '\0';
	if (input[count - 1] == '\n')
		input[count - 1] = '\0';

	if (!strcmp(input, "tier reset")) {
		fork_guard_reset_tiers();
		return count;
	}

	parsed = sscanf(input, "tier set %u %15s", &uid_num, tier_name);
	if (parsed != 2)
		return -EINVAL;

	uid = make_kuid(&init_user_ns, uid_num);
	if (!uid_valid(uid))
		return -EINVAL;

	if (!strcmp(tier_name, "gold"))
		tier = FG_TIER_GOLD;
	else if (!strcmp(tier_name, "silver"))
		tier = FG_TIER_SILVER;
	else if (!strcmp(tier_name, "bronze"))
		tier = FG_TIER_BRONZE;
	else
		return -EINVAL;

	fork_guard_retier_uid(uid, tier);
	pr_info("fork_guard: tier updated for uid %u\n", uid_num);
	return count;
}

static const struct proc_ops fg_control_ops = {
	.proc_write = fg_control_write,
};

int fork_guard_init(void)
{
	fg_sysctl_header = register_sysctl("kernel/fork_guard",
					   fg_sysctl_table);
	if (!fg_sysctl_header) {
		pr_err("fork_guard: failed to register sysctl table\n");
		return -ENOMEM;
	}

	if (!proc_create("fork_guard_status", 0444, NULL,
			 &fg_status_ops)) {
		pr_err("fork_guard: failed to create /proc/fork_guard_status\\n");
		unregister_sysctl_table(fg_sysctl_header);
		fg_sysctl_header = NULL;
		return -ENOMEM;
	}
	if (!proc_create("fork_guard_control", 0200, NULL,
			 &fg_control_ops)) {
		pr_err("fork_guard: failed to create control interface\n");
		remove_proc_entry("fork_guard_status", NULL);
		unregister_sysctl_table(fg_sysctl_header);
		fg_sysctl_header = NULL;
		return -ENOMEM;
	}

	INIT_DELAYED_WORK(&fg_recovery_work, fg_recovery_worker);
	schedule_delayed_work(&fg_recovery_work, msecs_to_jiffies(250));

	pr_info("fork_guard: initialized\n");
	return 0;
}



void fork_guard_task_init(struct task_struct *task)
{
	/* Tier selection is applied in sched_fork(). */
}

enum fg_tier fork_guard_get_tier(kuid_t uid)
{
	unsigned long flags;
	enum fg_tier tier = FG_TIER_SILVER;
	int i;

	spin_lock_irqsave(&fg_tier_lock, flags);

	for (i = 0; i < FG_MAX_TIERS; i++) {
		if (fg_tiers[i].used && uid_eq(fg_tiers[i].uid, uid)) {
			tier = fg_tiers[i].tier;
			break;
		}
	}

	spin_unlock_irqrestore(&fg_tier_lock, flags);

	return tier;
}
void fork_guard_retier_uid(kuid_t uid, enum fg_tier tier)
{
	unsigned long flags;
	struct task_struct *p;
	kuid_t old_gold_uid = INVALID_UID;
	int i, free_slot = -1;
	int nice;

	if (tier < FG_TIER_BRONZE || tier > FG_TIER_GOLD)
		return;

	spin_lock_irqsave(&fg_tier_lock, flags);

	for (i = 0; i < FG_MAX_TIERS; i++) {
		if (!fg_tiers[i].used) {
			if (free_slot < 0)
				free_slot = i;
			continue;
		}

		if (uid_eq(fg_tiers[i].uid, uid))
			free_slot = -2;

		if (tier == FG_TIER_GOLD &&
		    fg_tiers[i].tier == FG_TIER_GOLD &&
		    !uid_eq(fg_tiers[i].uid, uid)) {
			old_gold_uid = fg_tiers[i].uid;
			fg_tiers[i].tier = FG_TIER_SILVER;
		}
	}

	if (free_slot >= 0) {
		fg_tiers[free_slot].used = true;
		fg_tiers[free_slot].uid = uid;
		fg_tiers[free_slot].tier = tier;
	} else {
		for (i = 0; i < FG_MAX_TIERS; i++) {
			if (fg_tiers[i].used &&
			    uid_eq(fg_tiers[i].uid, uid)) {
				fg_tiers[i].tier = tier;
				break;
			}
		}
	}

	spin_unlock_irqrestore(&fg_tier_lock, flags);

	if (uid_valid(old_gold_uid)) {
		rcu_read_lock();
		for_each_process(p) {
			if (uid_eq(task_uid(p), old_gold_uid) &&
			    (p->policy == SCHED_NORMAL ||
			     p->policy == SCHED_BATCH))
				set_user_nice(p, 0);
		}
		rcu_read_unlock();
	}

	switch (tier) {
	case FG_TIER_GOLD:
		nice = -7;
		break;
	case FG_TIER_BRONZE:
		nice = 5;
		break;
	default:
		nice = 0;
		break;
	}

	rcu_read_lock();
	for_each_process(p) {
		if (uid_eq(task_uid(p), uid) &&
		    (p->policy == SCHED_NORMAL ||
		     p->policy == SCHED_BATCH))
			set_user_nice(p, nice);
	}
	rcu_read_unlock();

	pr_info("fork_guard: updated tier for uid %u\n", __kuid_val(uid));
}

void fork_guard_reset_tiers(void)
{
	unsigned long flags;
	struct task_struct *p;

	/*
	 * Remove tier assignments. Do not alter nice values for users
	 * who were never assigned a custom tier.
	 */
	spin_lock_irqsave(&fg_tier_lock, flags);
	memset(fg_tiers, 0, sizeof(fg_tiers));
	spin_unlock_irqrestore(&fg_tier_lock, flags);

	rcu_read_lock();
	for_each_process(p) {
		if (uid_eq(task_uid(p), GLOBAL_ROOT_UID))
			continue;

		if (p->policy == SCHED_NORMAL ||
		    p->policy == SCHED_BATCH)
			set_user_nice(p, 0);
	}
	rcu_read_unlock();

	pr_info("fork_guard: all custom tiers reset to SILVER\n");
}
