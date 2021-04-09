/*
 * FDT Parsed Machine Internal Header File
 *
 * Author: Alfred Wanga <awanga@gmail.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 *
 */

#ifndef QEMU_MACH_FDT_PARSE_INTERNAL_H
#define QEMU_MACH_FDT_PARSE_INTERNAL_H

qemu_irq *mch_fdt_get_cpu_irqs(CPUState *cpu, unsigned *num);

#endif /* QEMU_MACH_FDT_PARSE_INTERNAL_H */
