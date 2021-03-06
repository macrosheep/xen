/*
 * xen/arch/arm/irq.c
 *
 * ARM Interrupt support
 *
 * Ian Campbell <ian.campbell@citrix.com>
 * Copyright (c) 2011 Citrix Systems.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <xen/config.h>
#include <xen/lib.h>
#include <xen/spinlock.h>
#include <xen/irq.h>
#include <xen/init.h>
#include <xen/errno.h>
#include <xen/sched.h>

#include <asm/gic.h>

static void ack_none(struct irq_desc *irq)
{
    printk("unexpected IRQ trap at irq %02x\n", irq->irq);
}

static void end_none(struct irq_desc *irq) { }

hw_irq_controller no_irq_type = {
    .typename = "none",
    .startup = irq_startup_none,
    .shutdown = irq_shutdown_none,
    .enable = irq_enable_none,
    .disable = irq_disable_none,
    .ack = ack_none,
    .end = end_none
};

static irq_desc_t irq_desc[NR_IRQS];
static DEFINE_PER_CPU(irq_desc_t[NR_LOCAL_IRQS], local_irq_desc);

irq_desc_t *__irq_to_desc(int irq)
{
    if (irq < NR_LOCAL_IRQS) return &this_cpu(local_irq_desc)[irq];
    return &irq_desc[irq-NR_LOCAL_IRQS];
}

int __init arch_init_one_irq_desc(struct irq_desc *desc)
{
    return 0;
}


static int __init init_irq_data(void)
{
    int irq;

    for (irq = NR_LOCAL_IRQS; irq < NR_IRQS; irq++) {
        struct irq_desc *desc = irq_to_desc(irq);
        init_one_irq_desc(desc);
        desc->irq = irq;
        desc->action  = NULL;
    }

    return 0;
}

static int __cpuinit init_local_irq_data(void)
{
    int irq;

    for (irq = 0; irq < NR_LOCAL_IRQS; irq++) {
        struct irq_desc *desc = irq_to_desc(irq);
        init_one_irq_desc(desc);
        desc->irq = irq;
        desc->action  = NULL;
    }

    return 0;
}

void __init init_IRQ(void)
{
    BUG_ON(init_local_irq_data() < 0);
    BUG_ON(init_irq_data() < 0);
}

void __cpuinit init_secondary_IRQ(void)
{
    BUG_ON(init_local_irq_data() < 0);
}

static inline struct domain *irq_get_domain(struct irq_desc *desc)
{
    ASSERT(spin_is_locked(&desc->lock));

    if ( !(desc->status & IRQ_GUEST) )
        return dom_xen;

    ASSERT(desc->action != NULL);

    return desc->action->dev_id;
}

int request_dt_irq(const struct dt_irq *irq,
                   void (*handler)(int, void *, struct cpu_user_regs *),
                   const char *devname, void *dev_id)
{
    struct irqaction *action;
    int retval;

    /*
     * Sanity-check: shared interrupts must pass in a real dev-ID,
     * otherwise we'll have trouble later trying to figure out
     * which interrupt is which (messes up the interrupt freeing
     * logic etc).
     */
    if (irq->irq >= nr_irqs)
        return -EINVAL;
    if (!handler)
        return -EINVAL;

    action = xmalloc(struct irqaction);
    if (!action)
        return -ENOMEM;

    action->handler = handler;
    action->name = devname;
    action->dev_id = dev_id;
    action->free_on_release = 1;

    retval = setup_dt_irq(irq, action);
    if (retval)
        xfree(action);

    return retval;
}

/* Dispatch an interrupt */
void do_IRQ(struct cpu_user_regs *regs, unsigned int irq, int is_fiq)
{
    struct irq_desc *desc = irq_to_desc(irq);
    struct irqaction *action = desc->action;

    /* TODO: perfc_incr(irqs); */

    /* TODO: this_cpu(irq_count)++; */

    irq_enter();

    spin_lock(&desc->lock);
    desc->handler->ack(desc);

    if ( action == NULL )
    {
        printk("Unknown %s %#3.3x\n",
               is_fiq ? "FIQ" : "IRQ", irq);
        goto out;
    }

    if ( desc->status & IRQ_GUEST )
    {
        struct domain *d = irq_get_domain(desc);

        desc->handler->end(desc);

        desc->status |= IRQ_INPROGRESS;
        desc->arch.eoi_cpu = smp_processor_id();

        /* XXX: inject irq into all guest vcpus */
        vgic_vcpu_inject_irq(d->vcpu[0], irq, 0);
        goto out_no_end;
    }

    desc->status |= IRQ_PENDING;

    /*
     * Since we set PENDING, if another processor is handling a different
     * instance of this same irq, the other processor will take care of it.
     */
    if ( desc->status & (IRQ_DISABLED | IRQ_INPROGRESS) )
        goto out;

    desc->status |= IRQ_INPROGRESS;

    action = desc->action;
    while ( desc->status & IRQ_PENDING )
    {
        desc->status &= ~IRQ_PENDING;
        spin_unlock_irq(&desc->lock);
        action->handler(irq, action->dev_id, regs);
        spin_lock_irq(&desc->lock);
    }

    desc->status &= ~IRQ_INPROGRESS;

out:
    desc->handler->end(desc);
out_no_end:
    spin_unlock(&desc->lock);
    irq_exit();
}

void release_irq(unsigned int irq)
{
    struct irq_desc *desc;
    unsigned long flags;
   struct irqaction *action;

    desc = irq_to_desc(irq);

    spin_lock_irqsave(&desc->lock,flags);

    desc->handler->shutdown(desc);

    action = desc->action;
    desc->action  = NULL;
    desc->status &= ~IRQ_GUEST;

    spin_unlock_irqrestore(&desc->lock,flags);

    /* Wait to make sure it's not being used on another CPU */
    do { smp_mb(); } while ( desc->status & IRQ_INPROGRESS );

    if ( action && action->free_on_release )
        xfree(action);
}

static int __setup_irq(struct irq_desc *desc, struct irqaction *new)
{
    if ( desc->action != NULL )
        return -EBUSY;

    desc->action  = new;
    dsb(sy);

    return 0;
}

int setup_dt_irq(const struct dt_irq *irq, struct irqaction *new)
{
    int rc;
    unsigned long flags;
    struct irq_desc *desc;
    bool_t disabled;

    desc = irq_to_desc(irq->irq);

    spin_lock_irqsave(&desc->lock, flags);

    if ( desc->status & IRQ_GUEST )
    {
        struct domain *d = irq_get_domain(desc);

        spin_unlock_irqrestore(&desc->lock, flags);
        printk(XENLOG_ERR "ERROR: IRQ %u is already in use by the domain %u\n",
               irq->irq, d->domain_id);
        return -EBUSY;
    }

    disabled = (desc->action == NULL);

    rc = __setup_irq(desc, new);
    if ( rc )
        goto err;

    /* First time the IRQ is setup */
    if ( disabled )
    {
        bool_t level;

        level = dt_irq_is_level_triggered(irq);
        /* It's fine to use smp_processor_id() because:
         * For PPI: irq_desc is banked
         * For SPI: we don't care for now which CPU will receive the
         * interrupt
         * TODO: Handle case where SPI is setup on different CPU than
         * the targeted CPU and the priority.
         */
        gic_route_irq_to_xen(desc, level, cpumask_of(smp_processor_id()),
                             GIC_PRI_IRQ);
        desc->handler->startup(desc);
    }

err:
    spin_unlock_irqrestore(&desc->lock, flags);

    return rc;
}

int route_dt_irq_to_guest(struct domain *d, const struct dt_irq *irq,
                          const char * devname)
{
    struct irqaction *action;
    struct irq_desc *desc = irq_to_desc(irq->irq);
    unsigned long flags;
    int retval = 0;
    bool_t level;

    action = xmalloc(struct irqaction);
    if (!action)
        return -ENOMEM;

    action->dev_id = d;
    action->name = devname;
    action->free_on_release = 1;

    spin_lock_irqsave(&desc->lock, flags);

    /* If the IRQ is already used by someone
     *  - If it's the same domain -> Xen doesn't need to update the IRQ desc
     *  - Otherwise -> For now, don't allow the IRQ to be shared between
     *  Xen and domains.
     */
    if ( desc->action != NULL )
    {
        struct domain *ad = irq_get_domain(desc);

        if ( (desc->status & IRQ_GUEST) && d == ad )
            goto out;

        if ( desc->status & IRQ_GUEST )
            printk(XENLOG_ERR "ERROR: IRQ %u is already used by domain %u\n",
                   irq->irq, ad->domain_id);
        else
            printk(XENLOG_ERR "ERROR: IRQ %u is already used by Xen\n",
                   irq->irq);
        retval = -EBUSY;
        goto out;
    }

    retval = __setup_irq(desc, action);
    if ( retval )
        goto out;

    level = dt_irq_is_level_triggered(irq);
    gic_route_irq_to_guest(d, desc, level, cpumask_of(smp_processor_id()),
                           GIC_PRI_IRQ);
    spin_unlock_irqrestore(&desc->lock, flags);
    return 0;

out:
    spin_unlock_irqrestore(&desc->lock, flags);
    xfree(action);

    return retval;
}

/*
 * pirq event channels. We don't use these on ARM, instead we use the
 * features of the GIC to inject virtualised normal interrupts.
 */
struct pirq *alloc_pirq_struct(struct domain *d)
{
    return NULL;
}

/*
 * These are all unreachable given an alloc_pirq_struct
 * which returns NULL, all callers try to lookup struct pirq first
 * which will fail.
 */
int pirq_guest_bind(struct vcpu *v, struct pirq *pirq, int will_share)
{
    BUG();
}

void pirq_guest_unbind(struct domain *d, struct pirq *pirq)
{
    BUG();
}

void pirq_set_affinity(struct domain *d, int pirq, const cpumask_t *mask)
{
    BUG();
}

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
