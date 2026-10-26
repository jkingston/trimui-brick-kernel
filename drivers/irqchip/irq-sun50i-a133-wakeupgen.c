// SPDX-License-Identifier: GPL-2.0-only
/* A133 firmware wake routing; ordinary interrupts remain owned by GIC. */
#include <linux/arm-smccc.h>
#include <linux/bits.h>
#include <linux/errno.h>
#include <linux/irq.h>
#include <linux/irqchip.h>
#include <linux/irqchip/irq-sun50i-a133-wakeupgen.h>
#include <linux/irqdomain.h>
#include <linux/of.h>

#include <dt-bindings/interrupt-controller/arm-gic.h>

#define A133_STOCK_MONITOR_UID		0x8000ff01
#define A133_STOCK_CLEAR_WAKE_SOURCE	0xc0000025
#define A133_STOCK_SET_WAKE_SOURCE	0xc0000026

#define A133_NATIVE_UID			0x8200ff01
#define A133_NATIVE_VERSION		0x8200ff03
#define A133_NATIVE_CAPABILITIES		0x82000100
#define A133_NATIVE_SET_WAKE_SOURCE	0x82000101
#define A133_NATIVE_CLEAR_WAKE_SOURCE	0x82000102

#define A133_NATIVE_UID_WORD0		0x28c05220
#define A133_NATIVE_UID_WORD1		0x0c5421f9
#define A133_NATIVE_UID_WORD2		0xa0bb3083
#define A133_NATIVE_UID_WORD3		0x72c7a28b
#define A133_NATIVE_VERSION_MAJOR	1
#define A133_NATIVE_CAP_WAKE_SOURCE	BIT(0)
#define A133_NATIVE_CAP_RAW_SPI_INDEX	BIT(1)
#define A133_NATIVE_REQUIRED_CAPS	(A133_NATIVE_CAP_WAKE_SOURCE | \
					 A133_NATIVE_CAP_RAW_SPI_INDEX)

#define A133_NATIVE_SUCCESS		0
#define A133_NATIVE_NOT_SUPPORTED	-1
#define A133_NATIVE_INVALID_PARAMETER	-4
#define A133_NATIVE_NOT_READY		-5
#define A133_NATIVE_TIMEOUT		-6
#define A133_NATIVE_POISONED		-7
#define A133_NATIVE_PROTOCOL_ERROR	-8
#define A133_NATIVE_REMOTE_ERROR		-9

#define A133_NR_SPIS			(1020 - 32)

struct a133_wakeupgen_profile {
	const char *name;
	int (*set_wake)(irq_hw_number_t hwirq, unsigned int on);
};

static int __init a133_wakeupgen_stock_probe(void)
{
	struct arm_smccc_res res;

	/* Read-only identity query: never probe support by changing wake sources. */
	arm_smccc_smc(A133_STOCK_MONITOR_UID, 0, 0, 0, 0, 0, 0, 0, &res);
	if ((u32)res.a0 != 0x83b53a5b || (u32)res.a1 != 0x40b9c594 ||
	    (u32)res.a2 != 0xfcf29153 || (u32)res.a3 != 0x48862954)
		return -ENODEV;

	return 0;
}

static int a133_wakeupgen_stock_set_wake(irq_hw_number_t hwirq,
					 unsigned int on)
{
	struct arm_smccc_res res;

	/* The firmware takes the DT SPI index, not GIC INTID (index + 32). */
	arm_smccc_smc(on ? A133_STOCK_SET_WAKE_SOURCE :
		      A133_STOCK_CLEAR_WAKE_SOURCE,
		      hwirq, 0, 0, 0, 0, 0, 0, &res);
	if ((s32)res.a0 == SMCCC_RET_NOT_SUPPORTED)
		return -EOPNOTSUPP;
	/* The SCP response is an unsigned result byte, not a Linux errno. */
	return res.a0 ? -EIO : 0;
}

static int a133_wakeupgen_native_status(u32 status, u32 scp_result)
{
	if ((s32)status == A133_NATIVE_SUCCESS)
		return scp_result ? -EREMOTEIO : 0;

	switch ((s32)status) {
	case A133_NATIVE_NOT_SUPPORTED:
		return -EOPNOTSUPP;
	case A133_NATIVE_INVALID_PARAMETER:
		return -EINVAL;
	case A133_NATIVE_NOT_READY:
		return -EAGAIN;
	case A133_NATIVE_TIMEOUT:
		return -ETIMEDOUT;
	case A133_NATIVE_POISONED:
		return -EIO;
	case A133_NATIVE_PROTOCOL_ERROR:
		return -EPROTO;
	case A133_NATIVE_REMOTE_ERROR:
		return -EREMOTEIO;
	default:
		return -EIO;
	}
}

static int a133_wakeupgen_native_set_wake(irq_hw_number_t hwirq,
					  unsigned int on)
{
	struct arm_smccc_res res;

	arm_smccc_smc(on ? A133_NATIVE_SET_WAKE_SOURCE :
		      A133_NATIVE_CLEAR_WAKE_SOURCE,
		      hwirq, 0, 0, 0, 0, 0, 0, &res);
	return a133_wakeupgen_native_status((u32)res.a0, (u32)res.a1);
}

static int __init a133_wakeupgen_native_probe(void)
{
	struct arm_smccc_res res;

	arm_smccc_smc(A133_NATIVE_UID, 0, 0, 0, 0, 0, 0, 0, &res);
	if ((u32)res.a0 != A133_NATIVE_UID_WORD0 ||
	    (u32)res.a1 != A133_NATIVE_UID_WORD1 ||
	    (u32)res.a2 != A133_NATIVE_UID_WORD2 ||
	    (u32)res.a3 != A133_NATIVE_UID_WORD3)
		return -ENODEV;

	arm_smccc_smc(A133_NATIVE_VERSION, 0, 0, 0, 0, 0, 0, 0, &res);
	if ((u32)res.a0 != A133_NATIVE_VERSION_MAJOR)
		return -EPROTONOSUPPORT;

	arm_smccc_smc(A133_NATIVE_CAPABILITIES, 0, 0, 0, 0, 0, 0, 0, &res);
	if ((s32)(u32)res.a0 != A133_NATIVE_SUCCESS)
		return a133_wakeupgen_native_status((u32)res.a0, 0);
	if (((u32)res.a1 & A133_NATIVE_REQUIRED_CAPS) !=
	    A133_NATIVE_REQUIRED_CAPS)
		return -EOPNOTSUPP;

	return 0;
}

static const struct a133_wakeupgen_profile a133_wakeupgen_stock_profile = {
	.name = "stock monitor",
	.set_wake = a133_wakeupgen_stock_set_wake,
};

static const struct a133_wakeupgen_profile a133_wakeupgen_native_profile = {
	.name = "native firmware",
	.set_wake = a133_wakeupgen_native_set_wake,
};

static int a133_wakeupgen_set_wake(struct irq_data *data, unsigned int on)
{
	const struct a133_wakeupgen_profile *profile;

	profile = irq_data_get_irq_chip_data(data);
	return profile->set_wake(data->hwirq, on);
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

bool sun50i_a133_wakeupgen_irq_ready(unsigned int irq)
{
	struct irq_data *data = irq_get_irq_data(irq);

	while (data) {
		if (irq_data_get_irq_chip(data) == &a133_wakeupgen_chip)
			return true;
		data = data->parent_data;
	}

	return false;
}
EXPORT_SYMBOL_GPL(sun50i_a133_wakeupgen_irq_ready);

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
						    &a133_wakeupgen_chip,
						    domain->host_data);
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
				      struct device_node *parent,
				      const struct a133_wakeupgen_profile *profile,
				      int (*probe)(void))
{
	struct irq_domain *parent_domain;
	int ret;

	if (!parent)
		return -EINVAL;
	parent_domain = irq_find_host(parent);
	if (!parent_domain)
		return -ENXIO;
	ret = probe();
	if (ret) {
		pr_err("A133 wakeupgen %s admission failed: %d\n",
		       profile->name, ret);
		return ret;
	}
	if (!irq_domain_create_hierarchy(parent_domain, 0, A133_NR_SPIS,
					 of_fwnode_handle(node),
				      &a133_wakeupgen_ops, (void *)profile))
		return -ENOMEM;
	return 0;
}

static int __init a133_wakeupgen_stock_init(struct device_node *node,
					    struct device_node *parent)
{
	return a133_wakeupgen_init(node, parent, &a133_wakeupgen_stock_profile,
				   a133_wakeupgen_stock_probe);
}

static int __init a133_wakeupgen_native_init(struct device_node *node,
					     struct device_node *parent)
{
	return a133_wakeupgen_init(node, parent, &a133_wakeupgen_native_profile,
				   a133_wakeupgen_native_probe);
}

IRQCHIP_DECLARE(a133_stock_wakeupgen, "allwinner,sun50i-a133-wakeupgen",
		a133_wakeupgen_stock_init);
IRQCHIP_DECLARE(a133_native_wakeupgen, "allwinner,sun50i-a133-native-wakeupgen",
		a133_wakeupgen_native_init);
