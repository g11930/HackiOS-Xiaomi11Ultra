/*
 * What the driver's PCI bus (pci.c, mhi.c) asks of Linux that is not under XNU:
 * two questions of a device tree there is none of, and two of the kernel's MHI that
 * this phone's radio does not need -- the driver's own mhi.c does the resetting and the
 * starting of the controller; what is left is a controller's power-down that keeps the
 * device (ath11k calls it when it stops) and the taking of a dump of the radio's RAM when
 * the firmware has panicked, which nothing here reads.
 *
 * There is no device tree under XNU, and so no "memory" node to answer the first two with:
 * they are answered with "there is none", which is what mhi.c expects when it cannot find
 * the region it is looking for.
 */
#include <linux/types.h>
#include <linux/errno.h>
#include <linux/of.h>
#include <linux/ioport.h>

#include "lx.h"

struct mhi_controller;			/* only its address is passed on */

struct device_node *
of_find_node_by_type(struct device_node *from, const char *type)
{
	return (NULL);
}

int
of_address_to_resource(struct device_node *np, int index, struct resource *r)
{
	return (-EINVAL);
}

void
mhi_power_down_keep_dev(struct mhi_controller *mhi_cntrl, bool keep_dev)
{
}

void
mhi_download_rddm_image(struct mhi_controller *mhi_cntrl, bool in_panic)
{
}

int
irq_set_affinity_and_hint(unsigned int irq, const void *mask)
{
	/* (the kernel has put the radio's interrupts where it wants them) */
	return (0);
}
