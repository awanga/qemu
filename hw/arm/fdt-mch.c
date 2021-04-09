/*
 * ARM FDT Machine Helper Routines
 *
 * Author: Alfred Wanga <awanga@gmail.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 *
 */

#include "qemu/osdep.h"
#include "hw/sysbus.h"
#include "hw/qdev-properties.h"
#include "sysemu/sysemu.h"

#include "hw/fdt-mch/fdt-mch-internal.h"
#include "cpu.h"

#define CPU_MACH_NUM_IRQS   2

qemu_irq *mch_fdt_get_cpu_irqs(CPUState *cpu, unsigned *num)
{
    qemu_irq *irqs = g_new0(qemu_irq, CPU_MACH_NUM_IRQS);
    unsigned n;

    /*
     * There are four ARM cpu irqs but we only return the
     * non-virtual ones for constructing the device tree
     */
    *num = CPU_MACH_NUM_IRQS;
    for (n = 0; n < CPU_MACH_NUM_IRQS; n++) {
        irqs[n] = qdev_get_gpio_in(DEVICE(cpu), n);
    }

    return irqs;
}
