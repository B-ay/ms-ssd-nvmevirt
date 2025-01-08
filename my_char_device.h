#ifndef _MY_CHAR_DEVICE_H_
#define _MY_CHAR_DEVICE_H_

#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/mm.h>
#include <linux/mm_types.h>
#include <linux/io.h>

struct my_char_device {
    void __iomem *storage_mapped;
    size_t storage_size;
    struct cdev cdev;
    dev_t devt;
    struct class *class;
    struct device *device;
};

struct my_page {
    unsigned long address;
    struct page *page;
    struct my_page *next;
    struct my_page *pre;
};

struct my_active_list {
    struct my_page *active_list;
    unsigned long nr_active;
};

struct my_inactive_list {
    struct my_page *inactive_list;
    unsigned long nr_inactive;
};

extern struct my_char_device *my_char_vdevice;

int create_mydev(void);
void destroy_mydev(void);

#endif