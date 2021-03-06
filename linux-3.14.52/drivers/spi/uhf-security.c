/*
 * UHF security module driver
 *
 * Author: Shao Depeng <dp.shao@gmail.com>
 * Copyright 2016 Golden Sky Technology CO.,LTD
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 *
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/init.h>
#include <linux/device.h>
#include <linux/cdev.h>
#include <linux/gpio.h>
#include <linux/workqueue.h>
#include <linux/semaphore.h>
#include <asm/cache.h>
#include <asm/uaccess.h>
#include <asm/atomic.h>
#include <linux/fs.h>
#include <linux/of_device.h>
#include <linux/of_gpio.h>
#include <linux/spi/spi.h>
#include <linux/delay.h>
#include <linux/of.h>
#include <linux/errno.h>
#include <linux/sched.h>
#include <linux/kthread.h>
#include <linux/timer.h>
#include <linux/spi/uhf-security.h>


struct uhf_security *__uhf = NULL;

static LIST_HEAD(device_list);
static DEFINE_MUTEX(device_list_lock);

/*-------------------------------------------------------------------------*/

static int us_init_gpio(struct uhf_security *uhf)
{
    int ret;
    /*
     * radio_reset   	gpio2-26  J4-6
     * status   		gpio3-21  J4-10
     * reset    		gpio3-22  J4-12
     */
    ret = devm_gpio_request_one(&uhf->spi->dev, uhf->radio_reset,
                                GPIOF_OUT_INIT_HIGH, "radio-reset");

    ret += devm_gpio_request_one(&uhf->spi->dev, uhf->reset,
                                GPIOF_OUT_INIT_HIGH, "uhf-reset");

    ret += devm_gpio_request_one(&uhf->spi->dev, uhf->status,
                                GPIOF_IN, "uhf-status");

    return ret;
}

static inline void us_reset(int reset)
{
    gpio_set_value(reset, GPIOF_INIT_HIGH);
    msleep(2);
    gpio_set_value(reset, GPIOF_INIT_LOW);
    msleep(3);
    gpio_set_value(reset, GPIOF_INIT_HIGH);
    printk(KERN_ALERT "reset security module\n");
}

static inline void us_reset_radio(int reset)
{
    gpio_set_value(reset, GPIOF_INIT_HIGH);
    msleep(1);
    gpio_set_value(reset, GPIOF_INIT_LOW);
    msleep(3);
    gpio_set_value(reset, GPIOF_INIT_HIGH);
    printk(KERN_ALERT "reset radio module\n");
}

static inline int us_get_status(int status)
{
    int value;

    value = gpio_get_value(status);
    if (value == 0)
        return OK;
    else
        return BUSY;
}

static inline int us_get_radio_status(int status)
{
    int value;

    value = gpio_get_value(status);
    if (value == 0)
        return OK;
    else
        return BUSY;
}

static int us_init(struct uhf_security *uhf)
{
	uhf->cache = devm_kzalloc(uhf->dev, sizeof(struct uhf_security_cache), GFP_KERNEL);
	if (NULL == uhf->cache) {
		printk(KERN_ALERT "Allocate cache memory failed.\n");
		return -ENOMEM;
	}

	uhf->cache->recv_buf = (unsigned long)devm_kzalloc(uhf->dev,
				sizeof(struct uhf_security_data) * UHF_CACHE_NUM, GFP_KERNEL);
	if (!uhf->cache->recv_buf) {
		printk(KERN_ALERT "Allocate recv buffer memory failed.\n");
		kfree(uhf->cache);
		uhf->cache = NULL;
		return -ENOMEM;
	}

	uhf->cache->recv_head = uhf->cache->recv_buf;
	uhf->cache->recv_tail = uhf->cache->recv_buf;

	init_waitqueue_head(&uhf->cache->inq);

	return us_init_gpio(uhf);
}

static inline void us_enable_irq(struct uhf_security *uhf)
{
    if (!uhf->irq_enabled) {
        uhf->irq_enabled = true;
        enable_irq(uhf->irq);
    } else {
        printk(KERN_ALERT "%s: irq has been enabled\n", __func__);
    }
}

static inline void us_disable_irq(struct uhf_security *uhf)
{
    if (uhf->irq_enabled) {
        uhf->irq_enabled = false;
        disable_irq_nosync(uhf->irq);
    } else {
        printk(KERN_ALERT "%s: irq has been disabled\n", __func__);
    }
}

/*-------------------------------------------------------------------------*/

/*
 * Atomicly increment the cache tail pointer
 */
static inline void us_incr_cache_tail(struct uhf_security *uhf, int delta)
{
	unsigned long newvalue;
	unsigned int n = delta / sizeof(struct uhf_security_data);

	if (delta % sizeof(struct uhf_security_data))
		delta = (n + 1) * sizeof(struct uhf_security_data);

	newvalue = uhf->cache->recv_tail + delta;
	barrier ();  /* Don't optimize these two together */
	if (newvalue >= (uhf->cache->recv_buf + UHF_CACHE_SIZE))
		uhf->cache->recv_tail = uhf->cache->recv_buf;
	else
		uhf->cache->recv_tail = newvalue;
}

/*
 * Atomicly increment the cache head pointer
 */
static inline void us_incr_cache_head(struct uhf_security *uhf, int delta)
{
	unsigned long newvalue;
	unsigned int n = delta / sizeof(struct uhf_security_data);

	if (delta % sizeof(struct uhf_security_data))
		delta = (n + 1) * sizeof(struct uhf_security_data);

	newvalue = uhf->cache->recv_head + delta;
	barrier ();  /* Don't optimize these two together */
	if (newvalue >= (uhf->cache->recv_buf + UHF_CACHE_SIZE))
		uhf->cache->recv_head = uhf->cache->recv_buf;
	else
		uhf->cache->recv_head = newvalue;
}

static inline void us_copy_to_cache(struct uhf_security *uhf, struct uhf_security_data datagram)
{
	// FIXME: memcpy could be datagram.len + 2
	// conside: If security module can send more than 1 packet
	memcpy((void *)uhf->cache->recv_head, &datagram, sizeof(struct uhf_security_data));
	us_incr_cache_head(uhf, sizeof(struct uhf_security_data));
	wake_up_interruptible(&(uhf->cache->inq)); /* awake any reading process */
}

/*-------------------------------------------------------------------------*/

static inline ssize_t us_sync_write(struct uhf_security *uhf, struct uhf_security_data *us_data)
{
    struct spi_transfer t = {
            .tx_buf     = us_data->data,
            .len        = us_data->len,
    };
    struct spi_message m;

    spi_message_init(&m);
    spi_message_add_tail(&t, &m);
    if (spi_sync(uhf->spi, &m) != 0 || m.status != 0)
		return 0;

	return (us_data->len - m.actual_length);
}

static inline ssize_t us_sync_read(struct uhf_security *uhf)
{
	struct uhf_security_data us_data;
    struct spi_transfer t = {
            .rx_buf     = us_data.data,
            .len        = UHF_SPI_MTU,
    };
    struct spi_message m;

    spi_message_init(&m);
    spi_message_add_tail(&t, &m);

    if (spi_sync(uhf->spi, &m) != 0 || m.status != 0)
		return 0;

	us_data.len = m.actual_length;

	us_copy_to_cache(uhf, us_data);

	return (UHF_SPI_MTU - m.actual_length);
}

/*-------------------------------------------------------------------------*/

static int us_open(struct inode *inode,struct file *filp)
{
    int ret = -ENXIO;
    struct uhf_security *uhf = NULL;

    mutex_lock(&device_list_lock);

    list_for_each_entry(uhf, &device_list, device_entry) {
        if (uhf->devt == inode->i_rdev) {
            ret = 0;
            break;
        }
    }

	if (ret == 0) {
		uhf->users++;
		filp->private_data = uhf;
		nonseekable_open(inode, filp);
		printk(KERN_ALERT "%s: successfully.\n", __func__);
	}

	uhf->cache->recv_head = uhf->cache->recv_buf;
	uhf->cache->recv_tail = uhf->cache->recv_buf;

    mutex_unlock(&device_list_lock);

    return ret;
}

static int us_release(struct inode *inode,struct file *filp)
{
    int ret = 0;
    struct uhf_security *uhf = NULL;

    mutex_lock(&device_list_lock);
    uhf = filp->private_data;

    filp->private_data = NULL;

    /* last close? */
    uhf->users--;
    if (!uhf->users) {
        int dofree;

        /* ... after we unbound from the underlying device? */
        spin_lock_irq(&uhf->lock);
        dofree = (uhf->spi == NULL);
        spin_unlock_irq(&uhf->lock);

        if (dofree)
            kfree(uhf);
    }
    mutex_unlock(&device_list_lock);

    return ret;
}

ssize_t us_read(struct file *filp, char __user *buf, size_t count, loff_t *f_pos)
{
    ssize_t count0 = 0;
    struct uhf_security *uhf = NULL;
	struct uhf_security_data *temp;

    if (count > UHF_SPI_MTU)
        return -EMSGSIZE;

    uhf = filp->private_data;

    if (down_interruptible(&uhf->sem))
		return -ERESTARTSYS;

	while (uhf->cache->recv_head == uhf->cache->recv_tail) { /* nothing to read */
		up(&uhf->sem);

		if (filp->f_flags & O_NONBLOCK)
			return -EAGAIN;

		if (wait_event_interruptible(uhf->cache->inq, (uhf->cache->recv_head != uhf->cache->recv_tail)))
			return -ERESTARTSYS; /* signal: tell the fs layer to handle it */

		/* otherwise loop, but first reacquire the lock */
		if (down_interruptible(&uhf->sem))
			return -ERESTARTSYS;
	}

	/* ok, data is there, return something */
	/* count0 is the number of readable data bytes */
	count0 = uhf->cache->recv_head - uhf->cache->recv_tail;
	if (count0 < 0) /* wrapped */
		count0 = uhf->cache->recv_buf + UHF_CACHE_SIZE - uhf->cache->recv_tail;

	if (count0 <= count)
		count = count0; /* Be sure count equal with sizeof(struct uhf_security_data). */

	temp = (struct uhf_security_data *)uhf->cache->recv_tail;
	temp->len = *(uint16_t *)(temp->data + 2) + 5;

	if (copy_to_user((uint8_t __user *)buf, (char *)temp->data, temp->len)) {
		up(&uhf->sem);
		return -EFAULT;
	}

	us_incr_cache_tail(uhf, count);
	up(&uhf->sem);

	return temp->len;
}

ssize_t us_write(struct file *filp, const char __user *buf, size_t count, loff_t *f_pos)
{
    ssize_t ret = 0;
    struct uhf_security *uhf = NULL;
	struct uhf_security_data temp;

    if (count > UHF_SPI_MTU)
        return -EMSGSIZE;

    uhf = filp->private_data;

	if(copy_from_user(temp.data, (const uint8_t __user *)(uintptr_t)buf, count))
		return -EFAULT;

	temp.len = count;

	if(down_interruptible(&uhf->sem))
		return -ERESTARTSYS;

    ret = us_sync_write(uhf, &temp);

    up(&uhf->sem);
    printk(KERN_ALERT "%s:ret:%d, count=%d\n", __func__, ret, count);

    return ret;
}

static long us_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	int ret = 0;
	struct uhf_security *uhf = filp->private_data;

	if (_IOC_TYPE(cmd) != US_IOC_MAGIC)
		return -ENOTTY;

	switch (cmd) {
		case US_IOC_RESET:
			us_reset(uhf->reset);
			break;
		case US_IOC_GET_STATUS:
			ret = us_get_status(uhf->status);
			break;
		case US_IOC_RESET_RADIO:
			us_reset_radio(uhf->radio_reset);
			break;
		case US_IOC_GET_RADIO_STATUS:
			ret = us_get_radio_status(uhf->radio_status);
			break;
		default:
			printk(KERN_ALERT "%s: no this cmd.\n", __func__);
	};

    return ret;
}

#ifdef CONFIG_COMPAT
static long us_compat_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
    return us_ioctl(filp, cmd, (unsigned long)compat_ptr(arg));
}
#else
#define us_compat_ioctl NULL
#endif /* CONFIG_COMPAT */

struct file_operations us_fops = {
    .owner =    THIS_MODULE,
    .read = us_read,
    .write = us_write,
    .unlocked_ioctl = us_ioctl,
    .compat_ioctl = us_compat_ioctl,
    .open = us_open,
    .release = us_release,
    .llseek = no_llseek,
};

/*-------------------------------------------------------------------------*/
static irqreturn_t us_intr_handler(int irq, void *handle)
{
	struct uhf_security *uhf = (struct uhf_security *)handle;

	us_sync_read(uhf);

    return IRQ_HANDLED;
}

static void us_calc_crc(struct uhf_security_data *data)
{
	int i;
	uint8_t crc = 0;

	for (i = 1; i < data->len - 1; i ++) {
		crc ^= *(data->data + i);
	}

	*(data->data + i) = crc;
}

static int us_stress_func(void *data)
{
	struct uhf_security *uhf = (struct uhf_security *)data;
	struct uhf_security_data us_data_1;
	struct uhf_security_data us_data_2;
	int index = 0;
	int data2_index = 0;
	uint64_t time = 0;
	struct timeval tv;
	unsigned int count = 0;

	memset(&us_data_1, 0, sizeof(struct uhf_security_data));
	/* hardcode for stress test */
	us_data_1.data[index++] = 0xAB;
	us_data_1.data[index++] = 0x51;
	us_data_1.data[index++] = 0x69;
	us_data_1.data[index++] = 0x00;
	us_data_1.data[index++] = 0x03;
	us_data_1.data[index++] = 0x00; /* error type */
	data2_index = index;
	us_data_1.data[index++] = 0x11;
	us_data_1.data[index++] = 0x22;
	us_data_1.data[index++] = 0x33;
	us_data_1.data[index++] = 0x44;
	us_data_1.data[index++] = 0x55;
	us_data_1.data[index++] = 0x66;
	us_data_1.data[index++] = 0x77;
	us_data_1.data[index++] = 0x00;
	us_data_1.data[index++] = 0x2B; /* antenn id */

	do_gettimeofday(&tv);
	time = ((uint64_t) tv.tv_sec) * (uint64_t)1000 +  (uint64_t)(tv.tv_usec / 1000);
	*((uint64_t *)(us_data_1.data + index)) = time;
	index += 8;

	*(uint16_t *)(us_data_1.data + index) = 0x8801;
	index += 2;
	*(uint16_t *)(us_data_1.data + index) = 5;
	index += 2;
	us_data_1.data[index++] = 0x00;
	us_data_1.data[index++] = 0x40;
	us_data_1.data[index++] = 0x00;
	us_data_1.data[index++] = 0x00;
	us_data_1.data[index++] = 0x00;

	*(uint16_t *)(us_data_1.data + index) = 0x8802;
	index += 2;
	*(uint16_t *)(us_data_1.data + index) = 2;
	index += 2;
	us_data_1.data[index++] = 0x2;
	us_data_1.data[index++] = 0x1;

	*(uint16_t *)(us_data_1.data + index) = 0x8803;
	index += 2;
	*(uint16_t *)(us_data_1.data + index) = 1;
	index += 2;
	us_data_1.data[index++] = 0x2;

	*(uint16_t *)(us_data_1.data + index) = 0x8804;
	index += 2;
	*(uint16_t *)(us_data_1.data + index) = 2;
	index += 2;
	us_data_1.data[index++] = 0x1;
	us_data_1.data[index++] = 0x38;

	*(uint16_t *)(us_data_1.data + index) = 0x8805;
	index += 2;
	*(uint16_t *)(us_data_1.data + index) = 2;
	index += 2;
	us_data_1.data[index++] = 0x0;
	us_data_1.data[index++] = 0x18;

	*(uint16_t *)(us_data_1.data + index) = 0x8806;
	index += 2;
	*(uint16_t *)(us_data_1.data + index) = 2;
	index += 2;
	us_data_1.data[index++] = 0x0;
	us_data_1.data[index++] = 0xFF;

	*(uint16_t *)(us_data_1.data + index) = 0x8807;
	index += 2;
	*(uint16_t *)(us_data_1.data + index) = 2;
	index += 2;
	us_data_1.data[index++] = 0x0;
	us_data_1.data[index++] = 0x0F;

	*(uint16_t *)(us_data_1.data + index) = 0x8808;
	index += 2;
	*(uint16_t *)(us_data_1.data + index) = 1;
	index += 2;
	us_data_1.data[index++] = 0x5;

	*(uint16_t *)(us_data_1.data + index) = 0x8809;
	index += 2;
	*(uint16_t *)(us_data_1.data + index) = 5;
	index += 2;
	us_data_1.data[index++] = 0x15;
	us_data_1.data[index++] = 0x1F;
	us_data_1.data[index++] = 0x1F;
	us_data_1.data[index++] = 0x23;
	us_data_1.data[index++] = 0x15;

	*(uint16_t *)(us_data_1.data + index) = 0x880A;
	index += 2;
	*(uint16_t *)(us_data_1.data + index) = 2;
	index += 2;
	us_data_1.data[index++] = 0x1;
	us_data_1.data[index++] = 0x38;

	*(uint16_t *)(us_data_1.data + index) = 0x880B;
	index += 2;
	*(uint16_t *)(us_data_1.data + index) = 1;
	index += 2;
	us_data_1.data[index++] = 0x4;

	*(uint16_t *)(us_data_1.data + index) = 0x880C;
	index += 2;
	*(uint16_t *)(us_data_1.data + index) = 2;
	index += 2;
	us_data_1.data[index++] = 0x0;
	us_data_1.data[index++] = 0xFF;

	*(uint16_t *)(us_data_1.data + index) = 0x880D;
	index += 2;
	*(uint16_t *)(us_data_1.data + index) = 1;
	index += 2;
	us_data_1.data[index++] = 0x3;

	index += 6;

	memcpy(&us_data_2, &us_data_1, sizeof(struct uhf_security_data));
	us_data_2.data[data2_index++] = 0x99;
	us_data_2.data[data2_index++] = 0xAA;
	us_data_2.data[data2_index++] = 0xBB;
	us_data_2.data[data2_index++] = 0xCC;
	us_data_2.data[data2_index++] = 0xDD;
	us_data_2.data[data2_index++] = 0xEE;
	us_data_2.data[data2_index++] = 0xFF;
	us_data_2.data[data2_index++] = 0x00;
	us_data_2.data[data2_index++] = 0x01; /* antenn id */

	us_data_1.len = index;
	us_data_2.len = index;

	while (!kthread_should_stop()) {
		mdelay(uhf->stress_interval);

		if (count ++ % 3) {
			us_calc_crc(&us_data_1);
			us_copy_to_cache(uhf, us_data_1);
		} else {
			us_calc_crc(&us_data_2);
			us_copy_to_cache(uhf, us_data_2);
		}
	}

	return 0;
}

static ssize_t us_sys_show(struct device *dev,
            struct device_attribute *attr, char *buf)
{
    return 0;
}

static ssize_t us_sys_store(struct device *dev,
            struct device_attribute *attr, const char *buf, size_t size)
{
    int ret;
    struct uhf_security *uhf = __uhf;

    if (strstr(buf, "status") != NULL) {
        ret = us_get_status(uhf->status);
        printk(KERN_ALERT "uhf security module status is %s.\n", ret == OK ? "OK" : "BUSY");
    } else if (strstr(buf, "reset") != NULL) {
        us_reset(uhf->reset);
    }

    return size;
}

static ssize_t us_stress_show(struct device *dev,
            struct device_attribute *attr, char *buf)
{
    return 0;
}

static ssize_t us_stress_store(struct device *dev,
            struct device_attribute *attr, const char *buf, size_t size)
{
    struct uhf_security *uhf = __uhf;

    if (strstr(buf, "start") != NULL) {
		uhf->stress_task = kthread_run(us_stress_func, (void *)uhf, "us_stress_task");
        if (uhf->stress_task)
            printk(KERN_ALERT "uhf security start stress work.\n");
    } else if (strstr(buf, "stop") != NULL) {
        if (uhf->stress_task) {
            kthread_stop(uhf->stress_task);
            uhf->stress_task = NULL;
            printk(KERN_ALERT "uhf security stop stress work.\n");
		}
    } else {
		unsigned long interval_ms;
		if (strict_strtoul(buf, 10, &interval_ms))
			return -1;
		if (!interval_ms)
			return -1;

		uhf->stress_interval = (unsigned int)interval_ms;
	}

    return size;
}

static DEVICE_ATTR(us_sys, 0664, us_sys_show, us_sys_store);
static DEVICE_ATTR(us_stress, 0664, us_stress_show, us_stress_store);

static int us_parse_dt(struct spi_device *spi, struct uhf_security *uhf)
{
    struct device_node *np = spi->dev.of_node;

    uhf->reset = of_get_named_gpio(np, "reset-gpio", 0);
    if (!gpio_is_valid(uhf->reset)) {
        dev_err(&spi->dev, "no reset gpio setting in dts\n");
        return -EINVAL;
    }

    uhf->status = of_get_named_gpio(np, "status-gpio", 0);
    if (!gpio_is_valid(uhf->status)) {
        dev_err(&spi->dev, "no reset gpio setting in dts\n");
        return -EINVAL;
    }

    uhf->radio_reset = of_get_named_gpio(np, "radio_reset-gpio", 0);
    if (!gpio_is_valid(uhf->radio_reset)) {
        dev_err(&spi->dev, "no radio reset gpio setting in dts\n");
        return -EINVAL;
    }

    uhf->radio_status = of_get_named_gpio(np, "radio_status-gpio", 0);
    if (!gpio_is_valid(uhf->radio_status)) {
        dev_err(&spi->dev, "no radio status gpio setting in dts\n");
        return -EINVAL;
    }

    printk(KERN_ALERT "@%s: reset = %d, status=%d, radio_reset=%d\n",
					__func__, uhf->reset, uhf->status, uhf->radio_reset);

    return 0;
}

static int us_probe(struct spi_device *spi)
{
    int ret = 0;
    struct uhf_security *uhf = NULL;
    unsigned long minor;

    printk(KERN_ALERT "Enter %s\n", __func__);

    uhf = devm_kzalloc(&spi->dev, sizeof(struct uhf_security), GFP_KERNEL);
    if (uhf == NULL) {
        dev_err(&spi->dev, "no memory\n");
        return -ENOMEM;
    }

    spi->mode = SPI_MODE_0;
    spi->bits_per_word = 8;
    if (!spi->max_speed_hz)
        spi->max_speed_hz = UHF_SECURITY_SPI_MAX_SPEED_HZ;

    ret = spi_setup(spi);
    if (ret < 0) {
        dev_err(&spi->dev, "setup spi failed\n");
        return ret;
    }

    spin_lock_init(&uhf->lock);
    sema_init(&uhf->sem, 1);
    mutex_init(&uhf->buf_lock);

    INIT_LIST_HEAD(&uhf->device_entry);

    if (spi->irq <= 0) {
        dev_err(&spi->dev, "no irq\n");
        return -ENODEV;
    }

    printk(KERN_ALERT "irq no:%d\n", spi->irq);

    ret = us_parse_dt(spi, uhf);
    if (ret < 0) {
        dev_err(&spi->dev, "parse dts failed\n");
        return -ENODEV;
    }

    /*
     * If we can allocate a minor number, hook up this device.
     * Reusing minors is fine so long as udev or mdev is working.
     */

    ret = register_chrdev(UHF_SPI_MAJOR, "spi", &us_fops);
    if (ret < 0) {
        printk(KERN_ALERT "%s: register chardev failed.\n", __func__);
        return -ENOMEM;
    }

    /* create class and device for udev information. */
    uhf->class = class_create(THIS_MODULE, "uhf_security");
    if (IS_ERR(uhf->class)) {
        printk(KERN_ERR "failed to create uhf class\n");
        goto class_fail;
    }

    mutex_lock(&device_list_lock);
    minor = find_first_zero_bit(minors, UHF_SPI_MINORS);
    if (minor < UHF_SPI_MINORS) {
        uhf->devt = MKDEV(UHF_SPI_MAJOR, minor);
        uhf->dev = device_create(uhf->class, &spi->dev, uhf->devt,
                    uhf, "uhf_security");
        ret = PTR_ERR_OR_ZERO(uhf->dev);
    } else {
        printk(KERN_ALERT "%s: no minor number available!\n", __func__);
        ret = -ENODEV;
        goto device_fail;
    }

    if (ret == 0) {
        set_bit(minor, minors);
        list_add(&uhf->device_entry, &device_list);
    }
    mutex_unlock(&device_list_lock);

    uhf->spi = spi;
    uhf->irq = spi->irq;
    spi_set_drvdata(spi, uhf);

	uhf->stress_interval = 1000;

    ret = us_init(uhf);

    ret += device_create_file(uhf->dev, &dev_attr_us_sys);
    if (ret) {
        printk(KERN_ALERT "%s: create sys file failed.\n", __func__);
        goto sys_fail;
    }

    ret += device_create_file(uhf->dev, &dev_attr_us_stress);
    if (ret) {
        printk(KERN_ALERT "%s: create stress sys file failed.\n", __func__);
        goto sys_fail;
    }

    uhf->irq_enabled = true;

    /* Initializes uhf security INT irq. */
	ret = devm_request_threaded_irq(&spi->dev, uhf->irq, NULL, us_intr_handler,
									IRQF_TRIGGER_FALLING | IRQF_ONESHOT,
									"uhf_irq", uhf);
	if (ret) {
        printk(KERN_ERR "failed to request irq_handler, ret=%d\n", ret);
        goto irq_fail;
    }

	__uhf = uhf;

    return ret;

irq_fail:
    device_remove_file(uhf->dev, &dev_attr_us_sys);
sys_fail:
    device_destroy(uhf->class, uhf->devt);
device_fail:
    class_destroy(uhf->class);
class_fail:
    unregister_chrdev(UHF_SPI_MAJOR, UHF_SECURITY_NAME);
    return ret;
}

static int us_remove(struct spi_device *spi)
{
    struct uhf_security *uhf = spi_get_drvdata(spi);

    if (uhf->stress_task) {
        kthread_stop(uhf->stress_task);
        uhf->stress_task = NULL;
    }
    /* make sure ops on existing fds can abort cleanly */
    spin_lock_irq(&uhf->lock);
    uhf->spi = NULL;
    spin_unlock_irq(&uhf->lock);

    free_irq(uhf->irq, uhf);

    /* prevent new opens */
    mutex_lock(&device_list_lock);
    list_del(&uhf->device_entry);

    device_destroy(uhf->class, uhf->devt);
    class_destroy(uhf->class);
    unregister_chrdev(UHF_SPI_MAJOR, UHF_SECURITY_NAME);
    clear_bit(MINOR(uhf->devt), minors);
    mutex_unlock(&device_list_lock);

    if (uhf->users == 0)
        kfree(uhf);

	__uhf = NULL;

    return 0;
}

#ifdef CONFIG_OF
static const struct of_device_id us_of_match[] = {
    { .compatible = "jzt,uhf_security" },
    { /* sentinel */ }
};
#endif

static struct spi_driver us_driver = {
    .driver = {
        .owner  = THIS_MODULE,
        .name   = UHF_SECURITY_NAME,
        .of_match_table = of_match_ptr(us_of_match),
    },
    .probe = us_probe,
    .remove = us_remove,
};

module_spi_driver(us_driver);


MODULE_AUTHOR("Shao Depeng <dp.shao@gmail.com>");
MODULE_DESCRIPTION("UHF security module driver");
MODULE_LICENSE("GPL");
MODULE_ALIAS("spi:uhf_security");
