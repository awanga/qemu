/*
 * Microblaze FDT Machine Helper Routines
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

#define CPU_MACH_NUM_IRQS   1

qemu_irq *mch_fdt_get_cpu_irqs(CPUState *cpu, unsigned *num)
{
    qemu_irq *irqs = g_new0(qemu_irq, CPU_MACH_NUM_IRQS);
    unsigned n;

    /* There are four ARM cpu irqs: IRQ, FIQ, VIRQ, VFIQ */
    *num = CPU_MACH_NUM_IRQS;
    irqs[0] = qdev_get_gpio_in(DEVICE(cpu), MB_CPU_IRQ);

    return irqs;
}
