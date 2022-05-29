.. SPDX-License-Identifier: GPL-2.0

=====================================
SDEI Virtualization Support for ARM64
=====================================

ARM specification DEN0054/C defines Software Delegated Exception Interface
(SDEI). It provides a mechanism for registering and servicing system events
from system firmware. The interface is offered by a higher exception level
to a lower exception level, in other words by a secure platform firmware
to hypervisor or hypervisor to OS or both.

https://developer.arm.com/documentation/den0054/c

KVM/arm64 implementation follows the specification to support the defined
hypercalls so that the system events can be registered and serviced from
KVM hypervisor to the guest OS. However, some of specified functionalities
are missed by the implementation.

  * The shared event is not supported. It means all supported events are
    private. They need to be registered, enabled, disabled, unregistered
    and reset from one particular PE (vCPU).

  * The critical priority is not supported. It means all supported events
    have normal priority. So there is no preemption between two events
    in the critical and normal priorities. One event can be running at
    once on one particular PE (vCPU).

  * Interrupt binding event is not supported. It means all supported
    events triggered by software.

  * Relative mode for the event handler's address is not supported.
    The event handler address is always an absolute address.

The event handlers, states and context need to be migrated. Several pseudo
firmware registers are added for this.

  * KVM_REG_ARM_SDEI_EVENT_HANDLER_0
    KVM_REG_ARM_SDEI_EVENT_HANDLER_1
    KVM_REG_ARM_SDEI_EVENT_HANDLER_2
    KVM_REG_ARM_SDEI_EVENT_HANDLER_3

    The event handler's address and argument for events, whose number
    are 0, 1, 2 and 3 respectively. Currently, there are only two
    supported events, which are Software Signaled Event (0) and Async
    PF Event (1). So the first two registers are only used. zeroes are
    returned on reading KVM_REG_ARM_SDEI_EVENT_HANDLER_{2, 3}, and the
    values written to them are ignored.

  * KVM_REG_ARM_SDEI_EVENT_REGISTERED
    KVM_REG_ARM_SDEI_EVENT_ENABLED
    KVM_REG_ARM_SDEI_EVENT_RUNNING
    KVM_REG_ARM_SDEI_EVENT_PENDING

    They are mapped to the registered, enabled, running and pending
    bitmap respectively.

  * KVM_REG_ARM_SDEI_EVENT_CONTEXT

    The interrupted context.

  * KVM_REG_ARM_SDEI_PE_STATE

    Relect the SDEI masked state on the PE (vCPU).
