/*
 *  FDT Parsed Machine Utility Functions
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
#include "sysemu/sysemu.h"
#include "sysemu/device_tree.h"

#include "hw/fdt-mch/fdt-mch.h"

static int _fdt_read_cells(const fdt32_t *cells, unsigned int n,
    uint64_t *value)
{
    int i;

    if (n > 2) {
        return -FDT_ERR_BADNCELLS;
    }

    *value = 0;
    for (i = 0; i < n; i++) {
        *value <<= (sizeof(*cells) * 8);
        *value |= fdt32_to_cpu(cells[i]);
    }

    return 0;
}

/**
 *
 * fdt_simple_addr_size - read address and/or size from the reg property of a
 *                        device node.
 * @fdt: pointer to the device tree blob
 * @nodeoffset: offset of the node to find the address and/or size from
 * @idx: which address/size pair to read
 * @addrp: pointer to where address will be stored (will be overwritten) or NULL
 * @sizep: pointer to where size will be stored (will be overwritten) or NULL
 *
 * When the node has a valid reg property, returns the address and/or size
 * values stored there. It does not perform any type of translation based on
 * the parent bus(es).
 *
 * NOTE: This function is expensive, as it must scan the device tree
 * structure from the start to nodeoffset, *twice*, with fdt_parent_offset.
 *
 * returns:
 *  0, on success
 *  -FDT_ERR_BADVALUE, if there is an unexpected number of entries in the
 *      reg property
 *  -FDT_ERR_NOTFOUND, if the node does not have a reg property
 *  -FDT_ERR_BADNCELLS, if the number of address or size cells is invalid
 *      or greater than 2 (which is the maximum currently supported)
 *  -FDT_ERR_BADMAGIC,
 *  -FDT_ERR_BADSTATE,
 *  -FDT_ERR_BADSTRUCTURE,
 *  -FDT_ERR_BADVERSION,
 *  -FDT_ERR_TRUNCATED, standard meanings
 */
int fdt_simple_addr_size(const void *fdt, int nodeoffset, unsigned idx,
         uint64_t *addrp, uint64_t *sizep)
{
    int parent;
    int ac, sc, reg_stride;
    int res;
    const fdt32_t *reg;

    reg = fdt_getprop(fdt, nodeoffset, "reg", &res);
    if (res < 0) {
        return res;
    }

    parent = fdt_parent_offset(fdt, nodeoffset);
    if (parent == -FDT_ERR_NOTFOUND) {
        /*
         * a node without a parent does
         * not have _any_ number of cells
         */
        return -FDT_ERR_BADNCELLS;
    }
    if (parent < 0) {
        return parent;
    }

    ac = fdt_address_cells(fdt, parent);
    if (ac < 0) {
        return ac;
    }

    sc = fdt_size_cells(fdt, parent);
    if (sc < 0) {
        return sc;
    }

    reg_stride = ac + sc;

    /* bounds checking on index to size of property */
    if (idx >= (res / (reg_stride * sizeof(*reg)))) {
        return -FDT_ERR_NOTFOUND;
    }

    /*
     * res is the number of bytes read and must be an even multiple of the
     * sum of address cells and size cells
     */
    if ((res % (reg_stride * sizeof(*reg))) != 0) {
        return -FDT_ERR_BADVALUE;
    }

    if (addrp) {
        res = _fdt_read_cells(&reg[reg_stride * idx], ac, addrp);
        if (res < 0) {
            return res;
        }
    }
    if (sizep) {
        res = _fdt_read_cells(&reg[ac + reg_stride * idx], sc, sizep);
        if (res < 0) {
            return res;
        }
    }

    return 0;
}

int fdt_node_offset_by_prop(const void *fdt, int startoffset,
                            const char *propname)
{
    int offset;
    const void *val;

    for (offset = fdt_next_node(fdt, startoffset, NULL);
         offset >= 0;
         offset = fdt_next_node(fdt, offset, NULL)) {
        val = fdt_getprop(fdt, offset, propname, NULL);
        if (val) {
            return offset;
        }
    }

    return offset; /* error from fdt_next_node() */
}

int mch_fdt_dev_add_mapping(DynamicState *s, DeviceState *dev,
                                      int node_offset)
{
    struct device_fdt_mapping *mapping =
                g_new0(struct device_fdt_mapping, 1);

    mapping->dev = dev;
    mapping->offset = node_offset;
    mapping->next = s->mapping;
    s->mapping = mapping;
    return 0;
}

int mch_fdt_dev_find_mapping(DynamicState *s, int node, DeviceState **dev)
{
    struct device_fdt_mapping *mapping = s->mapping;

    while (mapping != NULL) {
        if (mapping->offset == node) {
            if (dev) {
                *dev = mapping->dev;
            }
            return 0;
        }
        mapping = mapping->next;
    }
    return -1;
}

