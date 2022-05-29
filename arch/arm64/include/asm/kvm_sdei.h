/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Definitions of various KVM SDEI events.
 *
 * Copyright (C) 2022 Red Hat, Inc.
 *
 * Author(s): Gavin Shan <gshan@redhat.com>
 */

#ifndef __ARM64_KVM_SDEI_H__
#define __ARM64_KVM_SDEI_H__

#include <uapi/linux/arm_sdei.h>
#include <linux/arm-smccc.h>
#include <linux/bits.h>

enum {
	KVM_SDEI_EVENT_SW_SIGNALED = 0,
	KVM_SDEI_EVENT_ASYNC_PF,
	KVM_NR_SDEI_EVENTS,
};

/**
 * struct kvm_sdei_event_handler - SDEI event handler
 *
 * @ep_addr:	Address of SDEI event handler
 * @ep_arg:	Argument passed to SDEI event handler
 */
struct kvm_sdei_event_handler {
	unsigned long	ep_addr;
	unsigned long	ep_arg;
};

/**
 * struct kvm_sdei_event_context - Saved context during SDEI event delivery
 *
 * @pc:		PC of the saved context
 * @pstate:	PSTATE of the saved context
 * @regs:	x0 to x17 of the saved context
 */
struct kvm_sdei_event_context {
	unsigned long	pc;
	unsigned long	pstate;
	unsigned long	regs[18];
};

/**
 * struct kvm_sdei_vcpu - SDEI events and their sates
 *
 * @registered:	Bitmap of registration states for SDEI events
 * @enabled:	Bitmap of enablement states for SDEI events
 * @running:	Bitmap of running states for SDEI events
 * @pending:	Bitmap of pending states for SDEI events
 * @handlers:	Array of SDEI event handlers
 * @ctxt:	Saved context during SDEI event delivery
 */
struct kvm_sdei_vcpu {
	unsigned long			registered;
	unsigned long			enabled;
	unsigned long			running;
	unsigned long			pending;

	struct kvm_sdei_event_handler	handlers[KVM_NR_SDEI_EVENTS];
	struct kvm_sdei_event_context	ctxt;
};

/* Returned as vendor through SDEI_VERSION hypercall */
#define KVM_SDEI_VENDOR	ARM_SMCCC_VENDOR_HYP_UID_KVM_REG_2

/* APIs */
int kvm_sdei_call(struct kvm_vcpu *vcpu);
int kvm_sdei_inject_event(struct kvm_vcpu *vcpu,
			  unsigned int num, bool immediate);
int kvm_sdei_cancel_event(struct kvm_vcpu *vcpu, unsigned int num);
void kvm_sdei_deliver_event(struct kvm_vcpu *vcpu);
void kvm_sdei_create_vcpu(struct kvm_vcpu *vcpu);
void kvm_sdei_destroy_vcpu(struct kvm_vcpu *vcpu);

#endif /* __ARM64_KVM_SDEI_H__ */
