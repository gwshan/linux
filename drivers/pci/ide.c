// SPDX-License-Identifier: GPL-2.0
/* Copyright(c) 2024-2025 Intel Corporation. All rights reserved. */

/* PCIe r7.0 section 6.33 Integrity & Data Encryption (IDE) */

#define dev_fmt(fmt) "PCI/IDE: " fmt
#include <linux/bitfield.h>
#include <linux/bitops.h>
#include <linux/pci.h>
#include <linux/pci-ide.h>
#include <linux/pci_regs.h>
#include <linux/slab.h>
#include <linux/sysfs.h>

#include "pci.h"

static int __sel_ide_offset(u16 ide_cap, u8 nr_link_ide, u8 stream_index,
			    u8 nr_ide_mem)
{
	u32 offset = ide_cap + PCI_IDE_LINK_STREAM_0 +
		     nr_link_ide * PCI_IDE_LINK_BLOCK_SIZE;

	/*
	 * Assume a constant number of address association resources per
	 * stream index
	 */
	return offset + stream_index * PCI_IDE_SEL_BLOCK_SIZE(nr_ide_mem);
}

static int sel_ide_offset(struct pci_dev *pdev,
			  struct pci_ide_partner *settings)
{
	return __sel_ide_offset(pdev->ide_cap, pdev->nr_link_ide,
				settings->stream_index, pdev->nr_ide_mem);
}

void pci_ide_init(struct pci_dev *pdev)
{
	u8 nr_link_ide, nr_ide_mem, nr_streams;
	u16 ide_cap;
	u32 val;

	if (!pci_is_pcie(pdev))
		return;

	ide_cap = pci_find_ext_capability(pdev, PCI_EXT_CAP_ID_IDE);
	if (!ide_cap)
		return;

	pci_read_config_dword(pdev, ide_cap + PCI_IDE_CAP, &val);
	if ((val & PCI_IDE_CAP_SELECTIVE) == 0)
		return;

	/*
	 * Require endpoint IDE capability to be paired with IDE Root
	 * Port IDE capability.
	 */
	if (pci_pcie_type(pdev) == PCI_EXP_TYPE_ENDPOINT) {
		struct pci_dev *rp = pcie_find_root_port(pdev);

		if (!rp->ide_cap)
			return;
	}

	if (val & PCI_IDE_CAP_SEL_CFG)
		pdev->ide_cfg = 1;

	if (val & PCI_IDE_CAP_TEE_LIMITED)
		pdev->ide_tee_limit = 1;

	if (val & PCI_IDE_CAP_LINK)
		nr_link_ide = 1 + FIELD_GET(PCI_IDE_CAP_LINK_TC_NUM, val);
	else
		nr_link_ide = 0;

	nr_ide_mem = 0;
	nr_streams = min(1 + FIELD_GET(PCI_IDE_CAP_SEL_NUM, val),
			 CONFIG_PCI_IDE_STREAM_MAX);
	for (u8 i = 0; i < nr_streams; i++) {
		int pos = __sel_ide_offset(ide_cap, nr_link_ide, i, nr_ide_mem);
		int nr_assoc;
		u32 val;

		pci_read_config_dword(pdev, pos, &val);

		/*
		 * Let's not entertain streams that do not have a
		 * constant number of address association blocks
		 */
		nr_assoc = FIELD_GET(PCI_IDE_SEL_CAP_ASSOC_NUM, val);
		if (i && (nr_assoc != nr_ide_mem)) {
			pci_info(pdev, "Unsupported Selective Stream %d capability, SKIP the rest\n", i);
			nr_streams = i;
			break;
		}

		nr_ide_mem = nr_assoc;
	}

	pdev->ide_cap = ide_cap;
	pdev->nr_link_ide = nr_link_ide;
	pdev->nr_sel_ide = nr_streams;
	pdev->nr_ide_mem = nr_ide_mem;
}

struct stream_index {
	unsigned long *map;
	u8 max, stream_index;
};

static void free_stream_index(struct stream_index *stream)
{
	clear_bit_unlock(stream->stream_index, stream->map);
}

DEFINE_FREE(free_stream, struct stream_index *, if (_T) free_stream_index(_T))
static struct stream_index *alloc_stream_index(unsigned long *map, u8 max,
					       struct stream_index *stream)
{
	if (!max)
		return NULL;

	do {
		u8 stream_index = find_first_zero_bit(map, max);

		if (stream_index == max)
			return NULL;
		if (!test_and_set_bit_lock(stream_index, map)) {
			*stream = (struct stream_index) {
				.map = map,
				.max = max,
				.stream_index = stream_index,
			};
			return stream;
		}
		/* collided with another stream acquisition */
	} while (1);
}

/**
 * pci_ide_stream_alloc() - Reserve stream indices and probe for settings
 * @pdev: IDE capable PCIe Endpoint Physical Function
 *
 * Retrieve the Requester ID range of @pdev for programming its Root
 * Port IDE RID Association registers, and conversely retrieve the
 * Requester ID of the Root Port for programming @pdev's IDE RID
 * Association registers.
 *
 * Allocate a Selective IDE Stream Register Block instance per port.
 *
 * Allocate a platform stream resource from the associated host bridge.
 * Retrieve stream association parameters for Requester ID range and
 * address range restrictions for the stream.
 */
struct pci_ide *pci_ide_stream_alloc(struct pci_dev *pdev)
{
	/* EP, RP, + HB Stream allocation */
	struct stream_index __stream[PCI_IDE_HB + 1];
	struct pci_host_bridge *hb;
	struct pci_dev *rp;
	int num_vf, rid_end;

	if (!pci_is_pcie(pdev))
		return NULL;

	if (pci_pcie_type(pdev) != PCI_EXP_TYPE_ENDPOINT)
		return NULL;

	if (!pdev->ide_cap)
		return NULL;

	/*
	 * Catch buggy PCI platform initialization (missing
	 * pci_ide_init_nr_streams())
	 */
	hb = pci_find_host_bridge(pdev->bus);
	if (WARN_ON_ONCE(!hb->nr_ide_streams))
		return NULL;

	struct pci_ide *ide __free(kfree) = kzalloc(sizeof(*ide), GFP_KERNEL);
	if (!ide)
		return NULL;

	struct stream_index *hb_stream __free(free_stream) = alloc_stream_index(
		hb->ide_stream_map, hb->nr_ide_streams, &__stream[PCI_IDE_HB]);
	if (!hb_stream)
		return NULL;

	rp = pcie_find_root_port(pdev);
	struct stream_index *rp_stream __free(free_stream) = alloc_stream_index(
		rp->ide_stream_map, rp->nr_sel_ide, &__stream[PCI_IDE_RP]);
	if (!rp_stream)
		return NULL;

	struct stream_index *ep_stream __free(free_stream) = alloc_stream_index(
		pdev->ide_stream_map, pdev->nr_sel_ide, &__stream[PCI_IDE_EP]);
	if (!ep_stream)
		return NULL;

	/* for SR-IOV case, cover all VFs */
	num_vf = pci_num_vf(pdev);
	if (num_vf)
		rid_end = PCI_DEVID(pci_iov_virtfn_bus(pdev, num_vf),
				    pci_iov_virtfn_devfn(pdev, num_vf));
	else
		rid_end = pci_dev_id(pdev);

	*ide = (struct pci_ide) {
		.pdev = pdev,
		.partner = {
			[PCI_IDE_EP] = {
				.rid_start = pci_dev_id(rp),
				.rid_end = pci_dev_id(rp),
				.stream_index = no_free_ptr(ep_stream)->stream_index,
			},
			[PCI_IDE_RP] = {
				.rid_start = pci_dev_id(pdev),
				.rid_end = rid_end,
				.stream_index = no_free_ptr(rp_stream)->stream_index,
			},
		},
		.host_bridge_stream = no_free_ptr(hb_stream)->stream_index,
		.stream_id = -1,
	};

	return_ptr(ide);
}
EXPORT_SYMBOL_GPL(pci_ide_stream_alloc);

/**
 * pci_ide_stream_free() - unwind pci_ide_stream_alloc()
 * @ide: idle IDE settings descriptor
 *
 * Free all of the stream index (register block) allocations acquired by
 * pci_ide_stream_alloc(). The stream represented by @ide is assumed to
 * be unregistered and not instantiated in any device.
 */
void pci_ide_stream_free(struct pci_ide *ide)
{
	struct pci_dev *pdev = ide->pdev;
	struct pci_dev *rp = pcie_find_root_port(pdev);
	struct pci_host_bridge *hb = pci_find_host_bridge(pdev->bus);

	clear_bit_unlock(ide->partner[PCI_IDE_EP].stream_index,
			 pdev->ide_stream_map);
	clear_bit_unlock(ide->partner[PCI_IDE_RP].stream_index,
			 rp->ide_stream_map);
	clear_bit_unlock(ide->host_bridge_stream, hb->ide_stream_map);
	kfree(ide);
}
EXPORT_SYMBOL_GPL(pci_ide_stream_free);

/**
 * pci_ide_stream_release() - unwind and release an @ide context
 * @ide: partially or fully registered IDE settings descriptor
 *
 * In support of automatic cleanup of IDE setup routines perform IDE
 * teardown in expected reverse order of setup and with respect to which
 * aspects of IDE setup have successfully completed.
 *
 * Be careful that setup order mirrors this shutdown order. Otherwise,
 * open code releasing the IDE context.
 */
void pci_ide_stream_release(struct pci_ide *ide)
{
	struct pci_dev *pdev = ide->pdev;
	struct pci_dev *rp = pcie_find_root_port(pdev);

	if (ide->partner[PCI_IDE_RP].enable)
		pci_ide_stream_disable(rp, ide);

	if (ide->partner[PCI_IDE_EP].enable)
		pci_ide_stream_disable(pdev, ide);

	if (ide->partner[PCI_IDE_RP].setup)
		pci_ide_stream_teardown(rp, ide);

	if (ide->partner[PCI_IDE_EP].setup)
		pci_ide_stream_teardown(pdev, ide);

	if (ide->name)
		pci_ide_stream_unregister(ide);

	pci_ide_stream_free(ide);
}
EXPORT_SYMBOL_GPL(pci_ide_stream_release);

/**
 * pci_ide_stream_register() - Prepare to activate an IDE Stream
 * @ide: IDE settings descriptor
 *
 * After a Stream ID has been acquired for @ide, record the presence of
 * the stream in sysfs. The expectation is that @ide is immutable while
 * registered.
 */
int pci_ide_stream_register(struct pci_ide *ide)
{
	struct pci_dev *pdev = ide->pdev;
	struct pci_host_bridge *hb = pci_find_host_bridge(pdev->bus);
	u8 ep_stream, rp_stream;
	int rc;

	if (ide->stream_id < 0 || ide->stream_id > U8_MAX) {
		pci_err(pdev, "Setup fail: Invalid Stream ID: %d\n", ide->stream_id);
		return -ENXIO;
	}

	ep_stream = ide->partner[PCI_IDE_EP].stream_index;
	rp_stream = ide->partner[PCI_IDE_RP].stream_index;
	const char *name __free(kfree) = kasprintf(GFP_KERNEL, "stream%d.%d.%d",
						   ide->host_bridge_stream,
						   rp_stream, ep_stream);
	if (!name)
		return -ENOMEM;

	rc = sysfs_create_link(&hb->dev.kobj, &pdev->dev.kobj, name);
	if (rc)
		return rc;

	ide->name = no_free_ptr(name);

	return 0;
}
EXPORT_SYMBOL_GPL(pci_ide_stream_register);

/**
 * pci_ide_stream_unregister() - unwind pci_ide_stream_register()
 * @ide: idle IDE settings descriptor
 *
 * In preparation for freeing @ide, remove sysfs enumeration for the
 * stream.
 */
void pci_ide_stream_unregister(struct pci_ide *ide)
{
	struct pci_dev *pdev = ide->pdev;
	struct pci_host_bridge *hb = pci_find_host_bridge(pdev->bus);

	sysfs_remove_link(&hb->dev.kobj, ide->name);
	kfree(ide->name);
	ide->name = NULL;
}
EXPORT_SYMBOL_GPL(pci_ide_stream_unregister);

static int pci_ide_domain(struct pci_dev *pdev)
{
	if (pdev->fm_enabled)
		return pci_domain_nr(pdev->bus);
	return 0;
}

struct pci_ide_partner *pci_ide_to_settings(struct pci_dev *pdev, struct pci_ide *ide)
{
	if (!pci_is_pcie(pdev)) {
		pci_warn_once(pdev, "not a PCIe device\n");
		return NULL;
	}

	switch (pci_pcie_type(pdev)) {
	case PCI_EXP_TYPE_ENDPOINT:
		if (pdev != ide->pdev) {
			pci_warn_once(pdev, "setup expected Endpoint: %s\n", pci_name(ide->pdev));
			return NULL;
		}
		return &ide->partner[PCI_IDE_EP];
	case PCI_EXP_TYPE_ROOT_PORT: {
		struct pci_dev *rp = pcie_find_root_port(ide->pdev);

		if (pdev != rp) {
			pci_warn_once(pdev, "setup expected Root Port: %s\n",
				      pci_name(rp));
			return NULL;
		}
		return &ide->partner[PCI_IDE_RP];
	}
	default:
		pci_warn_once(pdev, "invalid device type\n");
		return NULL;
	}
}
EXPORT_SYMBOL_GPL(pci_ide_to_settings);

static void set_ide_sel_ctl(struct pci_dev *pdev, struct pci_ide *ide, int pos,
			    bool enable)
{
	u32 val = FIELD_PREP(PCI_IDE_SEL_CTL_ID, ide->stream_id) |
		  FIELD_PREP(PCI_IDE_SEL_CTL_CFG_EN, pdev->ide_cfg) |
		  FIELD_PREP(PCI_IDE_SEL_CTL_TEE_LIMITED, pdev->ide_tee_limit) |
		  FIELD_PREP(PCI_IDE_SEL_CTL_EN, enable);

	pci_write_config_dword(pdev, pos + PCI_IDE_SEL_CTL, val);
}

/**
 * pci_ide_stream_setup() - program settings to Selective IDE Stream registers
 * @pdev: PCIe device object for either a Root Port or Endpoint Partner Port
 * @ide: registered IDE settings descriptor
 *
 * When @pdev is a PCI_EXP_TYPE_ENDPOINT then the PCI_IDE_EP partner
 * settings are written to @pdev's Selective IDE Stream register block,
 * and when @pdev is a PCI_EXP_TYPE_ROOT_PORT, the PCI_IDE_RP settings
 * are selected.
 */
void pci_ide_stream_setup(struct pci_dev *pdev, struct pci_ide *ide)
{
	struct pci_ide_partner *settings = pci_ide_to_settings(pdev, ide);
	int pos;
	u32 val;

	if (!settings)
		return;

	pos = sel_ide_offset(pdev, settings);

	val = FIELD_PREP(PCI_IDE_SEL_RID_1_LIMIT, settings->rid_end);
	pci_write_config_dword(pdev, pos + PCI_IDE_SEL_RID_1, val);

	val = FIELD_PREP(PCI_IDE_SEL_RID_2_VALID, 1) |
	      FIELD_PREP(PCI_IDE_SEL_RID_2_BASE, settings->rid_start) |
	      FIELD_PREP(PCI_IDE_SEL_RID_2_SEG, pci_ide_domain(pdev));

	pci_write_config_dword(pdev, pos + PCI_IDE_SEL_RID_2, val);

	/*
	 * Setup control register early for devices that expect
	 * stream_id is set during key programming.
	 */
	set_ide_sel_ctl(pdev, ide, pos, false);
	settings->setup = 1;
}
EXPORT_SYMBOL_GPL(pci_ide_stream_setup);

/**
 * pci_ide_stream_teardown() - disable the stream and clear all settings
 * @pdev: PCIe device object for either a Root Port or Endpoint Partner Port
 * @ide: registered IDE settings descriptor
 *
 * For stream destruction, zero all registers that may have been written
 * by pci_ide_stream_setup(). Consider pci_ide_stream_disable() to leave
 * settings in place while temporarily disabling the stream.
 */
void pci_ide_stream_teardown(struct pci_dev *pdev, struct pci_ide *ide)
{
	struct pci_ide_partner *settings = pci_ide_to_settings(pdev, ide);
	int pos;

	if (!settings)
		return;

	pos = sel_ide_offset(pdev, settings);

	pci_write_config_dword(pdev, pos + PCI_IDE_SEL_CTL, 0);
	pci_write_config_dword(pdev, pos + PCI_IDE_SEL_RID_2, 0);
	pci_write_config_dword(pdev, pos + PCI_IDE_SEL_RID_1, 0);
	settings->setup = 0;
}
EXPORT_SYMBOL_GPL(pci_ide_stream_teardown);

/**
 * pci_ide_stream_enable() - enable a Selective IDE Stream
 * @pdev: PCIe device object for either a Root Port or Endpoint Partner Port
 * @ide: registered and setup IDE settings descriptor
 *
 * Activate the stream by writing to the Selective IDE Stream Control
 * Register.
 *
 * Return: 0 if the stream successfully entered the "secure" state, and -ENXIO
 * otherwise.
 *
 * Note that the state may go "insecure" at any point after returning 0, but
 * those events are equivalent to a "link down" event and handled via
 * asynchronous error reporting.
 */
int pci_ide_stream_enable(struct pci_dev *pdev, struct pci_ide *ide)
{
	struct pci_ide_partner *settings = pci_ide_to_settings(pdev, ide);
	int pos;
	u32 val;

	if (!settings)
		return -ENXIO;

	pos = sel_ide_offset(pdev, settings);

	set_ide_sel_ctl(pdev, ide, pos, true);

	pci_read_config_dword(pdev, pos + PCI_IDE_SEL_STS, &val);
	if (FIELD_GET(PCI_IDE_SEL_STS_STATE, val) !=
	    PCI_IDE_SEL_STS_STATE_SECURE) {
		set_ide_sel_ctl(pdev, ide, pos, false);
		return -ENXIO;
	}

	settings->enable = 1;
	return 0;
}
EXPORT_SYMBOL_GPL(pci_ide_stream_enable);

/**
 * pci_ide_stream_disable() - disable a Selective IDE Stream
 * @pdev: PCIe device object for either a Root Port or Endpoint Partner Port
 * @ide: registered and setup IDE settings descriptor
 *
 * Clear the Selective IDE Stream Control Register, but leave all other
 * registers untouched.
 */
void pci_ide_stream_disable(struct pci_dev *pdev, struct pci_ide *ide)
{
	struct pci_ide_partner *settings = pci_ide_to_settings(pdev, ide);
	int pos;

	if (!settings)
		return;

	pos = sel_ide_offset(pdev, settings);

	pci_write_config_dword(pdev, pos + PCI_IDE_SEL_CTL, 0);
	settings->enable = 0;
}
EXPORT_SYMBOL_GPL(pci_ide_stream_disable);

static ssize_t available_secure_streams_show(struct device *dev,
					     struct device_attribute *attr,
					     char *buf)
{
	struct pci_host_bridge *hb = to_pci_host_bridge(dev);
	int avail;

	if (!hb->nr_ide_streams)
		return -ENXIO;

	avail = hb->nr_ide_streams -
		bitmap_weight(hb->ide_stream_map, hb->nr_ide_streams);
	return sysfs_emit(buf, "%d\n", avail);
}
static DEVICE_ATTR_RO(available_secure_streams);

static struct attribute *pci_ide_attrs[] = {
	&dev_attr_available_secure_streams.attr,
	NULL
};

static umode_t pci_ide_attr_visible(struct kobject *kobj, struct attribute *a, int n)
{
	struct device *dev = kobj_to_dev(kobj);
	struct pci_host_bridge *hb = to_pci_host_bridge(dev);

	if (a == &dev_attr_available_secure_streams.attr)
		if (!hb->nr_ide_streams)
			return 0;

	return a->mode;
}

struct attribute_group pci_ide_attr_group = {
	.attrs = pci_ide_attrs,
	.is_visible = pci_ide_attr_visible,
};

/**
 * pci_ide_init_nr_streams() - sets size of the pool of IDE Stream resources
 * @hb: host bridge boundary for the stream pool
 * @nr: number of streams
 *
 * Platform PCI init and/or expert test module use only. Enable IDE
 * Stream establishment by setting the number of stream resources
 * available at the host bridge. Platform init code must set this before
 * the first pci_ide_stream_alloc() call.
 *
 * The "PCI_IDE" symbol namespace is required because this is typically
 * a detail that is settled in early PCI init. I.e. this export is not
 * for endpoint drivers.
 */
void pci_ide_init_nr_streams(struct pci_host_bridge *hb, u8 nr)
{
	hb->nr_ide_streams = nr;
	sysfs_update_group(&hb->dev.kobj, &pci_ide_attr_group);
}
EXPORT_SYMBOL_NS_GPL(pci_ide_init_nr_streams, "PCI_IDE");
