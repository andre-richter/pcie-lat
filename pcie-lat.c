/*
 * Copyright (C) 2014 by the author(s)
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * =============================================================================
 *
 * Author(s):
 *   Andre Richter, andre.o.richter @t gmail_com
 *
 * Credits:
 *   Chris Wright: Linux pci-stub driver.
 *
 *   Gabriele Paoloni: "How to Benchmark Code Execution Times on
 *                     Intel IA-32 and IA-64 Instruction Set Architectures"
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/pci.h>
#include <linux/fs.h>
#include <linux/types.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/kernel.h>
#include <linux/uaccess.h>
#include <linux/delay.h>

#define DRIVER_NAME "pcie-lat"
#define LOOPS_UPPER_LIMIT	10000000
#define LOOPS_DEFAULT		100000
#define OVERHEAD_MEASURE_LOOPS	1000000

static char ids[1024] __initdata;

module_param_string(ids, ids, sizeof(ids), 0);
MODULE_PARM_DESC(ids, "Initial PCI IDs to add to the driver, format is "
                 "\"vendor:device[:subvendor[:subdevice[:class[:class_mask]]]]\""
		 " and multiple comma separated entries can be specified");

static unsigned int tsc_overhead;

struct result_data_t {
	u64 tsc_start;
	u64 tsc_diff;
};

/* BAR info*/
struct bar_t {
	int len;
	void __iomem *addr;
};

struct options_t {
	unsigned int loops;
	unsigned char target_bar;
	u32 bar_offset;
};

struct pcielat_priv {
	struct pci_dev *pdev;
	struct bar_t bar[6];
	dev_t dev_num;
	struct cdev cdev;
	struct result_data_t *result_data;
	unsigned int cur_resdata_size_in_bytes;
	struct options_t options;
};

/*
 * Character device data and callbacks
 */
static struct class *pcielat_class;

static int dev_open(struct inode *inode, struct file *file)
{
	struct pcielat_priv *priv = container_of(inode->i_cdev,
						 struct pcielat_priv, cdev);
	file->private_data = priv;

	return 0;
};

static ssize_t dev_read(struct file *file, char __user *buf,
			size_t count, loff_t *ppos)
{
	struct pcielat_priv *priv = file->private_data;

	/* If offset is behind string length, return nothing */
	if (*ppos >= priv->cur_resdata_size_in_bytes)
		return 0;

	/* If user wants to read more than is available, return what's there */
	if (*ppos + count > priv->cur_resdata_size_in_bytes)
		count = priv->cur_resdata_size_in_bytes - *ppos;

	if (copy_to_user(buf, (void *)priv->result_data + *ppos, count) != 0)
		return -EFAULT;

	*ppos += count;
	return count;
}

static const struct file_operations fops = {
	.owner	 = THIS_MODULE,
	.open	 = dev_open,
	.read	 = dev_read
};

/*
 * PCI device callbacks
 */
static int pci_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
	int err = 0, i;
	int mem_bars;
	struct pcielat_priv *priv;
	struct device *dev;

	priv = kzalloc(sizeof(struct pcielat_priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	err = pci_enable_device_mem(pdev);
	if (err)
		goto failure_pci_enable;

	/* Request only the BARs that contain memory regions */
	mem_bars = pci_select_bars(pdev, IORESOURCE_MEM);
	err = pci_request_selected_regions(pdev, mem_bars, DRIVER_NAME);
	if (err)
		goto failure_pci_regions;

	/* Memory Map BARs for MMIO */
	for (i = 0; i < 6; i++) {
		if (mem_bars & (1 << i)) {
			priv->bar[i].addr = ioremap(pci_resource_start(pdev, i),
						    pci_resource_len(pdev, i));

			if (IS_ERR(priv->bar[i].addr)) {
				err = PTR_ERR(priv->bar[i].addr);
				break;
			} else
				priv->bar[i].len = (int)pci_resource_len(pdev, i);
		} else {
			priv->bar[i].addr = NULL;
			priv->bar[i].len = -1;
		}
	}

	if (err) {
		for (i--; i >= 0; i--)
			if (priv->bar[i].len)
				iounmap(priv->bar[i].addr);
		goto failure_ioremap;
	}

	/* Get device number range */
	err = alloc_chrdev_region(&priv->dev_num, 0, 1, DRIVER_NAME);
	if (err)
		goto failure_alloc_chrdev_region;

	/* connect cdev with file operations */
	cdev_init(&priv->cdev, &fops);
	priv->cdev.owner = THIS_MODULE;

	/* add major/min range to cdev */
	err = cdev_add(&priv->cdev, priv->dev_num, 1);
	if (err)
		goto failure_cdev_add;

	dev = device_create(pcielat_class, &pdev->dev, priv->dev_num, NULL,
			    "%02x:%02x.%x", pdev->bus->number,
			    PCI_SLOT(pdev->devfn), PCI_FUNC(pdev->devfn));

	if (IS_ERR(dev)) {
		err = PTR_ERR(dev);
		goto failure_device_create;
	}

	dev_set_drvdata(dev, priv);
	pci_set_drvdata(pdev, priv);
	dev_info(&pdev->dev, "claimed by " DRIVER_NAME "\n");

	return 0;

failure_device_create:
	cdev_del(&priv->cdev);

failure_cdev_add:
	unregister_chrdev_region(priv->dev_num, 0);

failure_alloc_chrdev_region:
	for (i = 0; i < 6; i++)
		if (priv->bar[i].len)
			iounmap(priv->bar[i].addr);

failure_ioremap:
	pci_release_selected_regions(pdev,
				     pci_select_bars(pdev, IORESOURCE_MEM));

failure_pci_regions:
	pci_disable_device(pdev);

failure_pci_enable:
	kfree(priv);

	return err;
}

static void pci_remove(struct pci_dev *pdev)
{
	int i;
	struct pcielat_priv *priv = pci_get_drvdata(pdev);

	device_destroy(pcielat_class, priv->dev_num);

	cdev_del(&priv->cdev);

	unregister_chrdev_region(priv->dev_num, 0);

	for (i = 0; i < 6; i++)
		if (priv->bar[i].len)
			iounmap(priv->bar[i].addr);

	pci_release_selected_regions(pdev,
				     pci_select_bars(pdev, IORESOURCE_MEM));
	pci_disable_device(pdev);

	if (!priv->result_data)
		vfree(priv->result_data);

	kfree(priv);
}

static struct pci_driver pcielat_driver = {
	.name		= DRIVER_NAME,
	.id_table	= NULL,	/* only dynamic id's */
	.probe		= pci_probe,
	.remove         = pci_remove,
};

/*
 * The following code implements PCIe latency measurement by
 * benchmarking the time it takes to complete a readl() to a user
 * specified BAR and offset within this BAR.
 *
 * Time is measured via the TSC and implemented according to
 * "G. Paoloni, How to benchmark code execution times on
 * intel ia-32 and ia-64 instruction set architectures,
 * White paper, Intel Corporation."
 */
#define get_tsc_top(high, low)				\
	asm volatile ("cpuid         \n\t"		\
		      "rdtsc         \n\t"		\
		      "mov %%edx, %0 \n\t"		\
		      "mov %%eax, %1 \n\t"		\
		      :"=r" (high), "=r"(low)		\
		      :					\
		      :"rax", "rbx", "rcx", "rdx");	\

#define get_tsc_bottom(high, low)			\
	asm volatile ("rdtscp \n\t"			\
		      "mov %%edx, %0 \n\t"		\
		      "mov %%eax, %1 \n\t"		\
		      "cpuid \n\t"			\
		      :"=r" (high), "=r"(low)		\
		      :					\
		      :"rax", "rbx", "rcx", "rdx");	\

static void do_benchmark(void __iomem *addr, u32 bar_offset, unsigned int loops,
			 struct result_data_t *result_data)
{
	unsigned long flags;
	u32 tsc_high_before, tsc_high_after;
	u32 tsc_low_before, tsc_low_after;
	u64 tsc_start, tsc_end, tsc_diff;
	unsigned int i;

	/*
	 * "Warmup" of the benchmarking code.
	 * This will put instructions into cache.
	 */
	get_tsc_top(tsc_high_before, tsc_low_before);
	get_tsc_bottom(tsc_high_after, tsc_low_after);
	get_tsc_top(tsc_high_before, tsc_low_before);
	get_tsc_bottom(tsc_high_after, tsc_low_after);

        /* Main latency measurement loop */
	for (i = 0; i < loops; i++) {

		preempt_disable();
		raw_local_irq_save(flags);

		get_tsc_top(tsc_high_before, tsc_low_before);

		/*** Function to measure execution time for ***/
		readl(addr + bar_offset);
		/***************************************/

		get_tsc_bottom(tsc_high_after, tsc_low_after);

		raw_local_irq_restore(flags);
		preempt_enable();

		/* Calculate delta */
		tsc_start = ((u64) tsc_high_before << 32) | tsc_low_before;
		tsc_end = ((u64) tsc_high_after << 32) | tsc_low_after;
	        tsc_diff = tsc_end - tsc_start;

		result_data[i].tsc_start  = tsc_start;
		result_data[i].tsc_diff   = tsc_diff;

		/* Short delay to ensure we don't DoS the device */
		ndelay(800);
	}
}

static unsigned int __init get_tsc_overhead(void)
{
	unsigned long flags, sum;
	u32 tsc_high_before, tsc_high_after;
	u32 tsc_low_before, tsc_low_after;
	unsigned int i;

	/*
	 * "Warmup" of the benchmarking code.
	 * This will put instructions into cache.
	 */
	get_tsc_top(tsc_high_before, tsc_low_before);
	get_tsc_bottom(tsc_high_after, tsc_low_after);
	get_tsc_top(tsc_high_before, tsc_low_before);
	get_tsc_bottom(tsc_high_after, tsc_low_after);

        /* Main latency measurement loop */
	sum = 0;
	for (i = 0; i < OVERHEAD_MEASURE_LOOPS; i++) {

		preempt_disable();
		raw_local_irq_save(flags);

		get_tsc_top(tsc_high_before, tsc_low_before);
		get_tsc_bottom(tsc_high_after, tsc_low_after);

		raw_local_irq_restore(flags);
		preempt_enable();

		/* Calculate delta; lower 32 Bit should be enough here */
	        sum += tsc_low_after - tsc_low_before;
	}

	return sum / OVERHEAD_MEASURE_LOOPS;
}

/*
 * sysfs attributes
 */
static ssize_t pcielat_tsc_freq_show(struct device *dev,
				     struct device_attribute *attr,
				     char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "%u\n", tsc_khz * 1000);
}

static ssize_t pcielat_tsc_overhead_show(struct device *dev,
					 struct device_attribute *attr,
					 char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "%u\n", tsc_overhead);
}

static ssize_t pcielat_loops_show(struct device *dev,
				  struct device_attribute *attr,
				  char *buf)
{
	struct pcielat_priv *priv = dev_get_drvdata(dev);

	return scnprintf(buf, PAGE_SIZE, "%u\n",
			 priv->options.loops);
}

static ssize_t pcielat_loops_store(struct device *dev,
				   struct device_attribute *attr,
				   const char *buf, size_t count)
{
	struct pcielat_priv *priv = dev_get_drvdata(dev);
	unsigned int loops;
	int err;

	sscanf(buf, "%u", &loops);

	/* sanity check */
	if ((loops == 0) || (loops > LOOPS_UPPER_LIMIT))
		return -EINVAL;

	/* alloc new mem only if loop count changed */
	if (loops != priv->options.loops) {
		if (!priv->result_data) {
			vfree(priv->result_data);
			priv->cur_resdata_size_in_bytes = 0;
		}

		priv->options.loops = loops;
		priv->result_data = vmalloc(priv->options.loops * sizeof(struct result_data_t));

		if (IS_ERR(priv->result_data))
		{
			err = PTR_ERR(priv->result_data);
			return -ENOMEM;
		}

		priv->cur_resdata_size_in_bytes = priv->options.loops * sizeof(struct result_data_t);
	}

	return count;
}

static ssize_t pcielat_target_bar_show(struct device *dev,
				       struct device_attribute *attr, char *buf)
{
	struct pcielat_priv *priv = dev_get_drvdata(dev);

	return scnprintf(buf, PAGE_SIZE, "%u\n", priv->options.target_bar);
}

static ssize_t pcielat_target_bar_store(struct device *dev,
					struct device_attribute *attr,
					const char *buf, size_t count)
{
	struct pcielat_priv *priv = dev_get_drvdata(dev);
	unsigned short bar;

	sscanf(buf, "%hx", &bar);

	if (bar <= 5)
		priv->options.target_bar = bar;

	return count;
}

static ssize_t pcielat_bar_offset_show(struct device *dev,
				       struct device_attribute *attr, char *buf)
{
	struct pcielat_priv *priv = dev_get_drvdata(dev);

	return scnprintf(buf, PAGE_SIZE, "%u\n", priv->options.bar_offset);
}

static ssize_t pcielat_bar_offset_store(struct device *dev,
					struct device_attribute *attr,
					const char *buf, size_t count)
{
	struct pcielat_priv *priv = dev_get_drvdata(dev);
	unsigned int offset;

	sscanf(buf, "%u", &offset);

	if (!(offset % 4)) /* 32bit aligned */
		priv->options.bar_offset = offset;

	return count;
}

static ssize_t pcielat_measure_store(struct device *dev,
				     struct device_attribute *attr,
				     const char *buf, size_t count)
{
	struct pcielat_priv *priv = dev_get_drvdata(dev);
	int target_bar_len;

	if (priv->options.loops == 0) {
		dev_info(dev, "Loop count for measurements not set!\n");
		return -EINVAL;
	}

	target_bar_len = priv->bar[priv->options.target_bar].len;

	if (target_bar_len < 0) {
		dev_info(dev, "Target BAR not mmaped!\n");
		return -EINVAL;
	}

	/* cancel if offset is to high */
	if (priv->options.bar_offset > (target_bar_len - 4)) {
		dev_info(dev, "target_bar_len: %d, offset: %d; range failure!\n",
			 target_bar_len,
			 priv->options.bar_offset);
		return -EINVAL;
	}

	do_benchmark(priv->bar[priv->options.target_bar].addr,
		     priv->options.bar_offset,
		     priv->options.loops,
		     priv->result_data);

	dev_info(dev, "Benchmark done with %d measure_loops for BAR%d, offset 0x%08x\n",
		 priv->options.loops,
		 priv->options.target_bar,
		 priv->options.bar_offset);

	return count;
}

static DEVICE_ATTR_RO(pcielat_tsc_freq);
static DEVICE_ATTR_RO(pcielat_tsc_overhead);
static DEVICE_ATTR_RW(pcielat_loops);
static DEVICE_ATTR_RW(pcielat_target_bar);
static DEVICE_ATTR_RW(pcielat_bar_offset);
static DEVICE_ATTR_WO(pcielat_measure);

static struct attribute *pcielat_attrs[] = {
	&dev_attr_pcielat_tsc_freq.attr,
	&dev_attr_pcielat_tsc_overhead.attr,
	&dev_attr_pcielat_loops.attr,
	&dev_attr_pcielat_target_bar.attr,
	&dev_attr_pcielat_bar_offset.attr,
	&dev_attr_pcielat_measure.attr,
	NULL,
};

ATTRIBUTE_GROUPS(pcielat);

/*
 * Module init functions
 */
static char *pci_char_devnode(struct device *dev, umode_t *mode)
{
	struct pci_dev *pdev = to_pci_dev(dev->parent);
	return kasprintf(GFP_KERNEL, DRIVER_NAME "/%02x:%02x.%x",
			 pdev->bus->number,
			 PCI_SLOT(pdev->devfn),
			 PCI_FUNC(pdev->devfn));
}

static int check_tsc_invariant(void)
{
	uint32_t edx;

	/* Check for RDTSCP instruction */
	asm volatile("cpuid"
		     : "=d" (edx)
		     : "a" (0x80000001)
		     : "rbx", "rcx"
		);

	if (edx | 0x8000000) {
		pr_info(DRIVER_NAME ": CPUID.80000001:EDX[bit 27] == 1, "
			"RDTSCP instruction available\n");
	}
	else {
		pr_info(DRIVER_NAME ": CPUID.80000001:EDX[bit 27] == 0, "
			"RDTSCP instruction not available\n"
			"Exiting here\n");
		return 0;
	}

	/* Check for TSC invariant bit */
	asm volatile("cpuid"
		     : "=d" (edx)
		     : "a" (0x80000007)
		     : "rbx", "rcx"
		);

	if (edx | 0x100) {
		pr_info(DRIVER_NAME ": CPUID.80000007:EDX[bit 8] == 1, "
			"TSC is invariant\n");
		return 1;
	}
	else {
		pr_info(DRIVER_NAME ": CPUID.80000007:EDX[bit 8] == 0, "
			"TSC is not invariant\n"
			"Exiting here\n");
		return 0;
	}
}

static int __init pci_init(void)
{
	int err;
	char *p, *id;

	/* Check if host is capable of benchmarking with TSC */
	if (!check_tsc_invariant())
		return  -EPERM;

	/* Print TSC frequency as measured from the kernel boot routines */
	pr_info(DRIVER_NAME ": TSC frequency: %d kHz\n", tsc_khz);

	/* calculate TSC overhead of the system */
	tsc_overhead = get_tsc_overhead();
	pr_info(DRIVER_NAME ": Overhead of TSC measurement: %d cycles\n", tsc_overhead);

	pcielat_class = class_create(THIS_MODULE, DRIVER_NAME);
	if (IS_ERR(pcielat_class)) {
		err = PTR_ERR(pcielat_class);
		return err;
	}
	pcielat_class->devnode = pci_char_devnode;
	pcielat_class->dev_groups = pcielat_groups;

	err = pci_register_driver(&pcielat_driver);
	if (err)
		goto failure_register_driver;

	/* no ids passed actually */
	if (ids[0] == '\0')
		return 0;

	/* add ids specified in the module parameter */
	p = ids;
	while ((id = strsep(&p, ","))) {
		unsigned int vendor, device, subvendor = PCI_ANY_ID,
			subdevice = PCI_ANY_ID, class=0, class_mask=0;
		int fields;

		if (!strlen(id))
			continue;

		fields = sscanf(id, "%x:%x:%x:%x:%x:%x",
				&vendor, &device, &subvendor, &subdevice,
				&class, &class_mask);

		if (fields < 2) {
			pr_warn(DRIVER_NAME ": invalid id string \"%s\"\n", id);
			continue;
		}

		pr_info(DRIVER_NAME ": add %04X:%04X sub=%04X:%04X cls=%08X/%08X\n",
			vendor, device, subvendor, subdevice, class, class_mask);

		err = pci_add_dynid(&pcielat_driver, vendor, device,
				    subvendor, subdevice, class, class_mask, 0);
		if (err)
			pr_warn(DRIVER_NAME ": failed to add dynamic id (%d)\n", err);
	}

	return 0;

failure_register_driver:
	class_destroy(pcielat_class);

	return err;
}

static void __exit pci_exit(void)
{
	pci_unregister_driver(&pcielat_driver);
	class_destroy(pcielat_class);
}

module_init(pci_init);
module_exit(pci_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Generic x86_64 PCIe latency measurement module");
MODULE_AUTHOR("Andre Richter <andre.o.richter@gmail.com>,"
	      "Institute for Integrated Systems,"
	      "Technische Universität München");
