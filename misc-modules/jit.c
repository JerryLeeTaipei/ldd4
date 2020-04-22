/*
 * jit.c -- the just-in-time module
 *
 * Copyright (C) 2001,2003 Alessandro Rubini and Jonathan Corbet
 * Copyright (C) 2001,2003 O'Reilly & Associates
 *
 * The source code in this file can be freely used, adapted,
 * and redistributed in source or binary form, so long as an
 * acknowledgment appears in derived source files.  The citation
 * should list that the code comes from the book "Linux Device
 * Drivers" by Alessandro Rubini and Jonathan Corbet, published
 * by O'Reilly & Associates.   No warranty is attached;
 * we cannot take responsibility for errors or fitness for use.
 *
 */

#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/init.h>

#include <linux/time.h>
#include <linux/timer.h>
#include <linux/kernel.h>
#include <linux/proc_fs.h>
#include <linux/types.h>
#include <linux/spinlock.h>
#include <linux/interrupt.h>
#include <linux/sched.h>
#include <linux/seq_file.h>
#include <asm/hardirq.h>

#include <linux/slab.h>
#include <linux/uaccess.h>
/*
 * This module is a silly one: it only embeds short code fragments
 * that show how time delays can be handled in the kernel.
 */

#define JIT_ASYNC_LOOPS 5

int delay = HZ; /* The default delay, expressed in jiffies. */
module_param(delay, int, 0);

int tdelay = 10;
module_param(tdelay, int, 0);

MODULE_AUTHOR("Alessandro Rubini");
MODULE_LICENSE("Dual BSD/GPL");


#if 0


/*
 * This file, on the other hand, returns the current time forever.
 */
int jit_currentime(char *buf, char **start, off_t offset,
                   int len, int *eof, void *data)
{
	struct timeval tv1;
	struct timespec tv2;
	unsigned long j1;
	u64 j2;

	/* Get the time in 4 different ways. */
	j1 = jiffies;
	j2 = get_jiffies_64();
	do_gettimeofday(&tv1);
	tv2 = current_kernel_time();

	/* Print. */
	len = 0;
	len += sprintf(buf,"0x%08lx 0x%016Lx %10i.%06i\n"
		       "%40i.%09i\n",
		       j1, j2,
		       (int)tv1.tv_sec, (int)tv1.tv_usec,
		       (int)tv2.tv_sec, (int)tv2.tv_nsec);
	*start = buf;
	return len;
}

/*
 * The timer example follows.
 */

/* This data structure is used as "data" for the timer and tasklet functions. */
struct jit_data {
	struct timer_list timer;
	struct tasklet_struct tlet;
	int hi; /* tasklet or tasklet_hi */
	wait_queue_head_t wait;
	unsigned long prevjiffies;
	unsigned char *buf;
	int loops;
};


void jit_timer_fn(unsigned long arg)
{
	struct jit_data *data = (struct jit_data *)arg;
	unsigned long j = jiffies;
	data->buf += sprintf(data->buf, "%9li  %3li     %i    %6i   %i   %s\n",
			     j, j - data->prevjiffies, in_interrupt() ? 1 : 0,
			     current->pid, smp_processor_id(), current->comm);

	if (--data->loops) {
		data->timer.expires += tdelay;
		data->prevjiffies = j;
		add_timer(&data->timer);
	} else {
		wake_up_interruptible(&data->wait);
	}
}

/* the /proc function: allocate everything to allow concurrency. */
int jit_timer(char *buf, char **start, off_t offset,
	      int len, int *eof, void *unused_data)
{
	struct jit_data *data;
	char *buf2 = buf;
	unsigned long j = jiffies;

	data = kmalloc(sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	init_timer(&data->timer);
	init_waitqueue_head (&data->wait);

	/* Write the first lines in the buffer. */
	buf2 += sprintf(buf2, "   time   delta  inirq    pid   cpu command\n");
	buf2 += sprintf(buf2, "%9li  %3li     %i    %6i   %i   %s\n",
			j, 0L, in_interrupt() ? 1 : 0,
			current->pid, smp_processor_id(), current->comm);

	/* Fill the data for our timer function. */
	data->prevjiffies = j;
	data->buf = buf2;
	data->loops = JIT_ASYNC_LOOPS;
	
	/* Register the timer. */
	data->timer.data = (unsigned long)data;
	data->timer.function = jit_timer_fn;
	data->timer.expires = j + tdelay; /* Parameter. */
	add_timer(&data->timer);

	/* Wait for the buffer to fill. */
	wait_event_interruptible(data->wait, !data->loops);
	if (signal_pending(current))
		return -ERESTARTSYS;
	buf2 = data->buf;
	kfree(data);
	*eof = 1;
	return buf2 - buf;
}

void jit_tasklet_fn(unsigned long arg)
{
	struct jit_data *data = (struct jit_data *)arg;
	unsigned long j = jiffies;
	data->buf += sprintf(data->buf, "%9li  %3li     %i    %6i   %i   %s\n",
			     j, j - data->prevjiffies, in_interrupt() ? 1 : 0,
			     current->pid, smp_processor_id(), current->comm);

	if (--data->loops) {
		data->prevjiffies = j;
		if (data->hi)
			tasklet_hi_schedule(&data->tlet);
		else
			tasklet_schedule(&data->tlet);
	} else {
		wake_up_interruptible(&data->wait);
	}
}

/* The /proc function: allocate everything to allow concurrency. */
int jit_tasklet(char *buf, char **start, off_t offset,
	      int len, int *eof, void *arg)
{
	struct jit_data *data;
	char *buf2 = buf;
	unsigned long j = jiffies;
	long hi = (long)arg;

	data = kmalloc(sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	init_waitqueue_head(&data->wait);

	/* Write the first lines in the buffer. */
	buf2 += sprintf(buf2, "   time   delta  inirq    pid   cpu command\n");
	buf2 += sprintf(buf2, "%9li  %3li     %i    %6i   %i   %s\n",
			j, 0L, in_interrupt() ? 1 : 0,
			current->pid, smp_processor_id(), current->comm);

	/* Fill the data for our tasklet function. */
	data->prevjiffies = j;
	data->buf = buf2;
	data->loops = JIT_ASYNC_LOOPS;
	
	/* register the tasklet */
	tasklet_init(&data->tlet, jit_tasklet_fn, (unsigned long)data);
	data->hi = hi;
	if (hi)
		tasklet_hi_schedule(&data->tlet);
	else
		tasklet_schedule(&data->tlet);

	/* Wait for the buffer to fill. */
	wait_event_interruptible(data->wait, !data->loops);

	if (signal_pending(current))
		return -ERESTARTSYS;
	buf2 = data->buf;
	kfree(data);
	*eof = 1;
	return buf2 - buf;
}


#endif
static struct proc_dir_entry *proc_jit_dir;




static ssize_t mywrite(struct file *file, const char __user *ubuf,size_t count, loff_t *ppos) 
{
	printk( KERN_INFO "write handler\n");
	return -1;
}

#define BUFSIZE 21

/* Use these as data pointers, to implement four files in one function. */
enum jit_files {
	JIT_BUSY=1,
	JIT_SCHED,
	JIT_QUEUE,
	JIT_SCHEDTO
};

typedef struct jit_device	jit_device_t;
struct jit_device{
	int	type;
	wait_queue_head_t	wq;
};

static int myopen(struct inode *inode , struct file *file){
	struct dentry *dentry;	
	jit_device_t *private_data;

	dentry = file->f_path.dentry;
	printk( KERN_INFO "file name=%s\n", dentry->d_name.name);

	private_data = (jit_device_t *) kmalloc( sizeof(jit_device_t), GFP_KERNEL);
	// set type
	if ( !strncmp("jitbusy", dentry->d_name.name, 7) )
		private_data->type = JIT_BUSY;
	else if ( !strncmp("jitsched", dentry->d_name.name, 7) )
		private_data->type = JIT_SCHED;
	else if ( !strncmp("jitqueue", dentry->d_name.name, 7) )
		private_data->type = JIT_QUEUE;
	else if ( !strncmp("jitschedto", dentry->d_name.name, 7) )
		private_data->type = JIT_SCHEDTO;
	else
		private_data->type = 0;
	// init wqit queue head
	init_waitqueue_head(&(private_data->wq));

	file->private_data = private_data;
	return 0;
}

static int myrelease(struct inode *inode , struct file *file){

	printk( KERN_INFO "release\n");
	if ( file->private_data )
		kfree(file->private_data);

	return 0;
}

/*
 * This function prints one line of data, after sleeping one second.
 * It can sleep in different ways, according to the data pointer
 */
static ssize_t myread(struct file *file, char __user *ubuf, size_t count, loff_t *ppos) 
{
	unsigned long j0, j1; /* Jiffies. */
	jit_device_t *private_data;

	char buf[BUFSIZE];
	int len=0;
	
	private_data = file->private_data;
	printk( KERN_INFO "read handler for type#%d\n", private_data->type );

	
	if(*ppos > 0 || count < BUFSIZE) {
		printk( KERN_INFO "myread(): failed, input pos(%lld), count(%d)\n", *ppos, count);
		return 0;
	}

	printk( KERN_INFO "To delay %d jiffies\n", delay);	
	j0 = jiffies;
	j1 = j0 + delay;

	if ( private_data->type == JIT_BUSY ) { 
		while ( time_before(jiffies, j1) )
			cpu_relax();
	} else if ( private_data->type == JIT_SCHED ) {
		while ( time_before(jiffies, j1) ) 
			schedule();
	} else if ( private_data->type == JIT_SCHEDTO ) {
		set_current_state(TASK_INTERRUPTIBLE);
		schedule_timeout(delay);
	} else if ( private_data->type == JIT_QUEUE ) {
		wait_event_interruptible_timeout(private_data->wq, 0, delay);
	} else
		printk( KERN_INFO "not supported type#%d\n", private_data->type);

	j1 = jiffies; /* Actual value after we delayed. */
	len = snprintf(buf, BUFSIZE, "%9li %9li\n", j0, j1);
	if (copy_to_user(ubuf,buf,len) )
		return -EFAULT;

	*ppos = len;
	printk( KERN_INFO "myread(end): %lu - %lu\n", j0, j1);
	return len;

}
static const struct file_operations jit_ops = {
	.owner = THIS_MODULE,
	.open = myopen,
	.release = myrelease,
	.read = myread,
	.write = mywrite,
};

static int __init jit_init(void)
{
    struct proc_dir_entry *entry;

	proc_jit_dir = proc_mkdir("jit", NULL);
	if ( !proc_jit_dir)
		return -ENOMEM;
		
	entry = proc_create("jitbusy", 0, proc_jit_dir, &jit_ops);
	if (entry == NULL)
	{
		printk(KERN_WARNING "Failed to register /proc/jit/jitbusy\n");
		return 0;
	}
	entry = proc_create("jitsched", 0, proc_jit_dir, &jit_ops);
	if (entry == NULL)
	{
		printk(KERN_WARNING "Failed to register /proc/jit/jitsched\n");
		return 0;
	}
	entry = proc_create("jitschedto", 0, proc_jit_dir, &jit_ops);
	if (entry == NULL)
	{
		printk(KERN_WARNING "Failed to register /proc/jit/jitschedto\n");
		return 0;
	}
	entry = proc_create("jitqueue", 0, proc_jit_dir, &jit_ops);
	if (entry == NULL)
	{
		printk(KERN_WARNING "Failed to register /proc/jit/jitqueue\n");
		return 0;
	}

	//create_proc_read_entry("currentime", 0, NULL, jit_currentime, NULL);
	//create_proc_read_entry("jitimer", 0, NULL, jit_timer, NULL);
	//create_proc_read_entry("jitasklet", 0, NULL, jit_tasklet, NULL);
	//create_proc_read_entry("jitasklethi", 0, NULL, jit_tasklet, (void *)1);

	return 0; /* Success. */
}

static void __exit jit_cleanup(void)
{
	//remove_proc_entry("currentime", NULL);



	//remove_proc_entry("jitimer", NULL);
	//remove_proc_entry("jitasklet", NULL);
	//remove_proc_entry("jitasklethi", NULL);
	remove_proc_entry("jitbusy", proc_jit_dir);
	remove_proc_entry("jitsched", proc_jit_dir);
	remove_proc_entry("jitqueue", proc_jit_dir);
	remove_proc_entry("jitschedto", proc_jit_dir);
	remove_proc_entry("jit", NULL);
}

module_init(jit_init);
module_exit(jit_cleanup);
