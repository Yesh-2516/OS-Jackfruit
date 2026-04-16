#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/uaccess.h>
#include <linux/ioctl.h>
#include <linux/slab.h>
#include <linux/list.h>
#include <linux/sched.h>
#include <linux/sched/signal.h>
#include <linux/mm.h>
#include <linux/timer.h>
#include <linux/jiffies.h>
#include <linux/spinlock.h>
#include <linux/pid.h>
#include <linux/signal.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("PES2UG23CS606");
MODULE_DESCRIPTION("Container Memory Monitor LKM");
MODULE_VERSION("1.0");

/* ─── ioctl definitions (must match monitor_ioctl.h) ────────────────────── */
#define MON_MAGIC        'M'
#define CONTAINER_ID_MAX  32

struct monitor_request {
    pid_t         pid;
    unsigned long soft_limit_bytes;
    unsigned long hard_limit_bytes;
    char          container_id[CONTAINER_ID_MAX];
};

#define MONITOR_REGISTER   _IOW(MON_MAGIC, 1, struct monitor_request)
#define MONITOR_UNREGISTER _IOW(MON_MAGIC, 2, struct monitor_request)
#define IOCTL_SET_SOFT_LIMIT _IOW(MON_MAGIC, 3, unsigned long)
#define IOCTL_SET_HARD_LIMIT _IOW(MON_MAGIC, 4, unsigned long)
#define IOCTL_LIST_PIDS      _IOR(MON_MAGIC, 5, int)

/* ─── PID list entry ─────────────────────────────────────────────────────── */
struct pid_entry {
    pid_t            pid;
    struct list_head list;
};

/* ─── Module globals ─────────────────────────────────────────────────────── */
static int            major_number;
static struct class  *mon_class  = NULL;
static struct device *mon_device = NULL;
static struct cdev    mon_cdev;

static LIST_HEAD(pid_list);
static spinlock_t    pid_list_lock;
static int           pid_count   = 0;

static unsigned long soft_limit_kb = 50 * 1024;
static unsigned long hard_limit_kb = 100 * 1024;

static struct timer_list mon_timer;
#define CHECK_INTERVAL_SEC  5

/* ─── Helper: get RSS in kB ──────────────────────────────────────────────── */
static unsigned long get_rss_kb(pid_t pid)
{
    struct task_struct *task;
    struct mm_struct   *mm;
    unsigned long       rss = 0;

    rcu_read_lock();
    task = pid_task(find_vpid(pid), PIDTYPE_PID);
    if (!task) { rcu_read_unlock(); return 0; }
    mm = task->mm;
    if (mm)
        rss = get_mm_rss(mm) << (PAGE_SHIFT - 10);
    rcu_read_unlock();
    return rss;
}

/* ─── Helper: send signal ────────────────────────────────────────────────── */
static void signal_pid(pid_t pid, int sig)
{
    struct pid        *kpid;
    struct task_struct *task;

    rcu_read_lock();
    kpid = find_vpid(pid);
    if (kpid) {
        task = pid_task(kpid, PIDTYPE_PID);
        if (task) send_sig(sig, task, 1);
    }
    rcu_read_unlock();
}

/* ─── Timer callback ─────────────────────────────────────────────────────── */
static void monitor_timer_callback(struct timer_list *t)
{
    struct pid_entry *entry;
    unsigned long     rss_kb;
    unsigned long     flags;

    spin_lock_irqsave(&pid_list_lock, flags);
    list_for_each_entry(entry, &pid_list, list) {
        rss_kb = get_rss_kb(entry->pid);
        if (rss_kb == 0) continue;

        if (rss_kb >= hard_limit_kb) {
            printk(KERN_WARNING
                   "monitor: PID %d exceeded HARD limit "
                   "(%lu kB / %lu kB) — SIGKILL\n",
                   entry->pid, rss_kb, hard_limit_kb);
            signal_pid(entry->pid, SIGKILL);
        } else if (rss_kb >= soft_limit_kb) {
            printk(KERN_NOTICE
                   "monitor: PID %d exceeded SOFT limit "
                   "(%lu kB / %lu kB) — warning\n",
                   entry->pid, rss_kb, soft_limit_kb);
        } else {
            printk(KERN_DEBUG
                   "monitor: PID %d OK (%lu kB / %lu kB)\n",
                   entry->pid, rss_kb, hard_limit_kb);
        }
    }
    spin_unlock_irqrestore(&pid_list_lock, flags);
    mod_timer(&mon_timer, jiffies + CHECK_INTERVAL_SEC * HZ);
}

/* ─── ioctl handler ──────────────────────────────────────────────────────── */
static long mon_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    struct monitor_request mreq;
    unsigned long           limit_kb;
    unsigned long           flags;
    struct pid_entry       *entry, *tmp;
    int                     found;

    switch (cmd) {

    case MONITOR_REGISTER:
        if (copy_from_user(&mreq, (struct monitor_request __user *)arg,
                           sizeof(mreq)))
            return -EFAULT;
        spin_lock_irqsave(&pid_list_lock, flags);
        found = 0;
        list_for_each_entry(entry, &pid_list, list)
            if (entry->pid == mreq.pid) { found = 1; break; }
        if (!found) {
            entry = kmalloc(sizeof(*entry), GFP_ATOMIC);
            if (!entry) { spin_unlock_irqrestore(&pid_list_lock, flags); return -ENOMEM; }
            entry->pid = mreq.pid;
            list_add_tail(&entry->list, &pid_list);
            pid_count++;
            if (mreq.soft_limit_bytes) soft_limit_kb = mreq.soft_limit_bytes / 1024;
            if (mreq.hard_limit_bytes) hard_limit_kb = mreq.hard_limit_bytes / 1024;
            printk(KERN_INFO "monitor: registered '%s' PID %d (soft=%lu kB hard=%lu kB total=%d)\n",
                   mreq.container_id, mreq.pid, soft_limit_kb, hard_limit_kb, pid_count);
        }
        spin_unlock_irqrestore(&pid_list_lock, flags);
        return 0;

    case MONITOR_UNREGISTER:
        if (copy_from_user(&mreq, (struct monitor_request __user *)arg,
                           sizeof(mreq)))
            return -EFAULT;
        spin_lock_irqsave(&pid_list_lock, flags);
        list_for_each_entry_safe(entry, tmp, &pid_list, list) {
            if (entry->pid == mreq.pid) {
                list_del(&entry->list);
                kfree(entry);
                pid_count--;
                printk(KERN_INFO "monitor: unregistered '%s' PID %d (total=%d)\n",
                       mreq.container_id, mreq.pid, pid_count);
                break;
            }
        }
        spin_unlock_irqrestore(&pid_list_lock, flags);
        return 0;

    case IOCTL_SET_SOFT_LIMIT:
        if (copy_from_user(&limit_kb, (unsigned long __user *)arg, sizeof(unsigned long)))
            return -EFAULT;
        soft_limit_kb = limit_kb;
        printk(KERN_INFO "monitor: soft limit set to %lu kB\n", soft_limit_kb);
        return 0;

    case IOCTL_SET_HARD_LIMIT:
        if (copy_from_user(&limit_kb, (unsigned long __user *)arg, sizeof(unsigned long)))
            return -EFAULT;
        hard_limit_kb = limit_kb;
        printk(KERN_INFO "monitor: hard limit set to %lu kB\n", hard_limit_kb);
        return 0;

    case IOCTL_LIST_PIDS:
        spin_lock_irqsave(&pid_list_lock, flags);
        list_for_each_entry(entry, &pid_list, list)
            printk(KERN_INFO "monitor:   tracked PID %d\n", entry->pid);
        spin_unlock_irqrestore(&pid_list_lock, flags);
        return pid_count;

    default:
        return -EINVAL;
    }
}

/* ─── file_operations ────────────────────────────────────────────────────── */
static int mon_open   (struct inode *i, struct file *f) { return 0; }
static int mon_release(struct inode *i, struct file *f) { return 0; }

static const struct file_operations mon_fops = {
    .owner          = THIS_MODULE,
    .open           = mon_open,
    .release        = mon_release,
    .unlocked_ioctl = mon_ioctl,
};

/* ─── init ───────────────────────────────────────────────────────────────── */
static int __init monitor_init(void)
{
    dev_t dev;
    int   ret;

    printk(KERN_INFO "monitor: loading module\n");
    spin_lock_init(&pid_list_lock);

    ret = alloc_chrdev_region(&dev, 0, 1, "container_monitor");
    if (ret < 0) { printk(KERN_ERR "monitor: alloc_chrdev_region failed\n"); return ret; }
    major_number = MAJOR(dev);

    cdev_init(&mon_cdev, &mon_fops);
    mon_cdev.owner = THIS_MODULE;
    ret = cdev_add(&mon_cdev, dev, 1);
    if (ret) { unregister_chrdev_region(dev, 1); return ret; }

    mon_class = class_create("container_monitor");
    if (IS_ERR(mon_class)) {
        cdev_del(&mon_cdev); unregister_chrdev_region(dev, 1);
        return PTR_ERR(mon_class);
    }

    mon_device = device_create(mon_class, NULL, dev, NULL, "container_monitor");
    if (IS_ERR(mon_device)) {
        class_destroy(mon_class); cdev_del(&mon_cdev);
        unregister_chrdev_region(dev, 1);
        return PTR_ERR(mon_device);
    }

    timer_setup(&mon_timer, monitor_timer_callback, 0);
    mod_timer(&mon_timer, jiffies + CHECK_INTERVAL_SEC * HZ);

    printk(KERN_INFO "monitor: ready major=%d soft=%lu kB hard=%lu kB interval=%ds\n",
           major_number, soft_limit_kb, hard_limit_kb, CHECK_INTERVAL_SEC);
    return 0;
}

/* ─── exit ───────────────────────────────────────────────────────────────── */
static void __exit monitor_exit(void)
{
    struct pid_entry *entry, *tmp;
    unsigned long     flags;

    printk(KERN_INFO "monitor: unloading\n");
    timer_delete_sync(&mon_timer);

    spin_lock_irqsave(&pid_list_lock, flags);
    list_for_each_entry_safe(entry, tmp, &pid_list, list) {
        list_del(&entry->list); kfree(entry);
    }
    spin_unlock_irqrestore(&pid_list_lock, flags);

    device_destroy(mon_class, MKDEV(major_number, 0));
    class_destroy(mon_class);
    cdev_del(&mon_cdev);
    unregister_chrdev_region(MKDEV(major_number, 0), 1);
    printk(KERN_INFO "monitor: unloaded\n");
}

module_init(monitor_init);
module_exit(monitor_exit);