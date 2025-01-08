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
#include <linux/ktime.h>
#include <linux/sched/clock.h>
#include <linux/sched.h>
#include <linux/pgtable.h>
#include <linux/kthread.h>
#include <linux/vmalloc.h>
#include <linux/memory.h>
#include <linux/memory_hotplug.h>
#include <linux/delay.h>

#include "my_char_device.h"
#include "simple_ftl.h"
#include "conv_ftl.h"
#include "nvmev.h"
#include "nvme.h"

#define MY_DEVICE_NAME "nvmev_device"
#define LBA_BITS (9)
#define LBA_SIZE (1 << LBA_BITS)
#define PAGE_BUFFER_SIZE 10
#define numerator 9
#define denominator 10

struct my_char_device *my_char_vdevice = NULL;
static struct dev_pagemap dev_pagemap;
static struct device *dummy_device;
static unsigned long offset = 0;
extern struct nvmev_dev *nvmev_vdev;
extern unsigned long pagefault_start;
extern unsigned long pagefault_size;
static int count = 0;
static unsigned long address_buffer[PAGE_BUFFER_SIZE];
static unsigned long next_pointer = 0;
static struct task_struct *test_kthread = NULL;
static struct my_inactive_list my_inactive_list = {NULL, 0};
static struct my_active_list  my_active_list = {NULL, 0};
static unsigned long total_mem = 0;
static unsigned long used_mem = 0;
struct my_page *eviction_page;
struct my_page *tmp;


static inline unsigned long long __get_wallclock(void)
{
	return cpu_clock(nvmev_vdev->config.cpu_nr_dispatcher);
}

static int kthread_test_func(void *data)   //定义一个内核线程要执行的函数，用于维护active_list和inactive_list
{
	while (!kthread_should_stop()) {
		struct my_page *active_pointer = my_active_list.active_list;
        while (active_pointer != NULL)
        {
            if (PageReferenced(active_pointer->page) == 0) {  // 如果页面不活跃/最近没被引用
                tmp = active_pointer;
                active_pointer = active_pointer->next;
                // 从active_list中摘除
                if (tmp == my_active_list.active_list)
                    my_active_list.active_list = tmp->next;
                if (tmp->pre != NULL)
                    tmp->pre->next = tmp->next;
                if (tmp->next != NULL)
                    tmp->next->pre = tmp->pre;
                my_active_list.nr_active -= 1;
                // 插入inactive_list中
                if (my_inactive_list.inactive_list != NULL)
                    my_inactive_list.inactive_list->pre = tmp;
                tmp->next = my_inactive_list.inactive_list;
                tmp->pre = NULL;
                my_inactive_list.inactive_list = tmp;
                my_inactive_list.nr_inactive += 1;
            }
            else {
                active_pointer = active_pointer->next;
            }
        }
        struct my_page *inactive_pointer = my_inactive_list.inactive_list;
        while (inactive_pointer != NULL)
        {
            if (PageReferenced(inactive_pointer->page) == 1) {  // 如果页面活跃/最近被引用
                tmp = inactive_pointer;
                inactive_pointer = inactive_pointer->next;
                // 从inactive_list中摘除
                if (tmp == my_inactive_list.inactive_list)
                    my_inactive_list.inactive_list = tmp->next;
                if (tmp->pre != NULL)
                    tmp->pre->next = tmp->next;
                if (tmp->next != NULL)
                    tmp->next->pre = tmp->pre;
                my_inactive_list.nr_inactive -= 1;
                // 插入active_list中
                if (my_active_list.active_list != NULL)
                    my_active_list.active_list->pre = tmp;
                tmp->next = my_active_list.active_list;
                tmp->pre = NULL;
                my_active_list.active_list = tmp;
                my_active_list.nr_active += 1;
            }
            else {
                inactive_pointer = inactive_pointer->next;
            }
        }
        // printk("this is kthread!\n");
        msleep(5000);
	}
	return 0;
}

vm_fault_t nvmev_fault(struct vm_fault *vmf)
{
    struct vm_area_struct *vma = vmf->vma;
    size_t threhold = total_mem * numerator / denominator;
    while (used_mem > threhold) {
        // 内存不足，需要淘汰页面 
        if (my_inactive_list.nr_inactive > 0) {  // 从inactive_list中淘汰页面
            eviction_page = my_inactive_list.inactive_list;
            my_inactive_list.inactive_list = my_inactive_list.inactive_list->next;
            my_inactive_list.inactive_list->pre = NULL;
            // 淘汰页面
            zap_vma_ptes(vma, eviction_page->address, PAGE_SIZE);
            used_mem -= PAGE_SIZE;
            kfree(eviction_page);
        }
        else {
            eviction_page = my_active_list.active_list;
            my_active_list.active_list = my_active_list.active_list->next;
            my_active_list.active_list->pre = NULL;
            // 淘汰页面
            zap_vma_ptes(vma, eviction_page->address, PAGE_SIZE);
            used_mem -= PAGE_SIZE;
            kfree(eviction_page);
        }
        // zap_vma_ptes(vma, address_buffer[next_pointer], PAGE_SIZE);
    }
    struct nvme_rw_command *cmd = vmalloc(sizeof(struct nvme_rw_command));
    cmd->slba = (vmf->address - vma->vm_start) >> LBA_BITS;
    cmd->length = 4096 / LBA_SIZE; // pg == 4KB
    cmd->control = 0;
    struct nvmev_request req = {
		.cmd = (struct nvme_command *)cmd,
		.sq_id = 0,
		.nsecs_start = 0,
	};
	struct nvmev_result ret = {
		.nsecs_target = 0,
		.status = NVME_SC_SUCCESS,
	};
    struct nvmev_ns *ns = &nvmev_vdev->ns[0];
    if (conv_read(ns, &req, &ret))
    {
        printk("read delay:%llu\n",ret.nsecs_target);
    }
	
    // if (count == 0)
    // {
    //     address = vmf->address;
    //     printk("first page fault address: %lx\n", address);
    // }
    // else if (count == 1)
    // {
    //     struct vm_area_struct *vma = vmf->vma;
    //     zap_vma_ptes(vma, vma->vm_start, PAGE_SIZE);
    //     printk("second page fault address: %lx\n", vmf->address);
    // }
    // else if (count == 2)
    // {
    //     printk("third page fault address: %lx\n", vmf->address);
    // }

    struct page *page;
    // struct vm_area_struct *vma = vmf->vma;
    unsigned long address = vmf->address;
    char *kernel_buffer = "Hello, User!";
    size_t len = strlen(kernel_buffer) + 1; // 包括 NULL 终止符

    offset = (vmf->address - vma->vm_start) >> PAGE_SHIFT << PAGE_SHIFT;
    page = pfn_to_page((pagefault_start + offset)>> PAGE_SHIFT);
    if (!page) {
        return VM_FAULT_OOM; // 内存不足
    }
    // printk("page:%p\n",page);

    struct my_page *my_page = vmalloc(sizeof(struct my_page));
    my_page->address = vmf->address;
    my_page->page = page;
    my_page->next = my_active_list.active_list;
    my_page->pre = NULL;
    my_active_list.active_list = my_page;
    my_active_list.nr_active += 1;
    used_mem += PAGE_SIZE;

    memcpy(page_address(page), kernel_buffer, len);

    vmf->page = page;
    get_page(vmf->page);

    // count++;
    // last_page_pointor = free_page_pointor++;
    // printk("free page pointor: %lx\n", free_page_pointor);

    if (conv_write(ns, &req, &ret))
    {
        printk("write delay:%llu\n",ret.nsecs_target);
    }

    return 0;
}

static int nvmev_mmap(struct file *filp, struct vm_area_struct *vma)
{
    size_t size = vma->vm_end - vma->vm_start;
    if (size > pagefault_size) {
        return -EINVAL;
    }

    total_mem = vma->vm_end - vma->vm_start;

    static struct vm_operations_struct vm_ops = { .fault = nvmev_fault };

	vma->vm_ops = &vm_ops;

    vma->vm_flags |= VM_PFNMAP;

    count = 0;

    return 0;
}

static const struct file_operations nvmev_fops = {
    .owner = THIS_MODULE,
    .mmap = nvmev_mmap,
};

static struct dev_pagemap_ops my_dev_pagemap_ops = {
    // .page_free = my_page_free, // 提供有效的回调函数
    // .migrate_to_ram = nvmev_fault,
};

int create_mydev(void)
{
    // memset(address_buffer, 0, sizeof(address_buffer))

    int ret;
    my_char_vdevice = kmalloc(sizeof(struct my_char_device), GFP_KERNEL);
    if (!my_char_vdevice)
        return -ENOMEM;

    // 保留内存的映射
    char *kernel_buffer = "Hello, User!";
    size_t len = strlen(kernel_buffer) + 1; // 包括 NULL 终止符
    // my_char_vdevice->storage_mapped = nvmev_vdev->storage_mapped;
    // my_char_vdevice->storage_size = memmap_size;
    // memcpy(my_char_vdevice->storage_mapped, kernel_buffer, len);

    dummy_device = kmalloc(sizeof(struct device), GFP_KERNEL);
    device_initialize(dummy_device);
    dummy_device->init_name = "my_dummy_device"; // 设置设备名称

    memset(&dev_pagemap, 0, sizeof(dev_pagemap));
    dev_pagemap.type = MEMORY_DEVICE_GENERIC;
    dev_pagemap.ops = &my_dev_pagemap_ops;
    dev_pagemap.range.start = pagefault_start;
    dev_pagemap.range.end  = pagefault_start + pagefault_size -1;
    dev_pagemap.nr_range = 1;
    dev_pagemap.owner = THIS_MODULE;
    pr_info("memmap_start:%lx memmap_size:%lx\n",pagefault_start, pagefault_size);
    ret = devm_memremap_pages(dummy_device, &dev_pagemap);
    if (IS_ERR_VALUE(ret)) {
        pr_err("Failed to remap pages for reserved memory: %d\n", ret);
        return ret;
    }

    pr_info("Reserved memory successfully registered with ZONE_DEVICE\n");

    // if (!my_char_vdevice->storage_mapped) {
    //     ret = -ENOMEM;
    //     goto err_alloc;
    // }

    test_kthread = kthread_run(kthread_test_func, NULL, "kthread-test");  //创建线程kthread-test，并且运行
	if (!test_kthread) {
		goto err_dummy_device;
	}

    // 分配设备号
    ret = alloc_chrdev_region(&my_char_vdevice->devt, 0, 1, MY_DEVICE_NAME);
    if (ret)
        goto err_kthread;

    cdev_init(&my_char_vdevice->cdev, &nvmev_fops);
    my_char_vdevice->cdev.owner = THIS_MODULE;

    ret = cdev_add(&my_char_vdevice->cdev, my_char_vdevice->devt, 1);
    if (ret)
        goto err_chrdev;

    // 创建设备类
    my_char_vdevice->class = class_create(THIS_MODULE, MY_DEVICE_NAME);
    if (IS_ERR(my_char_vdevice->class)) {
        pr_err("Failed to create class\n");
        ret = PTR_ERR(my_char_vdevice->class);
        goto err_cdev;
    }

    // 创建设备文件
    my_char_vdevice->device = device_create(my_char_vdevice->class, NULL, my_char_vdevice->devt, NULL, MY_DEVICE_NAME);
    if (IS_ERR(my_char_vdevice->device)) {
        pr_err("Failed to create device\n");
        ret = PTR_ERR(my_char_vdevice->device);
        goto err_class;
    }

    pr_info("nvmev_device initialized successfully\n");
    printk("start=%lx size=%lx", pagefault_start, pagefault_size);
    return 0;

err_class:
    class_destroy(my_char_vdevice->class);
err_cdev:
    cdev_del(&my_char_vdevice->cdev);
err_chrdev:
    unregister_chrdev_region(my_char_vdevice->devt, 1);
err_kthread:
    // memunmap(my_char_vdevice->storage_mapped);
    kthread_stop(test_kthread); //停止内核线程
	test_kthread = NULL;
err_dummy_device:
    kfree(dummy_device);
    memunmap_pages(&dev_pagemap);
err_alloc:
    kfree(my_char_vdevice);
    return ret;
}

void destroy_mydev(void)
{
    if (test_kthread) {
		kthread_stop(test_kthread); //停止内核线程
		test_kthread = NULL;
	}
    kfree(dummy_device);
    memunmap_pages(&dev_pagemap);
    device_destroy(my_char_vdevice->class, my_char_vdevice->devt);
    class_destroy(my_char_vdevice->class);
    cdev_del(&my_char_vdevice->cdev);
    unregister_chrdev_region(my_char_vdevice->devt, 1);
    // memunmap(my_char_vdevice->storage_mapped);
    kfree(my_char_vdevice);
    pr_info("nvmev_device exited\n");
    return;
}