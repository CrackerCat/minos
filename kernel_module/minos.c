#include <linux/module.h>
#include <linux/fs.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/major.h>
#include <linux/mutex.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/stat.h>
#include <linux/init.h>
#include <linux/device.h>
#include <linux/tty.h>
#include <linux/kmod.h>
#include <linux/gfp.h>
#include <linux/slab.h>

#include "minos.h"

#define	MINOS_VM_MAJOR		(278)

static int create_vm_device(int vmid, struct vm_info *vm_info);

struct class *vm_class;
static LIST_HEAD(vm_list);
static DEFINE_MUTEX(vm_mutex);

#define dev_to_vm(_dev) \
	container_of(_dev, struct vm_device, device)

#define file_to_vm(_filp) \
	(struct vm_device *)(_filp->private_data)

#define VM_INFO_SHOW(_member, format)	\
	static ssize_t vm_ ## _member ## _show(struct device *dev, \
			struct device_attribute * attr, char *buf) \
	{ \
		struct vm_device *vm = dev_to_vm(dev); \
		struct vm_info *info = &vm->vm_info; \
		return sprintf(buf, format, info->_member); \
	}

VM_INFO_SHOW(mem_start, "0x%llx\n")
VM_INFO_SHOW(mem_end, "0x%llx\n")
VM_INFO_SHOW(bit64, "%d\n")
VM_INFO_SHOW(mem_size, "0x%llx\n")
VM_INFO_SHOW(entry, "0x%llx\n")
VM_INFO_SHOW(setup_data, "0x%llx\n")
VM_INFO_SHOW(nr_vcpus, "%d\n")
VM_INFO_SHOW(name, "%s\n")
VM_INFO_SHOW(os_type, "%s\n")

static ssize_t
vm_vmid_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct vm_device *vm = dev_to_vm(dev);

	return sprintf(buf, "%d\n", vm->vmid);
}

static DEVICE_ATTR(vmid, 0444, vm_vmid_show, NULL);
static DEVICE_ATTR(mem_size, 0444, vm_mem_size_show, NULL);
static DEVICE_ATTR(nr_vcpus, 0444, vm_nr_vcpus_show, NULL);
static DEVICE_ATTR(mem_start, 0444, vm_mem_start_show, NULL);
static DEVICE_ATTR(mem_end, 0444, vm_mem_end_show, NULL);
static DEVICE_ATTR(entry, 0444, vm_entry_show, NULL);
static DEVICE_ATTR(setup_data, 0444, vm_setup_data_show, NULL);
static DEVICE_ATTR(name, 0444, vm_name_show, NULL);
static DEVICE_ATTR(os_type, 0444, vm_os_type_show, NULL);
static DEVICE_ATTR(bit64, 0444, vm_bit64_show, NULL);

static int mvm_open(struct inode *inode, struct file *file)
{
	int vmid = iminor(inode), err;
	struct vm_device *tmp, *vm = NULL;
	struct file_operations *new_fops = NULL;

	mutex_lock(&vm_mutex);

	list_for_each_entry(tmp, &vm_list, list) {
		if (vmid == tmp->vmid) {
			vm = tmp;
			new_fops = fops_get(vm->fops);
			break;
		}
	}

	if ((vm == NULL) || (!new_fops)) {
		pr_err("no such vm with vmid:%d\n", vmid);
		return -ENOENT;
	}

	file->private_data = vm;
	replace_fops(file, new_fops);
	err = 0;

	if (file->f_op->open)
		err = file->f_op->open(inode, file);

	mutex_unlock(&vm_mutex);
	return err;
}

static struct file_operations mvm_fops = {
	.owner		= THIS_MODULE,
	.open		= mvm_open,
	.llseek		= noop_llseek,
};

int vm_device_register(struct vm_device *vm)
{
	dev_t dev;
	int err = 0;

	INIT_LIST_HEAD(&vm->list);
	dev = MKDEV(MINOS_VM_MAJOR, vm->vmid);

	vm->device.class = vm_class;
	vm->device.devt = dev;
	vm->device.parent = NULL;
	dev_set_name(&vm->device, "mvm%d", vm->vmid);
	device_initialize(&vm->device);

	err = device_add(&vm->device);
	if (err)
		return err;

	mutex_lock(&vm_mutex);
	list_add_tail(&vm->list, &vm_list);
	mutex_unlock(&vm_mutex);

	device_create_file(&vm->device, &dev_attr_vmid);
	device_create_file(&vm->device, &dev_attr_nr_vcpus);
	device_create_file(&vm->device, &dev_attr_mem_size);
	device_create_file(&vm->device, &dev_attr_mem_start);
	device_create_file(&vm->device, &dev_attr_mem_end);
	device_create_file(&vm->device, &dev_attr_entry);
	device_create_file(&vm->device, &dev_attr_setup_data);
	device_create_file(&vm->device, &dev_attr_name);
	device_create_file(&vm->device, &dev_attr_os_type);
	device_create_file(&vm->device, &dev_attr_bit64);

	return 0;
}

int vm_device_unregister(struct vm_device *vm)
{
	mutex_lock(&vm_mutex);
	list_del(&vm->list);
	device_destroy(vm_class, MKDEV(MINOS_VM_MAJOR, vm->vmid));
	mutex_unlock(&vm_mutex);

	return 0;
}

static int vm_release(struct inode *inode, struct file *filp)
{
	struct vm_device *vm = file_to_vm(filp);

	if (!vm) {
		pr_err("vm has not been opend\n");
		return -ENOENT;
	}

	filp->private_data = NULL;
	atomic_cmpxchg(&vm->opened, 1, 0);

	return 0;
}

static int vm_open(struct inode *inode, struct file *filp)
{
	struct vm_device *vm = (struct vm_device *)filp->private_data;

	if (atomic_cmpxchg(&vm->opened, 0, 1)) {
		pr_err("minos: vm%d has been opened\n", vm->vmid);
		return -EBUSY;
	}

	return 0;
}

static int map_guest_memory(struct vm_device *vm, uint64_t *args)
{
	int ret;
	uint64_t offset, size;
	uint64_t mstart = 0, msize = 0;

	offset = args[0];
	size = args[1];

	printk("map guest memory 0x%llx 0x%llx\n", offset, size);
	ret = hvc_vm_mmap(vm->vmid, offset, size, &mstart, &msize);
	if (ret)
		return ret;

	args[0] = mstart;
	args[1] = msize;

	return 0;
}

static long vm_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	int ret;
	uint64_t args[2];
	struct vm_device *vm = file_to_vm(filp);

	if (!vm)
		return -ENOENT;

	switch (cmd) {
	case MINOS_IOCTL_DESTORY_VM:
		break;

	case MINOS_IOCTL_RESTART_VM:
		break;

	case MINOS_IOCTL_POWER_DOWN_VM:
		break;

	case MINOS_IOCTL_POWER_UP_VM:
		hvc_vm_power_up(vm->vmid);
		break;

	case MINOS_IOCTL_MMAP:
		ret = copy_from_user(args, (void *)arg, sizeof(uint64_t) * 2);
		if (ret)
			return -EINVAL;

		ret = map_guest_memory(vm, args);
		if (!ret) {
			ret = copy_to_user((void *)arg, (void *)args,
					sizeof(uint64_t) * 2);
		}

		return ret;
	}

	return 0;
}

static struct file_operations vm_fops = {
	.open		= vm_open,
	.release	= vm_release,
	.unlocked_ioctl = vm_ioctl,
	.owner		= THIS_MODULE,
};

static int vm0_open(struct inode *inode, struct file *filp)
{
	return 0;
}

static int vm0_release(struct inode *inode, struct file *filp)
{
	return 0;
}

static int create_new_vm(struct vm_info *info)
{
	int vmid;

	if (!info)
		return -EINVAL;

	vmid = hvc_vm_create(info);
	if (vmid <= 0) {
		pr_err("unable to create new vm\n");
		return vmid;
	}

	create_vm_device(vmid, info);

	return vmid;
}

static long vm0_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	int ret;
	struct vm_info vm_info;

	switch (cmd) {
	case MINOS_IOCTL_CREATE_VM:
		memset(&vm_info, 0, sizeof(struct vm_info));
		ret = copy_from_user(&vm_info, (void *)arg, sizeof(struct vm_info));
		if (ret)
			return -EINVAL;

		return create_new_vm(&vm_info);
	default:
		break;
	}

	return -EINVAL;
}

static struct file_operations vm0_fops = {
	.open		= vm0_open,
	.release	= vm0_release,
	.unlocked_ioctl = vm0_ioctl,
	.owner		= THIS_MODULE,
};

static int create_vm_device(int vmid, struct vm_info *vm_info)
{
	int ret;
	struct vm_device *vm;

	vm = kzalloc(sizeof(struct vm_device), GFP_KERNEL);
	if (!vm)
		return -ENOMEM;

	vm->vmid = vmid;
	if (vm_info)
		memcpy(&vm->vm_info, vm_info, sizeof(struct vm_info));

	if (vmid == 0)
		vm->fops = &vm0_fops;
	else
		vm->fops = &vm_fops;

	ret = vm_device_register(vm);
	if (ret)
		goto out_free_vm;

	return 0;

out_free_vm:
	kfree(vm);
	return ret;
}

static char *vm_devnode(struct device *dev, umode_t *mode)
{
	return kasprintf(GFP_KERNEL, "mvm/%s", dev_name(dev));
}

static int __init minos_init(void)
{
	int err;

	vm_class = class_create(THIS_MODULE, "mvm");
	err = PTR_ERR(vm_class);
	if (IS_ERR(vm_class))
		return err;

	err = register_chrdev(MINOS_VM_MAJOR, "mvm", &mvm_fops);
	if (err) {
		printk("unable to get major %d for mvm devices\n",
				MINOS_VM_MAJOR);
		class_destroy(vm_class);
		return err;
	}

	vm_class->devnode = vm_devnode;

	err = create_vm_device(0, NULL);
	if (err) {
		class_destroy(vm_class);
		unregister_chrdev(MINOS_VM_MAJOR, "mvm");
		return err;
	}

	return 0;
}

static void minos_exit(void)
{
	/* remove all vm which has created */
	class_destroy(vm_class);
	unregister_chrdev(MINOS_VM_MAJOR, "mvm");
}

module_init(minos_init);
module_exit(minos_exit);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Min Le lemin@gmail.com");
