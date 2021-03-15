/*
 * DTB-Parsed Machine Header File
 *
 * Author: Alfred Wanga <awanga@gmail.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 *
 */

#ifndef QEMU_MACH_DTB_PARSE_H
#define QEMU_MACH_DTB_PARSE_H

#include <libfdt.h>

/* defines */

#define DTB_PARSE_MAX_NUM_CPUS    16

/* debug routines */

#define pr_debug(x, ...) printf("DEBUG: " x "\n", ##__VA_ARGS__)

/* libfdt defines and extensions */

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
int fdt_simple_addr_size(const void *fdt, int nodeoffset, int idx,
             uint64_t *addrp, uint64_t *sizep);

/* iterate over each reg property in a node */
#define fdt_for_each_reg_prop(idx, fdt, node, p_addr, p_size) \
    for (idx = 0; \
         fdt_simple_addr_size(fdt, node, idx, p_addr, p_size) == 0; \
         idx++)

/* helper function to strip manufacturer from compatibility string */
static inline const char *str_fdt_compat_strip(const char *s1)
{
    const char *s = strchr(s1, ',');

    if (s == NULL) {
        return s1;
    }
    return s + 1;
}

#endif /* QEMU_MACH_DTB_PARSE_H */
