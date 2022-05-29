// SPDX-License-Identifier: GPL-2.0-only
/*
 * SDEI virtualization test support.
 *
 * Copyright (C) 2022 Red Hat, Inc.
 *
 * Author(s): Gavin Shan <gshan@redhat.com>
 */

#include <linux/kernel.h>
#include <linux/kvm_host.h>
#include <linux/spinlock.h>
#include <linux/slab.h>
#include <linux/proc_fs.h>
#include <kvm/arm_hypercalls.h>
#include <asm/kvm_sdei.h>

#ifdef KVM_SDEI_TEST

static struct proc_dir_entry *pde;

static int proc_show(struct seq_file *m, void *v)
{
	struct kvm_vcpu *vcpu = (struct kvm_vcpu *)(m->private);
	struct kvm *kvm = vcpu->kvm;
	struct kvm_sdei_vcpu *vsdei = vcpu->arch.sdei;
	struct kvm_sdei_event_context *ctxt = &vsdei->ctxt;
	unsigned int i;

	if (!vsdei)
		return 0;

	seq_printf(m, "=============== %d-%d ===============\n",
		   kvm->userspace_pid, vcpu->vcpu_idx);
	seq_puts(m, "\n");
	seq_printf(m, "  vCPU masked:   %s\n",
		   (vcpu->arch.flags & KVM_ARM64_SDEI_MASKED) ? "Yes" : "No");
	seq_printf(m, "  registered:    %016lx\n", vsdei->registered);
	seq_printf(m, "  enabled:       %016lx\n", vsdei->enabled);
	seq_printf(m, "  running:       %016lx\n", vsdei->running);
	seq_printf(m, "  pending:       %016lx\n", vsdei->pending);

	seq_puts(m, "\n");
	for (i = 0; i < KVM_NR_SDEI_EVENTS; i++) {
		seq_printf(m, "  handlers[%d].ep_addr:  %016lx\n",
			   i, vsdei->handlers[i].ep_addr);
		seq_printf(m, "  handlers[%d].ep_arg:   %016lx\n",
			   i, vsdei->handlers[i].ep_arg);
	}

	seq_puts(m, "\n");
	seq_puts(m, "  Context:\n");
	seq_printf(m, "      PC:      %016lx\n", ctxt->pc);
	seq_printf(m, "      PSTATE:  %016lx\n", ctxt->pstate);
	seq_printf(m, "      Regs:    %016lx %016lx %016lx %016lx\n"
		      "               %016lx %016lx %016lx %016lx\n"
		      "               %016lx %016lx %016lx %016lx\n"
		      "               %016lx %016lx %016lx %016lx\n"
		      "               %016lx %016lx\n",
		   ctxt->regs[0],  ctxt->regs[1],
		   ctxt->regs[2],  ctxt->regs[3],
		   ctxt->regs[4],  ctxt->regs[5],
		   ctxt->regs[6],  ctxt->regs[7],
		   ctxt->regs[8],  ctxt->regs[9],
		   ctxt->regs[10], ctxt->regs[11],
		   ctxt->regs[12], ctxt->regs[13],
		   ctxt->regs[14], ctxt->regs[15],
		   ctxt->regs[16], ctxt->regs[17]);

	return 0;
}

int proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, proc_show, pde_data(inode));
}

static ssize_t proc_write(struct file *file,
			  const char __user *buf,
			  size_t count, loff_t *pos)
{
	struct kvm_vcpu *vcpu = (struct kvm_vcpu *)pde_data(file_inode(file));
	struct kvm *kvm = vcpu->kvm;
	int ret;

	ret = kvm_sdei_inject_event(vcpu, KVM_SDEI_EVENT_SW_SIGNALED, false);
	if (ret) {
		pr_warn("%s: Error %d to inject event 0x%x to %d-%d\n",
			__func__, ret, KVM_SDEI_EVENT_SW_SIGNALED,
			kvm->userspace_pid, vcpu->vcpu_idx);
		return ret;
	}

	pr_info("%s: Succeed to inject event 0x%x to %d-%d\n",
		__func__, KVM_SDEI_EVENT_SW_SIGNALED,
		kvm->userspace_pid, vcpu->vcpu_idx);

	return count;
}

static const struct proc_ops kvm_sdei_fops = {
	.proc_open	= proc_open,
	.proc_read	= seq_read,
	.proc_lseek	= seq_lseek,
	.proc_write	= proc_write,
	.proc_release	= single_release,
};

void kvm_sdei_test_create_vcpu(struct kvm_vcpu *vcpu)
{
	struct kvm *kvm = vcpu->kvm;
	struct kvm_sdei_vcpu *vsdei = vcpu->arch.sdei;
	char name[64];

	if (!vsdei)
		return;

	if (!pde) {
		pde = proc_mkdir("kvm", NULL);
		if (!pde) {
			pr_warn("%s: Unable to create /proc/kvm\n",
				__func__);
			return;
		}
	}

	memset(name, 0, sizeof(name));
	snprintf(name, sizeof(name), "kvm-%d-vcpu-%d",
		 kvm->userspace_pid, atomic_read(&vcpu->kvm->online_vcpus));
	vsdei->pde = proc_create_data(name, 0600, pde, &kvm_sdei_fops, vcpu);
}

void kvm_sdei_test_destroy_vcpu(struct kvm_vcpu *vcpu)
{
	struct kvm_sdei_vcpu *vsdei = vcpu->arch.sdei;

	if (vsdei && vsdei->pde) {
		proc_remove(vsdei->pde);
		vsdei->pde = NULL;
	}
}

#endif /* KVM_SDEI_TEST */
