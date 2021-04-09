/*
 *  FDT Machine Interrupt Routines
 *
 *  Author: Alfred Wanga <awanga@gmail.com>
 *
 * Portions of code written by Benjamin Fair
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 *
 * *****************************************************************
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "qemu/log.h"
#include "qemu/option.h"
#include "hw/sysbus.h"
#include "hw/qdev-properties.h"
#include "exec/address-spaces.h"
#include "sysemu/sysemu.h"
#include "sysemu/device_tree.h"

#include "hw/fdt-mch/fdt-mch.h"

__attribute__((weak))
qemu_irq *mch_fdt_get_cpu_irqs(CPUState *cpu, unsigned *num)
{
    /* not all archs need special handling (e.g. riscv) */
    *num = 0;
    return NULL;
}

/* setup and hookup cpu interrupts */
void mch_fdt_intc_cpu_fixup(DynamicState *s, const void *fdt)
{
    FDTDevInfo *pic_info;
    uint32_t idx;

    /*
     * NOTE: Details of how the cpu irqs are handled are architecture
     *       specific. We use a arch-specific hook as a HAL to get
     *       processor interrupt signals in a target agnostic manner.
     */
    for (idx = 0; idx < s->num_cpus; idx++) {
        qemu_irq *irqs;
        unsigned i, offset, num;

        irqs = mch_fdt_get_cpu_irqs(s->cpu[idx], &num);
        s->num_cpu_irqs = num; /* this should not change */

        /* no fixup needed */
        if (num == 0) return;

        /* extend list of cpu irqs and append new ones to the list */
        offset = num * idx;
        if (!s->cpu_irqs) {
            s->cpu_irqs = g_new0(qemu_irq, num);
        } else {
            s->cpu_irqs = g_renew(qemu_irq, s->cpu_irqs, offset + num);
        }
        for (i = 0; i < num; i++) {
            s->cpu_irqs[offset + i] = irqs[i];
        }

        g_free(irqs);
    }

    /* connect the CPU irqs to the parent interrupt controller */
    if (fdt_getprop_cell(fdt, 0, "interrupt-parent",
                         &idx) < 0) {
        /*
         * found CPU irqs, but no global interrupt parent
         * node found, so we can't connect interrupts properly
         */
        error_report("Expected to find parent interrupt controller "
                     "for cpus in device tree, but none found. Device "
                     "tree may not be valid. Cannot build functional "
                     "interrupt tree.");
        exit(1);
    }
    pic_info = mch_fdt_dev_find_mapping(s,
                                        fdt_node_offset_by_phandle(fdt, idx));
    if (pic_info == NULL)
    {
        /* found parent irq controller, but it failed to instantiate */
        error_report("Unable to instantiate parent interrupt controller "
                     "for cpu(s). Cannot build functional interrupt tree.");
        exit(1);
    }

    /* cpus irqs are implicitly connected to "root" interrupt controller */
    for (idx = 0; idx < s->num_cpus * s->num_cpu_irqs; idx++) {
        sysbus_connect_irq(SYS_BUS_DEVICE(pic_info->dev), idx,
                           s->cpu_irqs[idx]);
    }

    /* source interrupts will be connected when interrupt tree is generated */
    return;
}

/* get interrupt parent node from property or inherited from parent */
static int mch_fdt_intc_get_parent_node(const void *fdt, int node)
{
    uint32_t parent_intc = 0;

    do {
        /* if no property found, go to the parent node */
        if (fdt_getprop_cell(fdt, node, "interrupt-parent",
                             &parent_intc) < 0) {
            node = fdt_parent_offset(fdt, node);
            if (node < 0) {
                /* no interrupt-parent property found */
                return -FDT_ERR_NOTFOUND;
            }
        }
    } while (parent_intc == 0);

    return fdt_node_offset_by_phandle(fdt, parent_intc);
}

void mch_fdt_intc_build_tree(DynamicState *s, const void *fdt)
{
    int node;

    pr_debug("Building interrupt tree...");

    fdt_for_each_node_with_prop(node, fdt, -1, "interrupt-controller") {
        const char *node_name = fdt_get_name(fdt, node, NULL);
        FDTDevInfo *info = mch_fdt_dev_find_mapping(s, node);
        unsigned cnt = 0, num_intr_cell = 0;
        int offset;

        /* don't hook up controllers that were not instantiated */
        if (info) {
            if (!info->dev) {
                continue;
            }
        } else {
            continue;
        }

        /* pr_debug("Scanning for children of %s...", node_name); */

        /* get number of elements in interrupts specifier */
        fdt_getprop_cell(fdt, node, "#interrupt-cells", &num_intr_cell);

        /* phase 1: find the max number of interrupts for the controller */
        fdt_for_each_node_with_prop(offset, fdt, -1, "interrupts") {
            int intc = mch_fdt_intc_get_parent_node(fdt, offset);
            uint32_t irq;

            if (intc != node) {
                continue;
            }
            fdt_getprop_array_u32(fdt, node, "interrupts",
                               (num_intr_cell == 3) ? 1 : 0, &irq);
            info->num_irqs = MAX(info->num_irqs, irq);
            cnt++;
        }

        /*
         * FIXME: common interrupt controllers like arm-gic have more than
         *        one type of IRQ. we should support this ASAP.
         */

        /* there should be the same or more irqs available than assigned */
        g_assert(cnt <= info->num_irqs);
        pr_debug("* Found %u irqs for intc %s", info->num_irqs, node_name);

        /* phase 2: allocate and initialize irqs */
        info->irqs = g_new0(qemu_irq, info->num_irqs);
        for (cnt = 0; cnt < info->num_irqs; cnt++) {
            info->irqs[cnt] = qdev_get_gpio_in(info->dev, cnt);
        }

        /* phase 3: connect the devices to the right irq */
        fdt_for_each_node_with_prop(offset, fdt, -1, "interrupts") {
            const char *child_name = fdt_get_name(fdt, offset, NULL);
            int intc = mch_fdt_intc_get_parent_node(fdt, offset);
            FDTDevInfo *child_info;
            uint32_t irq;

            if (intc != node) {
                continue;
            }
            child_info = mch_fdt_dev_find_mapping(s, node);
            if (!child_info) {
                continue;
            }

            fdt_getprop_array_u32(fdt, node, "interrupts",
                               (num_intr_cell == 3) ? 1 : 0, &irq);

            pr_debug("* Connecting device %s to irq %u", child_name, irq);
            sysbus_connect_irq(SYS_BUS_DEVICE(child_info->dev),
                               0, info->irqs[irq]);
        }
    }
    pr_debug("Finished building interrupt tree");
}
