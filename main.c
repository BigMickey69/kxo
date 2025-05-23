/* kxo: A Tic-Tac-Toe Game Engine implemented as Linux kernel module */
// #define DEBUG    // For viewing pr_debug logs
#include <linux/cdev.h>
#include <linux/circ_buf.h>
#include <linux/container_of.h>
#include <linux/interrupt.h>
#include <linux/kfifo.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/sysfs.h>
#include <linux/version.h>
#include <linux/vmalloc.h>
#include <linux/workqueue.h>


#include "game.h"
#include "mcts.h"
#include "negamax.h"
#include "zobrist.h"

#include "gamecount.h"
#include "load.h"

static struct game games[MAX_GAMES];
static unsigned char check_won[MAX_GAMES];
static atomic_t won_count;

struct ai_work {
    struct work_struct work;
    struct game *game;
};


static struct ai_work ai_one_works[MAX_GAMES];
static struct ai_work ai_two_works[MAX_GAMES];
static struct ai_work drawboard_works[MAX_GAMES];
static struct work_struct loadavg_works[MAX_GAMES];

static struct mutex game_lock[MAX_GAMES];
static DEFINE_MUTEX(read_lock);
static DEFINE_MUTEX(consumer_lock);



MODULE_LICENSE("Dual MIT/GPL");
MODULE_AUTHOR("National Cheng Kung University, Taiwan");
MODULE_DESCRIPTION("In-kernel Tic-Tac-Toe game engine v0.2");

/* Macro DECLARE_TASKLET_OLD exists for compatibiity.
 * See https://lwn.net/Articles/830964/
 */
#ifndef DECLARE_TASKLET_OLD
#define DECLARE_TASKLET_OLD(arg1, arg2) DECLARE_TASKLET(arg1, arg2, 0L)
#endif

#define DEV_NAME "kxo"

#define NR_KMLDRV 1

static int delay = 500; /* time (in ms) to generate an event */

/* Load average struct*/
static struct kxo_loadavg O_load_logs[MAX_GAMES];
static struct kxo_loadavg X_load_logs[MAX_GAMES];

/* Declare kernel module attribute for sysfs */
struct kxo_attr {
    char display;
    char resume;
    char end;
    rwlock_t lock;
};

static struct kxo_attr attr_obj;

static ssize_t kxo_state_show(struct device *dev,
                              struct device_attribute *attr,
                              char *buf)
{
    read_lock(&attr_obj.lock);
    int ret = snprintf(buf, 6, "%c %c %c\n", attr_obj.display, attr_obj.resume,
                       attr_obj.end);
    read_unlock(&attr_obj.lock);
    return ret;
}

static ssize_t kxo_state_store(struct device *dev,
                               struct device_attribute *attr,
                               const char *buf,
                               size_t count)
{
    write_lock(&attr_obj.lock);
    sscanf(buf, "%c %c %c", &(attr_obj.display), &(attr_obj.resume),
           &(attr_obj.end));
    write_unlock(&attr_obj.lock);
    return count;
}

static DEVICE_ATTR_RW(kxo_state);

/* Data produced by the simulated device */

/* Timer to simulate a periodic IRQ */
static struct timer_list timer;

/* Character device stuff */
static int major;
static struct class *kxo_class;
static struct cdev kxo_cdev;

/* Data are stored into a kfifo buffer before passing them to the userspace */
static DECLARE_KFIFO_PTR(rx_fifo, unsigned char);

/* NOTE: the usage of kfifo is safe (no need for extra locking), until there is
 * only one concurrent reader and one concurrent writer. Writes are serialized
 * from the interrupt context, readers are serialized using this mutex.
 */


/* Wait queue to implement blocking I/O from userspace */
static DECLARE_WAIT_QUEUE_HEAD(rx_wait);



/* Workqueue for asynchronous bottom-half processing */
static struct workqueue_struct *kxo_workqueue;



// LSB indicates 'O' or 'X'
static void produce_board(const struct game *g)
{
    unsigned char messenger[2];
    mutex_lock(&consumer_lock);

    messenger[0] = g->id;
    messenger[1] = g->last_move << 1 | (g->turn == 'O' ? 1 : 0);
    unsigned int len = kfifo_in(&rx_fifo, messenger, sizeof(messenger));

    mutex_unlock(&consumer_lock);

    if (unlikely(len < sizeof(messenger)) && printk_ratelimit())
        pr_warn("%s: %zu bytes dropped\n", __func__, sizeof(messenger) - len);

    pr_debug("kxo: %s: in %u/%u bytes\n", __func__, len, kfifo_len(&rx_fifo));
}

/* Mutex to serialize fast_buf consumers: we can use a mutex because consumers
 * run in workqueue handler (kernel thread context).
 */


/* We use an additional "faster" circular buffer to quickly store data from
 * interrupt context, before adding them to the kfifo.
 */
static struct circ_buf fast_buf;



/* Clear all data from the circular buffer fast_buf */
static void fast_buf_clear(void)
{
    fast_buf.head = fast_buf.tail = 0;
}

/* Workqueue handler: executed by a kernel thread */
static void drawboard_work_func(struct game *g)
{
    int cpu;

    /* This code runs from a kernel thread, so softirqs and hard-irqs must
     * be enabled.
     */
    WARN_ON_ONCE(in_softirq());
    WARN_ON_ONCE(in_interrupt());

    /* Pretend to simulate access to per-CPU data, disabling preemption
     * during the pr_info().
     */
    cpu = get_cpu();
    pr_info("kxo: [CPU#%d] %s\n", cpu, __func__);
    put_cpu();

    read_lock(&attr_obj.lock);
    if (attr_obj.display == '0') {
        read_unlock(&attr_obj.lock);
        return;
    }
    read_unlock(&attr_obj.lock);

    produce_board(g);

    wake_up_interruptible(&rx_wait);
}


// MCTS algo is 'O'
static void ai_one_work_func(struct work_struct *w)
{
    ktime_t tv_start, tv_end;
    s64 nsecs;

    int cpu;

    WARN_ON_ONCE(in_softirq());
    WARN_ON_ONCE(in_interrupt());

    cpu = get_cpu();
    pr_info("kxo: [CPU#%d] start doing %s\n", cpu, __func__);
    tv_start = ktime_get();

    struct ai_work *work = container_of(w, struct ai_work, work);
    struct game *g = work->game;


    mutex_lock(&game_lock[g->id]);
    if (unlikely(check_won[g->id]))
        goto exit;

    int move;
    WRITE_ONCE(move, mcts(g->table, 'O'));

    smp_mb();

    if (move != -1) {
        WRITE_ONCE(g->table[move], 'O');
        WRITE_ONCE(g->last_move, move);
    }

    WRITE_ONCE(g->turn, 'X');
    WRITE_ONCE(g->finish, 1);
    smp_wmb();

    if (check_win(g->table) != ' ') {
        WRITE_ONCE(check_won[g->id], 1);
        atomic_inc(&won_count);
    }

    smp_wmb();
    drawboard_work_func(g);

exit:
    mutex_unlock(&game_lock[g->id]);
    tv_end = ktime_get();
    nsecs = (s64) ktime_to_ns(ktime_sub(tv_end, tv_start));

    // log time
    O_load_logs[g->id].active_nsec += nsecs;

    pr_info("kxo: [CPU#%d] did %s for %llu usec(game %u)\n", cpu, __func__,
            (unsigned long long) nsecs >> 10, g->id + 1);
    put_cpu();
}

// negamax algo is 'X'
static void ai_two_work_func(struct work_struct *w)
{
    ktime_t tv_start, tv_end;
    s64 nsecs;

    int cpu;

    WARN_ON_ONCE(in_softirq());
    WARN_ON_ONCE(in_interrupt());

    cpu = get_cpu();
    pr_info("kxo: [CPU#%d] start doing %s\n", cpu, __func__);
    tv_start = ktime_get();

    struct ai_work *work = container_of(w, struct ai_work, work);
    struct game *g = work->game;

    mutex_lock(&game_lock[g->id]);

    if (unlikely(check_won[g->id]))
        goto exit;

    int move;
    WRITE_ONCE(move, negamax_predict(g->table, 'X').move);

    smp_mb();

    if (move != -1) {
        WRITE_ONCE(g->table[move], 'X');
        WRITE_ONCE(g->last_move, move);
    }

    WRITE_ONCE(g->turn, 'O');
    WRITE_ONCE(g->finish, 1);
    smp_wmb();

    if (check_win(g->table) != ' ') {
        WRITE_ONCE(check_won[g->id], 1);
        atomic_inc(&won_count);
    }

    smp_wmb();
    drawboard_work_func(g);

exit:
    mutex_unlock(&game_lock[g->id]);
    tv_end = ktime_get();

    nsecs = (s64) ktime_to_ns(ktime_sub(tv_end, tv_start));

    // log time
    X_load_logs[g->id].active_nsec += nsecs;

    pr_info("kxo: [CPU#%d] did %s for %llu usec\n", cpu, __func__,
            (unsigned long long) nsecs >> 10);
    put_cpu();
}


static void loadavg_work_func(struct work_struct *w)
{
    read_lock(&attr_obj.lock);
    if (attr_obj.display == '0') {
        read_unlock(&attr_obj.lock);
        return;
    }
    read_unlock(&attr_obj.lock);

    // smart way to get game_id from memory layout
    int id = w - loadavg_works;

    const struct kxo_loadavg *o = &O_load_logs[id];
    const struct kxo_loadavg *x = &X_load_logs[id];

    unsigned int o5 = min((o->avg_5s * 200) >> FSHIFT, 200ul);
    unsigned int x5 = min((x->avg_5s * 200) >> FSHIFT, 200ul);

    unsigned char msg[2];

    mutex_lock(&consumer_lock);

    // send l1
    msg[0] = 0b01000000 | id;
    msg[1] = (unsigned char) o5;
    kfifo_in(&rx_fifo, msg, 2);

    // send l5 & l15
    msg[0] = (unsigned char) x5;
    msg[1] = 0;
    kfifo_in(&rx_fifo, msg, 2);

    mutex_unlock(&consumer_lock);

    wake_up_interruptible(&rx_wait);
}

/* Tasklet handler.
 *
 * NOTE: different tasklets can run concurrently on different processors, but
 * two of the same type of tasklet cannot run simultaneously. Moreover, a
 * tasklet always runs on the same CPU that schedules it.
 */
static void game_tasklet_func(unsigned long __data)
{
    pr_info("kxo: started game_tasklet_func...\n");
    // the games have finished
    if (atomic_read(&won_count) == game_count) {
        atomic_set(&won_count, 0);
        unsigned char reset_msg[2];
        reset_msg[0] = 0b10000000;
        reset_msg[1] = 0;

        mutex_lock(&consumer_lock);
        kfifo_in(&rx_fifo, reset_msg, sizeof(reset_msg));
        mutex_unlock(&consumer_lock);

        wake_up_interruptible(&rx_wait);

        memset(check_won, 0, sizeof(check_won));
        for (int i = 0; i < game_count; i++) {
            memset(games[i].table, ' ', N_GRIDS);
            games[i].finish = 1;
            games[i].turn = 'O';
        }
        return;  // return early
    }


    for (int i = 0; i < game_count; i++) {
        struct game *g = &games[i];

        int finish = READ_ONCE(g->finish);
        char turn = READ_ONCE(g->turn);

        if (finish && turn == 'O') {
            WRITE_ONCE(g->finish, 0);
            queue_work(kxo_workqueue, &ai_one_works[i].work);
        } else if (finish && turn == 'X') {
            WRITE_ONCE(g->finish, 0);
            queue_work(kxo_workqueue, &ai_two_works[i].work);
        }
        // queue_work(kxo_workqueue, &drawboard_works[i].work);
    }
}

/* Tasklet for asynchronous bottom-half processing in softirq context */
static DECLARE_TASKLET_OLD(game_tasklet, game_tasklet_func);



static void update_load(struct kxo_loadavg *stat, s64 total_nsec)
{
    unsigned long ratio;

    if (total_nsec == 0) {
        stat->active_nsec = 0;
        return;
    }

    ratio = div_u64((u64) stat->active_nsec * FIXED_1, total_nsec);

    stat->avg_5s =
        (stat->avg_5s * EXP_5S + ratio * (FIXED_1 - EXP_5S)) >> FSHIFT;

    stat->active_nsec = 0;
}


static void timer_handler(struct timer_list *__timer)
{
    ktime_t tv_start, tv_end;
    s64 nsecs;
    static ktime_t last_tick_time;
    s64 delta;

    pr_info("kxo: [CPU#%d] enter %s\n", smp_processor_id(), __func__);
    /* We are using a kernel timer to simulate a hard-irq, so we must expect
     * to be in softirq context here.
     */
    WARN_ON_ONCE(!in_softirq());

    /* Disable interrupts for this CPU to simulate real interrupt context */
    local_irq_disable();

    tv_start = ktime_get();

    // calculate delta
    if (unlikely(!last_tick_time))
        delta = 0;
    else
        delta = ktime_to_ns(ktime_sub(tv_start, last_tick_time));
    last_tick_time = tv_start;

    for (int i = 0; i < game_count; i++) {
        update_load(&O_load_logs[i], delta);
        update_load(&X_load_logs[i], delta);
        queue_work(kxo_workqueue, &loadavg_works[i]);
    }

    // Just mod the next timer.
    tasklet_schedule(&game_tasklet);
    mod_timer(&timer, jiffies + msecs_to_jiffies(delay));


    tv_end = ktime_get();
    nsecs = (s64) ktime_to_ns(ktime_sub(tv_end, tv_start));

    pr_info("kxo: [CPU#%d] %s in_irq: %llu usec\n", smp_processor_id(),
            __func__, (unsigned long long) nsecs >> 10);

    local_irq_enable();
}

static ssize_t kxo_read(struct file *file,
                        char __user *buf,
                        size_t count,
                        loff_t *ppos)
{
    unsigned int read;
    int ret;

    pr_debug("kxo: %s(%p, %zd, %lld)\n", __func__, buf, count, *ppos);

    if (unlikely(!access_ok(buf, count)))
        return -EFAULT;

    if (mutex_lock_interruptible(&read_lock))
        return -ERESTARTSYS;

    do {
        ret = kfifo_to_user(&rx_fifo, buf, count, &read);
        if (unlikely(ret < 0))
            break;
        if (read)
            break;
        if (file->f_flags & O_NONBLOCK) {
            ret = -EAGAIN;
            break;
        }
        ret = wait_event_interruptible(rx_wait, kfifo_len(&rx_fifo));
    } while (ret == 0);
    pr_debug("kxo: %s: out %u/%u bytes\n", __func__, read, kfifo_len(&rx_fifo));

    mutex_unlock(&read_lock);

    return ret ? ret : read;
}

static atomic_t open_cnt;

static int kxo_open(struct inode *inode, struct file *filp)
{
    pr_debug("kxo: %s\n", __func__);
    if (atomic_inc_return(&open_cnt) == 1)
        mod_timer(&timer, jiffies + msecs_to_jiffies(delay));
    pr_info("open current cnt: %d\n", atomic_read(&open_cnt));

    return 0;
}

static int kxo_release(struct inode *inode, struct file *filp)
{
    pr_debug("kxo: %s\n", __func__);
    if (atomic_dec_and_test(&open_cnt)) {
        del_timer_sync(&timer);
        flush_workqueue(kxo_workqueue);
        fast_buf_clear();
    }
    pr_info("release, current cnt: %d\n", atomic_read(&open_cnt));
    attr_obj.end = '0';

    return 0;
}


static const struct file_operations kxo_fops = {
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 4, 0)
    .owner = THIS_MODULE,
#endif
    .read = kxo_read,
    .llseek = no_llseek,
    .open = kxo_open,
    .release = kxo_release,
};


static int __init kxo_init(void)
{
    dev_t dev_id;
    int ret;

    if (kfifo_alloc(&rx_fifo, PAGE_SIZE, GFP_KERNEL) < 0)
        return -ENOMEM;

    /* Register major/minor numbers */
    ret = alloc_chrdev_region(&dev_id, 0, NR_KMLDRV, DEV_NAME);
    if (ret)
        goto error_alloc;
    major = MAJOR(dev_id);

    /* Add the character device to the system */
    cdev_init(&kxo_cdev, &kxo_fops);
    ret = cdev_add(&kxo_cdev, dev_id, NR_KMLDRV);
    if (ret) {
        kobject_put(&kxo_cdev.kobj);
        goto error_region;
    }

    /* Create a class structure */
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 4, 0)
    kxo_class = class_create(THIS_MODULE, DEV_NAME);
#else
    kxo_class = class_create(DEV_NAME);
#endif
    if (IS_ERR(kxo_class)) {
        printk(KERN_ERR "error creating kxo class\n");
        ret = PTR_ERR(kxo_class);
        goto error_cdev;
    }

    /* Register the device with sysfs */
    struct device *kxo_dev =
        device_create(kxo_class, NULL, MKDEV(major, 0), NULL, DEV_NAME);

    ret = device_create_file(kxo_dev, &dev_attr_kxo_state);
    if (ret < 0) {
        printk(KERN_ERR "failed to create sysfs file kxo_state\n");
        goto error_device;
    }

    /* Allocate fast circular buffer */
    fast_buf.buf = vmalloc(PAGE_SIZE);
    if (!fast_buf.buf) {
        ret = -ENOMEM;
        goto error_vmalloc;
    }

    /* Create the workqueue */
    kxo_workqueue = alloc_workqueue("kxod", WQ_UNBOUND, WQ_MAX_ACTIVE);
    if (!kxo_workqueue) {
        ret = -ENOMEM;
        goto error_workqueue;
    }
    negamax_init();
    mcts_init();

    // initialize every game
    for (int i = 0; i < MAX_GAMES; i++) {
        // init locks
        mutex_init(&game_lock[i]);

        games[i] = (struct game){.id = i, .turn = 'O', .finish = 1};
        memset(games[i].table, ' ', N_GRIDS);

        INIT_WORK(&ai_one_works[i].work, ai_one_work_func);
        ai_one_works[i].game = &games[i];

        INIT_WORK(&ai_two_works[i].work, ai_two_work_func);
        ai_two_works[i].game = &games[i];

        INIT_WORK(&loadavg_works[i], loadavg_work_func);
    }

    attr_obj.display = '1';
    attr_obj.resume = '1';
    attr_obj.end = '0';
    rwlock_init(&attr_obj.lock);
    /* Setup the timer */
    timer_setup(&timer, timer_handler, 0);
    atomic_set(&open_cnt, 0);

    pr_info("kxo: registered new kxo device: %d,%d\n", major, 0);
out:
    return ret;
error_workqueue:
    vfree(fast_buf.buf);
error_vmalloc:
    device_destroy(kxo_class, dev_id);
error_device:
    class_destroy(kxo_class);
error_cdev:
    cdev_del(&kxo_cdev);
error_region:
    unregister_chrdev_region(dev_id, NR_KMLDRV);
error_alloc:
    kfifo_free(&rx_fifo);
    goto out;
}
static void __exit kxo_exit(void)
{
    dev_t dev_id = MKDEV(major, 0);

    del_timer_sync(&timer);
    tasklet_kill(&game_tasklet);
    flush_workqueue(kxo_workqueue);
    destroy_workqueue(kxo_workqueue);
    vfree(fast_buf.buf);
    device_destroy(kxo_class, dev_id);
    class_destroy(kxo_class);
    cdev_del(&kxo_cdev);
    unregister_chrdev_region(dev_id, NR_KMLDRV);

    zobrist_free();

    kfifo_free(&rx_fifo);
    pr_info("kxo: unloaded\n");
}

module_init(kxo_init);
module_exit(kxo_exit);
