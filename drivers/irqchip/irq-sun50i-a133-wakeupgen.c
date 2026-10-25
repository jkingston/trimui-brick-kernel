// SPDX-License-Identifier: GPL-2.0-only
/* A133 stock-monitor wake routing; ordinary interrupts remain owned by GIC. */
#include <linux/arm-smccc.h>
#include <linux/errno.h>
#include <linux/irq.h>
#include <linux/irqchip.h>
#include <linux/irqdomain.h>
#include <linux/of.h>

#include <dt-bindings/interrupt-controller/arm-gic.h>

#define A133_MONITOR_UID		0x8000ff01
#define A133_CLEAR_WAKE_SOURCE	0xc0000025
#define A133_SET_WAKE_SOURCE	0xc0000026
#define A133_NR_SPIS		(1020 - 32)

static bool __init a133_wakeupgen_monitor_supported(void)
{
	struct arm_smccc_res res;

	/* Read-only identity query: never probe support by changing wake sources. */
	arm_smccc_smc(A133_MONITOR_UID, 0, 0, 0, 0, 0, 0, 0, &res);
	return (u32)res.a0 == 0x83b53a5b && (u32)res.a1 == 0x40b9c594 &&
	       (u32)res.a2 == 0xfcf29153 && (u32)res.a3 == 0x48862954;
}

static int a133_wakeupgen_set_wake(struct irq_data *data, unsigned int on)
{
	struct arm_smccc_res res;

	/* The firmware takes the DT SPI index, not GIC INTID (index + 32). */
	arm_smccc_smc(on ? A133_SET_WAKE_SOURCE : A133_CLEAR_WAKE_SOURCE,
		      data->hwirq, 0, 0, 0, 0, 0, 0, &res);
	if ((s32)res.a0 == SMCCC_RET_NOT_SUPPORTED)
		return -EOPNOTSUPP;
	/* The SCP response is an unsigned result byte, not a Linux errno. */
	return res.a0 ? -EIO : 0;
}

static struct irq_chip a133_wakeupgen_chip = {
	.name = "a133-wakeupgen",
	.irq_enable = irq_chip_enable_parent,
	.irq_disable = irq_chip_disable_parent,
	.irq_mask = irq_chip_mask_parent,
	.irq_unmask = irq_chip_unmask_parent,
	.irq_eoi = irq_chip_eoi_parent,
	.irq_retrigger = irq_chip_retrigger_hierarchy,
	.irq_set_type = irq_chip_set_type_parent,
	.irq_set_affinity = irq_chip_set_affinity_parent,
	.irq_set_wake = a133_wakeupgen_set_wake,
	.flags = IRQCHIP_SET_TYPE_MASKED,
};

static int a133_wakeupgen_translate(struct irq_domain *domain,
				   struct irq_fwspec *fwspec,
				   irq_hw_number_t *hwirq, unsigned int *type)
{
	if (!is_of_node(fwspec->fwnode) || fwspec->param_count != 3 ||
	    fwspec->param[0] != GIC_SPI || fwspec->param[1] >= A133_NR_SPIS)
		return -EINVAL;

	*hwirq = fwspec->param[1];
	*type = fwspec->param[2] & IRQ_TYPE_SENSE_MASK;
	return 0;
}

static int a133_wakeupgen_alloc(struct irq_domain *domain, unsigned int virq,
			       unsigned int nr_irqs, void *arg)
{
	struct irq_fwspec *fwspec = arg, parent_fwspec;
	irq_hw_number_t hwirq;
	unsigned int type, i;
	int ret;

	ret = a133_wakeupgen_translate(domain, fwspec, &hwirq, &type);
	if (ret)
		return ret;
	if (!nr_irqs || nr_irqs > A133_NR_SPIS - hwirq)
		return -EINVAL;

	parent_fwspec = *fwspec;
	parent_fwspec.fwnode = domain->parent->fwnode;
	ret = irq_domain_alloc_irqs_parent(domain, virq, nr_irqs, &parent_fwspec);
	if (ret)
		return ret;

	for (i = 0; i < nr_irqs; i++) {
		ret = irq_domain_set_hwirq_and_chip(domain, virq + i, hwirq + i,
						    &a133_wakeupgen_chip, NULL);
		if (ret) {
			irq_domain_free_irqs_common(domain, virq, nr_irqs);
			return ret;
		}
	}
	return 0;
}

static const struct irq_domain_ops a133_wakeupgen_ops = {
	.translate = a133_wakeupgen_translate,
	.alloc = a133_wakeupgen_alloc,
	.free = irq_domain_free_irqs_common,
};

static int __init a133_wakeupgen_init(struct device_node *node,
				    struct device_node *parent)
{
	struct irq_domain *parent_domain;

	if (!parent)
		return -EINVAL;
	parent_domain = irq_find_host(parent);
	if (!parent_domain)
		return -ENXIO;
	if (!a133_wakeupgen_monitor_supported()) {
		pr_err("A133 wakeupgen requires the stock secure monitor\n");
		return -ENODEV;
	}
	if (!irq_domain_create_hierarchy(parent_domain, 0, A133_NR_SPIS,
					 of_fwnode_handle(node),
				      &a133_wakeupgen_ops, NULL))
		return -ENOMEM;
	return 0;
}
IRQCHIP_DECLARE(a133_wakeupgen, "allwinner,sun50i-a133-wakeupgen", a133_wakeupgen_init);
