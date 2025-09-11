/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __PCI_TSM_H
#define __PCI_TSM_H
#include <linux/mutex.h>
#include <linux/pci.h>

struct pci_tsm;
struct kvm;
enum pci_tsm_req_scope;

/*
 * struct pci_tsm_ops - manage confidential links and security state
 * @link_ops: Coordinate PCIe SPDM and IDE establishment via a platform TSM.
 *	      Provide a secure session transport for TDISP state management
 *	      (typically bare metal physical function operations).
 * @sec_ops: Lock, unlock, and interrogate the security state of the
 *	     function via the platform TSM (typically virtual function
 *	     operations).
 * @owner: Back reference to the TSM device that owns this instance.
 *
 * This operations are mutually exclusive either a tsm_dev instance
 * manages physical link properties or it manages function security
 * states like TDISP lock/unlock.
 */
struct pci_tsm_ops {
	/*
	 * struct pci_tsm_link_ops - Manage physical link and the TSM/DSM session
	 * @probe: allocate context (wrap 'struct pci_tsm') for follow-on link
	 *	   operations
	 * @remove: destroy link operations context
	 * @connect: establish / validate a secure connection (e.g. IDE)
	 *	     with the device
	 * @disconnect: teardown the secure link
	 * @bind: bind a TDI in preparation for it to be accepted by a TVM
	 * @unbind: remove a TDI from secure operation with a TVM
	 *
	 * Context: @probe, @remove, @connect, and @disconnect run under
	 * pci_tsm_rwsem held for write to sync with TSM unregistration and
	 * mutual exclusion of @connect and @disconnect. @connect and
	 * @disconnect additionally run under the DSM lock (struct
	 * pci_tsm_pf0::lock) as well as @probe and @remove of the subfunctions.
	 * @bind and @unbind run under pci_tsm_rwsem held for read and the DSM
	 * lock.
	 */
	struct_group_tagged(pci_tsm_link_ops, link_ops,
		struct pci_tsm *(*probe)(struct pci_dev *pdev);
		void (*remove)(struct pci_tsm *tsm);
		int (*connect)(struct pci_dev *pdev);
		void (*disconnect)(struct pci_dev *pdev);
		struct pci_tdi *(*bind)(struct pci_dev *pdev,
					struct kvm *kvm, u32 tdi_id);
		void (*unbind)(struct pci_tdi *tdi);
	);

	/*
	 * struct pci_tsm_security_ops - Manage the security state of the function
	 * @lock: probe and initialize the device in the LOCKED state
	 * @unlock: destroy TSM context and return device to UNLOCKED state
	 *
	 * Context: @lock and @unlock run under pci_tsm_rwsem held for write to
	 * sync with TSM unregistration and each other
	 */
	struct_group_tagged(pci_tsm_security_ops, devsec_ops,
		struct pci_tsm *(*lock)(struct pci_dev *pdev);
		void (*unlock)(struct pci_dev *pdev);
	);
	struct tsm_dev *owner;
};

/**
 * struct pci_tdi - Core TEE I/O Device Interface (TDI) context
 * @pdev: host side representation of guest-side TDI
 * @kvm: TEE VM context of bound TDI
 */
struct pci_tdi {
	struct pci_dev *pdev;
	struct kvm *kvm;
};

/**
 * struct pci_tsm - Core TSM context for a given PCIe endpoint
 * @pdev: Back ref to device function, distinguishes type of pci_tsm context
 * @dsm: PCI Device Security Manager for link operations on @pdev
 * @tdi: TDI context established by the @bind link operation
 * @ops: Link Confidentiality or Device Function Security operations
 *
 * This structure is wrapped by low level TSM driver data and returned by
 * probe()/lock(), it is freed by the corresponding remove()/unlock().
 *
 * For link operations it serves to cache the association between a Device
 * Security Manager (DSM) and the functions that manager can assign to a TVM.
 * That can be "self", for assigning function0 of a TEE I/O device, a
 * sub-function (SR-IOV virtual function, or non-function0
 * multifunction-device), or a downstream endpoint (PCIe upstream switch-port as
 * DSM).
 */
struct pci_tsm {
	struct pci_dev *pdev;
	struct pci_dev *dsm;
	struct pci_tdi *tdi;
	const struct pci_tsm_ops *ops;
};

/**
 * struct pci_tsm_pf0 - Physical Function 0 TDISP link context
 * @base: generic core "tsm" context
 * @lock: mutual exclustion for pci_tsm_ops invocation
 * @doe_mb: PCIe Data Object Exchange mailbox
 */
struct pci_tsm_pf0 {
	struct pci_tsm base;
	struct mutex lock;
	struct pci_doe_mb *doe_mb;
};

/* physical function0 and capable of 'connect' */
static inline bool is_pci_tsm_pf0(struct pci_dev *pdev)
{
	if (!pci_is_pcie(pdev))
		return false;

	if (pdev->is_virtfn)
		return false;

	/*
	 * Allow for a Device Security Manager (DSM) associated with function0
	 * of an Endpoint to coordinate TDISP requests for other functions
	 * (physical or virtual) of the device, or allow for an Upstream Port
	 * DSM to accept TDISP requests for the Endpoints downstream of the
	 * switch.
	 */
	switch (pci_pcie_type(pdev)) {
	case PCI_EXP_TYPE_ENDPOINT:
	case PCI_EXP_TYPE_UPSTREAM:
	case PCI_EXP_TYPE_RC_END:
		if (pdev->ide_cap || (pdev->devcap & PCI_EXP_DEVCAP_TEE))
			break;
		fallthrough;
	default:
		return false;
	}

	return PCI_FUNC(pdev->devfn) == 0;
}

#ifdef CONFIG_PCI_TSM
struct tsm_dev;
int pci_tsm_register(struct tsm_dev *tsm_dev);
void pci_tsm_unregister(struct tsm_dev *tsm_dev);
int pci_tsm_link_constructor(struct pci_dev *pdev, struct pci_tsm *tsm,
			     const struct pci_tsm_ops *ops);
int pci_tsm_pf0_constructor(struct pci_dev *pdev, struct pci_tsm_pf0 *tsm,
			    const struct pci_tsm_ops *ops);
void pci_tsm_pf0_destructor(struct pci_tsm_pf0 *tsm);
int pci_tsm_bind(struct pci_dev *pdev, struct kvm *kvm, u32 tdi_id);
void pci_tsm_unbind(struct pci_dev *pdev);
#else
static inline int pci_tsm_register(struct tsm_dev *tsm_dev)
{
	return 0;
}
static inline void pci_tsm_unregister(struct tsm_dev *tsm_dev)
{
}
static inline int pci_tsm_bind(struct pci_dev *pdev, struct kvm *kvm, u64 tdi_id)
{
	return -ENXIO;
}
static inline void pci_tsm_unbind(struct pci_dev *pdev)
{
}
#endif
#endif /*__PCI_TSM_H */
