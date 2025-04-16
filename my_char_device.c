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
#include <linux/spinlock.h>

#include "my_char_device.h"
#include "simple_ftl.h"
#include "conv_ftl.h"
#include "nvmev.h"
#include "nvme.h"

#define MY_DEVICE_NAME "nvmev_device"
#define LBA_BITS (9)
#define LBA_SIZE (1 << LBA_BITS)
#define PAGE_BUFFER_SIZE 10
#define numerator 95
#define denominator 100

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
static struct task_struct *mark_kthread = NULL;
static struct task_struct *eviction_kthread = NULL;
static struct my_inactive_list my_inactive_list = {NULL, NULL, 0};
static struct my_active_list  my_active_list = {NULL, NULL, 0};
static unsigned long total_mem = 0;
static unsigned long used_mem = 0;
struct my_page *eviction_page;
struct my_page *eviction_page2;
struct my_page *tmp;
struct vm_area_struct *vma;

// 定义锁和受保护的资源
static DEFINE_SPINLOCK(inactive_list_lock);  // 静态初始化自旋锁
static DEFINE_SPINLOCK(active_list_lock);  // 静态初始化自旋锁


static inline unsigned long long __get_wallclock(void)
{
	return cpu_clock(nvmev_vdev->config.cpu_nr_dispatcher);
}

static void print_list(struct my_page *p)
{
    while(p != NULL)
    {
        printk("/*address:%lx*/ ", p->address);
        p = p->next;
    }
    printk("\n");
}

// static int kthread_mark_func(void *data)   //定义一个内核线程要执行的函数，用于维护active_list和inactive_list
// {
// 	while (!kthread_should_stop()) {
// 		struct my_page *active_pointer = my_active_list.active_list;
//         while (active_pointer != NULL)
//         {
//             if (PageReferenced(active_pointer->page) == 0) {  // 如果页面不活跃/最近没被引用
//                 tmp = active_pointer;
//                 active_pointer = active_pointer->next;
//                 // 从active_list中摘除
//                 spin_lock(&active_list_lock);          // 获取锁
//                 if (tmp == my_active_list.active_list)
//                     my_active_list.active_list = tmp->next;
//                 if (tmp->pre != NULL)
//                     tmp->pre->next = tmp->next;
//                 if (tmp->next != NULL)
//                     tmp->next->pre = tmp->pre;
//                 my_active_list.nr_active -= 1;
//                 spin_unlock(&active_list_lock);        // 释放锁
//                 // 插入inactive_list中
//                 spin_lock(&inactive_list_lock);          // 获取锁
//                 if (my_inactive_list.inactive_list != NULL)
//                     my_inactive_list.inactive_list->pre = tmp;
//                 tmp->next = my_inactive_list.inactive_list;
//                 tmp->pre = NULL;
//                 my_inactive_list.inactive_list = tmp;
//                 my_inactive_list.nr_inactive += 1;
//                 spin_unlock(&inactive_list_lock);        // 释放锁
//             }
//             else {
//                 active_pointer = active_pointer->next;
//             }
//         }
//         struct my_page *inactive_pointer = my_inactive_list.inactive_list;
//         while (inactive_pointer != NULL)
//         {
//             if (PageReferenced(inactive_pointer->page) == 1) {  // 如果页面活跃/最近被引用
//                 tmp = inactive_pointer;
//                 inactive_pointer = inactive_pointer->next;
//                 // 从inactive_list中摘除
//                 spin_lock(&inactive_list_lock);          // 获取锁
//                 if (tmp == my_inactive_list.inactive_list)
//                     my_inactive_list.inactive_list = tmp->next;
//                 if (tmp->pre != NULL)
//                     tmp->pre->next = tmp->next;
//                 if (tmp->next != NULL)
//                     tmp->next->pre = tmp->pre;
//                 my_inactive_list.nr_inactive -= 1;
//                 spin_unlock(&inactive_list_lock);        // 释放锁
//                 // 插入active_list中
//                 spin_lock(&active_list_lock);          // 获取锁
//                 if (my_active_list.active_list != NULL)
//                     my_active_list.active_list->pre = tmp;
//                 tmp->next = my_active_list.active_list;
//                 tmp->pre = NULL;
//                 my_active_list.active_list = tmp;
//                 my_active_list.nr_active += 1;
//                 spin_unlock(&active_list_lock);        // 释放锁
//             }
//             else {
//                 inactive_pointer = inactive_pointer->next;
//             }
//         }
//         // size_t threhold = total_mem * 8 / 10;
//         // while (used_mem > threhold) {
//         //     // 内存不足，需要淘汰页面 
//         //     if (my_inactive_list.nr_inactive > 0) {  // 从inactive_list中淘汰页面
//         //         eviction_page2 = my_inactive_list.inactive_list;
//         //         my_inactive_list.inactive_list = my_inactive_list.inactive_list->next;
//         //         my_inactive_list.inactive_list->pre = NULL;
//         //         // 写入ssd
//         //         struct nvme_rw_command *cmd = vmalloc(sizeof(struct nvme_rw_command));
//         //         cmd->slba = (eviction_page2->address - vma->vm_start) >> LBA_BITS;
//         //         cmd->length = 4096 / LBA_SIZE; // pg == 4KB
//         //         cmd->control = 0;
//         //         struct nvmev_request req = {
//         //             .cmd = (struct nvme_command *)cmd,
//         //             .sq_id = 0,
//         //             .nsecs_start = 0,
//         //         };
//         //         struct nvmev_result ret = {
//         //             .nsecs_target = 0,
//         //             .status = NVME_SC_SUCCESS,
//         //         };
//         //         struct nvmev_ns *ns = &nvmev_vdev->ns[0];
//         //         if (conv_write(ns, &req, &ret))
//         //         {
//         //             printk("write delay:%llu\n",ret.nsecs_target);
//         //         }
//         //         // 淘汰页面
//         //         zap_vma_ptes(vma, eviction_page2->address, PAGE_SIZE);
//         //         used_mem -= PAGE_SIZE;
//         //         kfree(eviction_page2);
//         //         kfree(cmd);
//         //     }
//         //     else {
//         //         eviction_page2 = my_active_list.active_list;
//         //         my_active_list.active_list = my_active_list.active_list->next;
//         //         my_active_list.active_list->pre = NULL;
//         //         // 写入ssd
//         //         struct nvme_rw_command *cmd = vmalloc(sizeof(struct nvme_rw_command));
//         //         cmd->slba = (eviction_page2->address - vma->vm_start) >> LBA_BITS;
//         //         cmd->length = 4096 / LBA_SIZE; // pg == 4KB
//         //         cmd->control = 0;
//         //         struct nvmev_request req = {
//         //             .cmd = (struct nvme_command *)cmd,
//         //             .sq_id = 0,
//         //             .nsecs_start = 0,
//         //         };
//         //         struct nvmev_result ret = {
//         //             .nsecs_target = 0,
//         //             .status = NVME_SC_SUCCESS,
//         //         };
//         //         struct nvmev_ns *ns = &nvmev_vdev->ns[0];
//         //         if (conv_write(ns, &req, &ret))
//         //         {
//         //             printk("write delay:%llu\n",ret.nsecs_target);
//         //         }
//         //         // 淘汰页面
//         //         zap_vma_ptes(vma, eviction_page2->address, PAGE_SIZE);
//         //         used_mem -= PAGE_SIZE;
//         //         kfree(eviction_page2);
//         //         kfree(cmd);
//         //     }
//         //     // zap_vma_ptes(vma, address_buffer[next_pointer], PAGE_SIZE);
//         // }
//         // printk("this is kthread!\n");
//         msleep(5000);
// 	}
// 	return 0;
// }

// static int kthread_eviction_func(void *data)   //定义一个内核线程要执行的函数，用于维护active_list和inactive_list
// {
// 	while (!kthread_should_stop()) {
//         size_t threhold = total_mem * 8 / 10;
//         while (used_mem > threhold) {
//             // 内存不足，需要淘汰页面 
//             if (my_inactive_list.nr_inactive > 0) {  // 从inactive_list中淘汰页面
//                 spin_lock(&inactive_list_lock);          // 获取锁
//                 eviction_page2 = my_inactive_list.inactive_list;
//                 my_inactive_list.inactive_list = my_inactive_list.inactive_list->next;
//                 my_inactive_list.inactive_list->pre = NULL;
//                 spin_unlock(&inactive_list_lock);        // 释放锁
//                 // 写入ssd
//                 struct nvme_rw_command *cmd = vmalloc(sizeof(struct nvme_rw_command));
//                 cmd->slba = (eviction_page2->address - vma->vm_start) >> LBA_BITS;
//                 cmd->length = 4096 / LBA_SIZE; // pg == 4KB
//                 cmd->control = 0;
//                 struct nvmev_request req = {
//                     .cmd = (struct nvme_command *)cmd,
//                     .sq_id = 0,
//                     .nsecs_start = 0,
//                 };
//                 struct nvmev_result ret = {
//                     .nsecs_target = 0,
//                     .status = NVME_SC_SUCCESS,
//                 };
//                 struct nvmev_ns *ns = &nvmev_vdev->ns[0];
//                 if (conv_write(ns, &req, &ret))
//                 {
//                     printk("write delay:%llu\n",ret.nsecs_target);
//                 }
//                 // 淘汰页面
//                 zap_vma_ptes(vma, eviction_page2->address, PAGE_SIZE);
//                 used_mem -= PAGE_SIZE;
//                 kfree(eviction_page2);
//                 kfree(cmd);
//             }
//             else {
//                 spin_lock(&active_list_lock);          // 获取锁
//                 eviction_page2 = my_active_list.active_list;
//                 my_active_list.active_list = my_active_list.active_list->next;
//                 my_active_list.active_list->pre = NULL;
//                 spin_unlock(&active_list_lock);        // 释放锁
//                 // 写入ssd
//                 struct nvme_rw_command *cmd = vmalloc(sizeof(struct nvme_rw_command));
//                 cmd->slba = (eviction_page2->address - vma->vm_start) >> LBA_BITS;
//                 cmd->length = 4096 / LBA_SIZE; // pg == 4KB
//                 cmd->control = 0;
//                 struct nvmev_request req = {
//                     .cmd = (struct nvme_command *)cmd,
//                     .sq_id = 0,
//                     .nsecs_start = 0,
//                 };
//                 struct nvmev_result ret = {
//                     .nsecs_target = 0,
//                     .status = NVME_SC_SUCCESS,
//                 };
//                 struct nvmev_ns *ns = &nvmev_vdev->ns[0];
//                 if (conv_write(ns, &req, &ret))
//                 {
//                     printk("write delay:%llu\n",ret.nsecs_target);
//                 }
//                 // 淘汰页面
//                 zap_vma_ptes(vma, eviction_page2->address, PAGE_SIZE);
//                 used_mem -= PAGE_SIZE;
//                 kfree(eviction_page2);
//                 kfree(cmd);
//             }
//             // zap_vma_ptes(vma, address_buffer[next_pointer], PAGE_SIZE);
//         }
//         msleep(6000);
// 	}
// 	return 0;
// }

vm_fault_t nvmev_fault(struct vm_fault *vmf)
{
    vma = vmf->vma;
    size_t threhold = total_mem * numerator / denominator;
    while (count > 819) {
        // 内存不足，需要淘汰页面 
        // if (my_inactive_list.nr_inactive > 0) {  // 从inactive_list中淘汰页面
        //     spin_lock(&inactive_list_lock);          // 获取锁
        //     eviction_page = my_inactive_list.tail;
        //     my_inactive_list.tail = (my_inactive_list.tail)->pre;
        //     (my_inactive_list.tail)->pre->next = NULL;
        //     spin_unlock(&inactive_list_lock);        // 释放锁
        //     // 写入ssd
        //     struct nvme_rw_command *cmd = vmalloc(sizeof(struct nvme_rw_command));
        //     cmd->slba = (eviction_page->address - vma->vm_start) >> LBA_BITS;
        //     cmd->length = 4096 / LBA_SIZE; // pg == 4KB
        //     cmd->control = 0;
        //     struct nvmev_request req = {
        //         .cmd = (struct nvme_command *)cmd,
        //         .sq_id = 0,
        //         .nsecs_start = 0,
        //     };
        //     struct nvmev_result ret = {
        //         .nsecs_target = 0,
        //         .status = NVME_SC_SUCCESS,
        //     };
        //     struct nvmev_ns *ns = &nvmev_vdev->ns[0];
        //     if (conv_write(ns, &req, &ret))
        //     {
        //         printk("write delay:%llu\n",ret.nsecs_target);
        //         msleep(ret.nsecs_target);
        //     }
        //     // 淘汰页面
        //     zap_vma_ptes(vma, vma->vm_start, PAGE_SIZE);
        //     used_mem -= PAGE_SIZE;
        //     kfree(eviction_page);
        //     kfree(cmd);
        // }
        // else {  // 从active_list中淘汰页面
            spin_lock(&active_list_lock);          // 获取锁
            eviction_page = my_active_list.tail;
            // printk("eviction page address: %lx\n", eviction_page->address);
            // printk("vm_start address: %lx\n", vma->vm_start);
            if (eviction_page == NULL) {
                spin_unlock(&active_list_lock);        // 释放锁
                printk("active_list is empty!\n");
            }
            // printk("1\n");
            ((my_active_list.tail)->pre)->next = NULL;
            // printk("1.5\n");
            my_active_list.tail = (my_active_list.tail)->pre;
            my_active_list.nr_active -= 1;
            // printk("1.75\n");
            spin_unlock(&active_list_lock);        // 释放锁
            // 写入ssd
            // printk("2\n");
            struct nvme_rw_command *cmd = vmalloc(sizeof(struct nvme_rw_command));
            cmd->slba = (eviction_page->address - vma->vm_start) >> LBA_BITS;
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
            // printk("3\n");
            struct nvmev_ns *ns = &nvmev_vdev->ns[0];
            if (conv_write(ns, &req, &ret))
            {
                printk("write delay:%llu\n",ret.nsecs_target);
                // msleep(ret.nsecs_target);
            }
            // 淘汰页面
            printk("eviction page address: %lx\n", eviction_page->address);
            printk("vm_start address: %lx\n", vma->vm_start);
            zap_vma_ptes(vma, eviction_page->address, PAGE_SIZE);
            printk("1\n");
            used_mem -= PAGE_SIZE;
            printk("2\n");
            if (cmd != NULL)
                kfree(cmd);
            printk("3\n");
            if (eviction_page != NULL)
                kfree(eviction_page);
            printk("4\n");
            // printk("eviction page address: %lx\n", eviction_page->address);
        // }
        // zap_vma_ptes(vma, vma->vm_start, PAGE_SIZE);
        count--;
    }
    
    // 打印active_list和inactive_list
    // printk("active list: ");
    // print_list(my_active_list.head);
    // if (my_active_list.tail != NULL)
    //     printk("active list tail: %lx\n", (my_active_list.tail)->address);
    // printk("active list tail: %lx\n", my_active_list.tail->address);
    // printk("inactive list: ");
    // print_list(my_inactive_list.head);

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
        // msleep(ret.nsecs_target);
    }
	
    // if (count == 0)
    // {
    //     printk("first page fault address: %lx\n", vmf->address);
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
    my_page->address = vma->vm_start + offset;
    my_page->page = page;
    my_page->next = my_active_list.head;
    my_page->pre = NULL;
    if (my_active_list.head != NULL)
        (my_active_list.head)->pre = my_page;
    my_active_list.head = my_page;
    my_active_list.nr_active += 1;
    if (my_active_list.nr_active == 1) {
        my_active_list.tail = my_active_list.head;
    }
    used_mem += PAGE_SIZE;

    // memcpy(page_address(page), kernel_buffer, len);

    vmf->page = page;
    get_page(vmf->page);

    count++;
    // last_page_pointor = free_page_pointor++;
    // printk("free page pointor: %lx\n", free_page_pointor);

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

    // mark_kthread = kthread_run(kthread_mark_func, NULL, "kthread-mark");  //创建线程kthread-test，并且运行
	// if (!mark_kthread) {
	// 	goto err_dummy_device;
	// }

    // eviction_kthread = kthread_run(kthread_eviction_func, NULL, "kthread-eviction");  //创建线程kthread-test，并且运行
	// if (!eviction_kthread) {
	// 	goto err_kthread_mark;
	// }

    // 分配设备号
    ret = alloc_chrdev_region(&my_char_vdevice->devt, 0, 1, MY_DEVICE_NAME);
    if (ret)
        goto err_kthread_eviction;

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
err_kthread_eviction:
    // kthread_stop(eviction_kthread); //停止内核线程
	eviction_kthread = NULL;
err_kthread_mark:
    // memunmap(my_char_vdevice->storage_mapped);
    // kthread_stop(mark_kthread); //停止内核线程
	mark_kthread = NULL;
err_dummy_device:
    kfree(dummy_device);
    memunmap_pages(&dev_pagemap);
err_alloc:
    kfree(my_char_vdevice);
    return ret;
}

void destroy_mydev(void)
{
    if (mark_kthread) {
		kthread_stop(mark_kthread); //停止内核线程
		mark_kthread = NULL;
	}
    if (eviction_kthread) {
		kthread_stop(eviction_kthread); //停止内核线程
		eviction_kthread = NULL;
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