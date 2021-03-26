/*
 *  FDT Machine GPIO Routines
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

#include "hw/core/cpu.h"
#include "hw/fdt-mch/fdt-mch.h"

void mch_fdt_connect_gpio(DynamicState *s, const void *fdt)
{
    int node;

    pr_debug("Connecting gpio...");

    fdt_for_each_node_with_prop(node, fdt, -1, "gpio-controller") {
        const char *node_name = fdt_get_name(fdt, node, NULL);
        FDTDevInfo *info = mch_fdt_dev_find_mapping(s, node);
        unsigned num_gpio_cell = 0;
        int offset;

        pr_debug("scanning for %s gpios", node_name);

        /* don't hook up controllers that were not instantiated */
        if (info) {
            if (!info->dev) {
                continue;
            }
        } else {
            continue;
        }

        /* get number of elements in gpios specifier */
        fdt_getprop_cell(fdt, node, "#gpio-cells", &num_gpio_cell);

        /* connect the devices to the right gpio controller */
        fdt_for_each_node_with_prop(offset, fdt, -1, "gpios") {
            const char *child_name = fdt_get_name(fdt, offset, NULL);
            FDTDevInfo *child_info;
            unsigned row;

            /* skip child node if not instantiated */
            child_info = mch_fdt_dev_find_mapping(s, node);
            if (!child_info) {
                continue;
            }

            /* iterate over all gpio entries */
            fdt_for_each_cell_array(row, fdt, offset, "gpios", num_gpio_cell) {
                uint32_t ctlr_phandle;
                uint32_t gpio;

                /* get phandle for gpio controller */
                fdt_getprop_array_cell(fdt, node, "gpios", num_gpio_cell,
                                       row, 0, &ctlr_phandle);

                if (fdt_node_offset_by_phandle(fdt, ctlr_phandle) != node) {
                    continue;
                }

                /* get gpio number */
                fdt_getprop_array_cell(fdt, node, "gpios", num_gpio_cell,
                                       row, 1, &gpio);

                pr_debug("* Connecting device %s to gpio %u", child_name, gpio);
                qdev_connect_gpio_out(info->dev, gpio,
                                      qdev_get_gpio_in(child_info->dev, row));
            }
        }
    }
    pr_debug("Finished connecting gpio");
}
