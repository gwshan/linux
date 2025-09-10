#ifndef __LINUX_DEBUG_H__
#define __LINUX_DEBUG_H__

#include <linux/device.h>
#include <linux/pci.h>

#define KERN_DBG(debug, x...)			\
	do {					\
		if (debug)			\
			printk(KERN_INFO x);	\
	} while(0)

static inline bool kern_dbg_is_target(struct device *dev)
{
	struct pci_dev *pdev = dev_is_pci(dev) ? to_pci_dev(dev) : NULL;

	if (!pdev				||
	    pci_domain_nr(pdev->bus) != 0	||
	    pdev->bus->number != 7		||
	    PCI_SLOT(pdev->devfn) != 0		||
	    PCI_FUNC(pdev->devfn) != 0)
		return false;

	return true;
}

#endif /* __LINUX_DEBUG_H__ */
