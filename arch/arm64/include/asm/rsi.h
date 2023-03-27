/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2024 ARM Ltd.
 */

#ifndef __ASM_RSI_H_
#define __ASM_RSI_H_

#include <linux/jump_label.h>
#include <asm/rsi_cmds.h>

DECLARE_STATIC_KEY_FALSE(rsi_present);

void __init arm64_rsi_init(void);
void __init arm64_rsi_setup_memory(void);
static inline bool is_realm_world(void)
{
	return static_branch_unlikely(&rsi_present);
}

static inline int rsi_set_memory_range(phys_addr_t start, phys_addr_t end,
				       enum ripas state)
{
	unsigned long ret;
	phys_addr_t top;

	while (start != end) {
		ret = rsi_set_addr_range_state(start, end, state, &top);
		if (WARN_ON(ret || top < start || top > end))
			return -EINVAL;
		start = top;
	}

	return 0;
}

static inline int rsi_set_memory_range_protected(phys_addr_t start,
						 phys_addr_t end)
{
	return rsi_set_memory_range(start, end, RSI_RIPAS_RAM);
}

static inline int rsi_set_memory_range_shared(phys_addr_t start,
					      phys_addr_t end)
{
	return rsi_set_memory_range(start, end, RSI_RIPAS_EMPTY);
}
#endif
