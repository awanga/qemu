/*
 * FDT Parsed Machine Utility Functions
 *
 * Author: Alfred Wanga <awanga@gmail.com>
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

const char FDT_PROP_COMPAT[] = "compatible";

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

/* get value from an array of cells, by row and index with bound checking */
int fdt_getprop_array_cell(const void *fdt, int nodeoffset, const char *prop,
                           const unsigned stride, unsigned row, unsigned idx,
                           uint32_t *val)
{
    int res;
    const fdt32_t *reg;

    reg = fdt_getprop(fdt, nodeoffset, prop, &res);
    if (res < 0) {
        return res;
    }

    /* bounds checking on row/index to size of property */
    if (idx >= stride || row >= (res / (stride * sizeof(*reg)))) {
        return -FDT_ERR_NOTFOUND;
    }

    /*
     * res is the number of bytes read and must be an even multiple of the
     * sum of address cells and size cells
     */
    if ((res % (stride * sizeof(*reg))) != 0) {
        return -FDT_ERR_BADVALUE;
    }

    if (val) {
        *val = fdt32_to_cpu(reg[idx + stride * row]);
    }
    return 0;
}

/* find offset of next node with a given property */
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

/* find a property with a name that contains a substring */
const void *fdt_find_property_match(const void *fdt, int node,
                                       const char *match, int *lenp)
{
    int prop_offset;

    fdt_for_each_property(fdt, node, prop_offset) {
        int length;
        const void *val;
        const char *name, *sub;

        val = fdt_getprop_by_offset(fdt, prop_offset, &name, &length);

        sub = strstr(name, match);
        if (sub != NULL) {
            if (lenp) {
                *lenp = length;
            }
            return val;
        }
    }
    return NULL;
}

/* look for an exact match in compatible string */
int fdt_compat_strstr(const void *fdt, int node, const char *match)
{
    unsigned i, compat_num;

     /* try to instantiate "regular" device node using compatible string */
    compat_num = fdt_stringlist_count(fdt, node, FDT_PROP_COMPAT);
    for (i = 0; i < compat_num; i++) {
        const char *sub = NULL;
        const char *compat = fdt_stringlist_get(fdt, node,
                                FDT_PROP_COMPAT, i, NULL);

        sub = strstr(compat, match);
        if (sub != NULL) {
            return 0;
        }
    }
    return -1;
}
