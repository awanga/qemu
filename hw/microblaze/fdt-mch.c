/*
 *  Microblaze FDT Machine Helper Routines
 *
 *  Author: Alfred Wanga <awanga@gmail.com>
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
