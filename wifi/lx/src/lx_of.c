/*
 * The device tree, of which there is none.
 *
 * XNU is not told of the machine by a tree, and so every question the driver asks of one is
 * answered here with "there is no such thing": no machine is compatible with anything, no node
 * has a property, and there is no region of memory reserved for the radio by name (the radio's
 * memory is asked of the kernel, lx_os_radio_memory(), which knows where the machine puts it).
 * The driver's files that read a tree do so to find out what chip they are on and where its
 * memory is; under XNU the kernel says both (lx_os_option(), lx_os_radio_address()).
 *
 * These three were answered in lx_bus.c while that file was the bus of a radio inside the
 * processor; they are the same answers for a radio behind PCI, and are kept here so that both
 * have them.
 */
#include <linux/of.h>
#include <linux/of_reserved_mem.h>
#include <linux/ioport.h>

#include "lx.h"

int
of_property_read_string(const struct device_node *np, const char *name, const char **out)
{
	return (-EINVAL);
}

bool
of_machine_is_compatible(const char *compatible)
{
	return (false);
}

int
of_reserved_mem_region_to_resource(const struct device_node *np, unsigned int index,
    struct resource *res)
{
	return (-ENODEV);
}
