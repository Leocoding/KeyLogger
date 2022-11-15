#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/cdev.h>
#include <linux/uaccess.h>
#include <linux/fs.h>
#include <linux/device.h>
#include <linux/interrupt.h>
#include <linux/sched/clock.h>
#include <linux/list.h>
//#include <linux/completion.h>
#include <linux/spinlock.h>

// CONSTANT VARS

#define MODULE_NAME "psa_mod_char_dev"
#define CHAR_DEV_RANGE_DEVICES_NAME MODULE_NAME
#define CHAR_DEV_CLASS_NAME MODULE_NAME
#define CHAR_DEV_DEVICE_NAME MODULE_NAME
#define IRQ 1
#define COUNT 5
#define BUFFER_SIZE 1000

// GLOBAL VARS

static int major;
static int minor;
static int list_count = 0;
static char *buffer;

// Circular List

struct data {
    u64 timestamp;
    int irq;
    int code;
    struct list_head list;
};

LIST_HEAD(timestamp_list);

// SYNC VARS

static DEFINE_RWLOCK(lock);
//static DECLARE_COMPLETION(completion);

// CHAR DEVICE

struct output_dev_data {
    bool done;
};

int output_dev_open(struct inode *inode, struct file *filp) {
    struct output_dev_data *data = kzalloc(sizeof(*data), GFP_KERNEL);
    if (data == NULL) {
        return -ENOMEM;
    }
    filp->private_data = data;
    return 0;
}

ssize_t output_dev_read(struct file *filp, char __user *buf, size_t count, loff_t *f_pos) {
	size_t l;
    struct output_dev_data *data;
    struct data *cell;
	size_t pos;
	
    // Buffer reset
	memset(buffer, 0, BUFFER_SIZE - 1);
	  // Synchronisation lecture utilisateur
    /* if (list_empty(&timestamp_list)) {
            wait_for_completion(&completion);
    }*/
	pos = 0;

	  // Section critique - lecture
	read_lock(&lock);
    list_for_each_entry(cell, &timestamp_list, list) {
		// Creation chaine de caractère et vidage du tampon circulaire
        pos += snprintf(buffer+pos, BUFFER_SIZE, "irq %d received at %llu (code : %d)\n", cell->irq, cell->timestamp, cell->code);
	}
	read_unlock(&lock);
	write_lock(&lock);
	while(!list_empty(&timestamp_list)) {
	    cell = list_entry(timestamp_list.next, struct data, list);
		list_del(&cell->list);
		kfree(cell);
		list_count--;
	}
	write_unlock(&lock);
	  
    // Reset synchronisation
	  /*if (list_count == 0) {
		    reinit_completion(&completion);
	  }*/

	// Traitement des opérations
    l = strlen(buffer) > count ? count : strlen(buffer);
    data = filp->private_data;
    if (data->done) {
        return 0;
    }
    if (copy_to_user(buf, buffer, l)) {
        return  -EFAULT;
    }
    data->done = 1;
    return l;
}

int output_dev_close(struct inode *inode, struct file *filp) {
    kfree(filp->private_data);
    return 0;
}

struct file_operations fops = {
    .owner = THIS_MODULE,
    .open  = output_dev_open,
    .release = output_dev_close,
    .read  = output_dev_read,
};

struct class *class_output;
dev_t devno;

// IRQs

static irqreturn_t irq_handler(int irq, void *dev) {
    // Log
    pr_info("%s : irq %d received at %llu (code : %d)\n", MODULE_NAME, irq, local_clock(), inb(0x60));
	return IRQ_WAKE_THREAD;
}

static irqreturn_t irq_thread(int irq, void *dev) {

	// Ajout d'un nouvel élément (+ajout structure)
	struct data *d = kmalloc(sizeof(*d), GFP_KERNEL);
	struct data *first;
	d->timestamp = local_clock();
	INIT_LIST_HEAD(&d->list);
	d->irq = irq;
	d->code = inb(0x60);
	// Récupération premier élément
	first  = list_entry(timestamp_list.next, struct data, list);

	  // Liste non pleine
    if (list_count < COUNT) {
		write_lock(&lock);
        list_add_tail(&d->list, &timestamp_list);
		write_unlock(&lock);
        list_count++;
    // Liste pleine (rotation des éléments)
    } else {
		write_lock(&lock);
        list_del(&first->list);
        list_add_tail(&d->list, &timestamp_list);
		write_unlock(&lock);
    }

	  // Synchronisation utilisateur
	  //completion_done(&completion);
    return IRQ_HANDLED;
}

// MODULE INIT & EXIT

static int __init timestamp_char_dev_init(void) {
    int err;
    static struct device *dev_output;

    // Allocation chaine
    buffer = kmalloc(BUFFER_SIZE, GFP_KERNEL);
    pr_info("Init de psa_mod\n");
    if (!buffer) {
        pr_err("Erreur Allocation");
        return -ENOMEM;
    }

    // Character Device

    if ((major = register_chrdev(0, CHAR_DEV_RANGE_DEVICES_NAME, &fops)) < 0) {
        err = major;
        goto err2;
    }

    class_output = class_create(THIS_MODULE, CHAR_DEV_CLASS_NAME);
    if (IS_ERR(class_output)) {
        err = PTR_ERR(class_output);
        goto err1;
    }

    devno = MKDEV(major, minor);
    dev_output = device_create(class_output, NULL, devno, NULL,
                             CHAR_DEV_DEVICE_NAME);
    if (IS_ERR(dev_output)) {
        err = PTR_ERR(dev_output);
        goto err0;
    }

    // IRQ Setup

    if (request_threaded_irq(IRQ, irq_handler, irq_thread, IRQF_SHARED, MODULE_NAME, irq_handler)) {
        pr_err("%s : cannot register IRQ %d\n", MODULE_NAME, IRQ);
        return -EIO;
    }
    return 0;

    err0:
        class_destroy(class_output);
    err1:
        unregister_chrdev(major, CHAR_DEV_RANGE_DEVICES_NAME);
    err2:
        return err;
}
module_init(timestamp_char_dev_init);

static void __exit timestamp_char_dev_exit(void) {
    struct data *cell;
    device_destroy(class_output, devno);
    class_destroy(class_output);
    unregister_chrdev(major, CHAR_DEV_RANGE_DEVICES_NAME);
    disable_irq(IRQ);
    free_irq(IRQ, irq_handler);
    enable_irq(IRQ);
    kfree(buffer);
    write_lock(&lock);
    while(!list_empty(&timestamp_list)) {
        cell = list_entry(timestamp_list.next, struct data, list);
        list_del(&cell->list);
        kfree(cell);
  	list_count--;
    }
    write_unlock(&lock);
    pr_info("Exit de psa_mod\n");
}
module_exit(timestamp_char_dev_exit);

MODULE_LICENSE ("GPL");
MODULE_AUTHOR("Léo Menudé");
MODULE_DESCRIPTION("Programmation Noyau Projet Keylogger");
