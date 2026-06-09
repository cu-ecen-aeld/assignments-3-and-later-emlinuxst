/**
 * @file aesdchar.c
 * @brief Functions and data related to the AESD char driver implementation
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/printk.h>
#include <linux/types.h>
#include <linux/cdev.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/string.h>
#include <linux/ioctl.h>

#include "aesdchar.h"
#include "aesd_ioctl.h"

int aesd_major = 0;
int aesd_minor = 0;

MODULE_AUTHOR("Stephen Bachelder");
MODULE_LICENSE("Dual BSD/GPL");

struct aesd_dev aesd_device;

static size_t aesd_get_buffer_size(struct aesd_circular_buffer *buffer)
{
    size_t total = 0;
    uint8_t index;
    struct aesd_buffer_entry *entry;

    AESD_CIRCULAR_BUFFER_FOREACH(entry, buffer, index) {
        if (entry->buffptr != NULL) {
            total += entry->size;
        }
    }

    return total;
}

static int aesd_seekto_position(struct aesd_circular_buffer *buffer,
                                uint32_t write_cmd,
                                uint32_t write_cmd_offset,
                                loff_t *new_pos)
{
    uint8_t index = buffer->out_offs;
    uint8_t count = 0;
    uint32_t command_number = 0;
    loff_t position = 0;

    while (count < AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED) {
        struct aesd_buffer_entry *entry = &buffer->entry[index];

        if (entry->buffptr != NULL) {
            if (command_number == write_cmd) {
                if (write_cmd_offset >= entry->size) {
                    return -EINVAL;
                }

                *new_pos = position + write_cmd_offset;
                return 0;
            }

            position += entry->size;
            command_number++;
        }

        index = (index + 1) % AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED;
        count++;
    }

    return -EINVAL;
}

int aesd_open(struct inode *inode, struct file *filp)
{
    struct aesd_dev *dev;

    PDEBUG("open");

    dev = container_of(inode->i_cdev, struct aesd_dev, cdev);
    filp->private_data = dev;

    return 0;
}

int aesd_release(struct inode *inode, struct file *filp)
{
    PDEBUG("release");
    return 0;
}

ssize_t aesd_read(struct file *filp, char __user *buf, size_t count,
                loff_t *f_pos)
{
    struct aesd_dev *dev = filp->private_data;
    struct aesd_buffer_entry *entry;
    size_t entry_offset;
    size_t bytes_available;
    size_t bytes_to_copy;
    size_t total_copied = 0;
    ssize_t retval = 0;

    PDEBUG("read %zu bytes with offset %lld", count, *f_pos);

    if (mutex_lock_interruptible(&dev->lock)) {
        return -ERESTARTSYS;
    }

    while (total_copied < count) {
        entry = aesd_circular_buffer_find_entry_offset_for_fpos(
            &dev->buffer,
            *f_pos,
            &entry_offset
        );

        if (!entry) {
            break;
        }

        bytes_available = entry->size - entry_offset;
        bytes_to_copy = min(count - total_copied, bytes_available);

        if (copy_to_user(buf + total_copied,
                         entry->buffptr + entry_offset,
                         bytes_to_copy)) {
            retval = -EFAULT;
            goto out;
        }

        *f_pos += bytes_to_copy;
        total_copied += bytes_to_copy;
    }

    retval = total_copied;

out:
    mutex_unlock(&dev->lock);
    return retval;
}

ssize_t aesd_write(struct file *filp, const char __user *buf, size_t count,
                loff_t *f_pos)
{
    struct aesd_dev *dev = filp->private_data;
    char *new_pending;
    char *newline;
    struct aesd_buffer_entry entry;
    ssize_t retval = count;

    PDEBUG("write %zu bytes with offset %lld", count, *f_pos);

    if (mutex_lock_interruptible(&dev->lock)) {
        return -ERESTARTSYS;
    }

    new_pending = krealloc(dev->pending_write,
                           dev->pending_write_size + count,
                           GFP_KERNEL);
    if (!new_pending) {
        retval = -ENOMEM;
        goto out;
    }

    dev->pending_write = new_pending;

    if (copy_from_user(dev->pending_write + dev->pending_write_size,
                       buf,
                       count)) {
        retval = -EFAULT;
        goto out;
    }

    dev->pending_write_size += count;

    newline = memchr(dev->pending_write, '\n', dev->pending_write_size);
    if (newline) {
        entry.buffptr = dev->pending_write;
        entry.size = dev->pending_write_size;

        if (dev->buffer.full) {
            kfree(dev->buffer.entry[dev->buffer.in_offs].buffptr);
            dev->buffer.entry[dev->buffer.in_offs].buffptr = NULL;
            dev->buffer.entry[dev->buffer.in_offs].size = 0;
        }

        aesd_circular_buffer_add_entry(&dev->buffer, &entry);

        dev->pending_write = NULL;
        dev->pending_write_size = 0;
    }

out:
    mutex_unlock(&dev->lock);
    return retval;
}

loff_t aesd_llseek(struct file *filp, loff_t offset, int whence)
{
    struct aesd_dev *dev = filp->private_data;
    loff_t new_pos;
    loff_t buffer_size;

    if (mutex_lock_interruptible(&dev->lock)) {
        return -ERESTARTSYS;
    }

    buffer_size = aesd_get_buffer_size(&dev->buffer);

    switch (whence) {
    case SEEK_SET:
        new_pos = offset;
        break;
    case SEEK_CUR:
        new_pos = filp->f_pos + offset;
        break;
    case SEEK_END:
        new_pos = buffer_size + offset;
        break;
    default:
        mutex_unlock(&dev->lock);
        return -EINVAL;
    }

    if (new_pos < 0 || new_pos > buffer_size) {
        mutex_unlock(&dev->lock);
        return -EINVAL;
    }

    filp->f_pos = new_pos;

    mutex_unlock(&dev->lock);
    return new_pos;
}

long aesd_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
    struct aesd_dev *dev = filp->private_data;
    struct aesd_seekto seekto;
    loff_t new_pos;
    long retval = 0;

    if (cmd != AESDCHAR_IOCSEEKTO) {
        return -ENOTTY;
    }

    if (copy_from_user(&seekto, (const void __user *)arg, sizeof(seekto))) {
        return -EFAULT;
    }

    if (mutex_lock_interruptible(&dev->lock)) {
        return -ERESTARTSYS;
    }

    retval = aesd_seekto_position(&dev->buffer,
                                  seekto.write_cmd,
                                  seekto.write_cmd_offset,
                                  &new_pos);
    if (retval == 0) {
        filp->f_pos = new_pos;
    }

    mutex_unlock(&dev->lock);
    return retval;
}

struct file_operations aesd_fops = {
    .owner = THIS_MODULE,
    .read = aesd_read,
    .write = aesd_write,
    .open = aesd_open,
    .release = aesd_release,
    .llseek = aesd_llseek,
    .unlocked_ioctl = aesd_ioctl,
};

static int aesd_setup_cdev(struct aesd_dev *dev)
{
    int err, devno = MKDEV(aesd_major, aesd_minor);

    cdev_init(&dev->cdev, &aesd_fops);
    dev->cdev.owner = THIS_MODULE;
    dev->cdev.ops = &aesd_fops;
    err = cdev_add(&dev->cdev, devno, 1);
    if (err) {
        printk(KERN_ERR "Error %d adding aesd cdev", err);
    }
    return err;
}

int aesd_init_module(void)
{
    dev_t dev = 0;
    int result;

    result = alloc_chrdev_region(&dev, aesd_minor, 1, "aesdchar");
    aesd_major = MAJOR(dev);
    if (result < 0) {
        printk(KERN_WARNING "Can't get major %d\n", aesd_major);
        return result;
    }

    memset(&aesd_device, 0, sizeof(struct aesd_dev));

    aesd_circular_buffer_init(&aesd_device.buffer);
    mutex_init(&aesd_device.lock);
    aesd_device.pending_write = NULL;
    aesd_device.pending_write_size = 0;

    result = aesd_setup_cdev(&aesd_device);

    if (result) {
        unregister_chrdev_region(dev, 1);
    }

    return result;
}

void aesd_cleanup_module(void)
{
    dev_t devno = MKDEV(aesd_major, aesd_minor);
    uint8_t index;
    struct aesd_buffer_entry *entry;

    cdev_del(&aesd_device.cdev);

    mutex_lock(&aesd_device.lock);

    AESD_CIRCULAR_BUFFER_FOREACH(entry, &aesd_device.buffer, index) {
        kfree(entry->buffptr);
        entry->buffptr = NULL;
        entry->size = 0;
    }

    kfree(aesd_device.pending_write);
    aesd_device.pending_write = NULL;
    aesd_device.pending_write_size = 0;

    mutex_unlock(&aesd_device.lock);
    mutex_destroy(&aesd_device.lock);

    unregister_chrdev_region(devno, 1);
}

module_init(aesd_init_module);
module_exit(aesd_cleanup_module);
