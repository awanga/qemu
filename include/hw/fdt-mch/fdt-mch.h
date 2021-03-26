/*
 * FDT Parsed Machine Header File
 *
 * Author: Alfred Wanga <awanga@gmail.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 *
 */

#ifndef QEMU_MACH_FDT_PARSE_H
#define QEMU_MACH_FDT_PARSE_H

#include <libfdt.h>

#include "hw/clock.h"

/* defines */

#define DTB_PARSE_MAX_NUM_CPUS    16

/* debug routines */

#define pr_debug(x, ...) printf("DEBUG: " x "\n", ##__VA_ARGS__)

/* structures */

typedef struct clock_cb_parameters ClockParameters;

typedef struct {
    MachineState *mch;
    MemoryRegion *ram;

    CPUState *cpu[DTB_PARSE_MAX_NUM_CPUS];
    unsigned num_cpus;
    qemu_irq *cpu_irqs;
    unsigned num_cpu_intr;
    uint64_t default_cpu_rate;

    /* clock tree structures */
    Clock **clocks;
    ClockParameters *clock_params;
    unsigned num_clocks;
    /* used to map clocks to fdt nodes during machine setup */
    int *clock_node_map;

    const char *model_name;

    /* mapping used to build machine at runtime */
    struct device_fdt_mapping {
        struct device_fdt_mapping *next;
        int offset;
        struct device_fdt_info {
            DeviceState *dev;

            /* per device interrupt mapping */
            qemu_irq *irqs;
            unsigned num_irqs;
        } info;
    } *mapping;

} DynamicState;

void mch_fdt_init_clocks(DynamicState *s, const void *fdt);
void mch_fdt_link_clocks(DynamicState *s, DeviceState *dev,
                         const void *fdt, int node);

void mch_fdt_build_interrupt_tree(DynamicState *s, const void *fdt);
void mch_fdt_connect_gpio(DynamicState *s, const void *fdt);

/* internal device <-> fdt mapping routines */
int mch_fdt_dev_add_mapping(DynamicState *s, DeviceState *dev, int node_offset);
struct device_fdt_info *mch_fdt_dev_find_mapping(DynamicState *s, int node);

/* libfdt defines and extensions */
int fdt_simple_addr_size(const void *fdt, int nodeoffset, unsigned idx,
             uint64_t *addrp, uint64_t *sizep);

int fdt_getprop_array_cell(const void *fdt, int nodeoffset, const char *prop,
                           const unsigned stride, unsigned row, unsigned idx,
                           uint32_t *val);

int fdt_node_offset_by_prop(const void *fdt, int startoffset,
                            const char *propname);

/* iterate over each property group in a node */
#define fdt_for_each_cell_array(row, fdt, node, prop, stride) \
    for (row = 0; \
         fdt_getprop_array_cell(fdt, node, prop, stride, row, 0, NULL) == 0; \
         row++)

/* iterate over each reg property in a node */
#define fdt_for_each_reg_prop(idx, fdt, node, p_addr, p_size) \
    for (idx = 0; \
         fdt_simple_addr_size(fdt, node, idx, p_addr, p_size) == 0; \
         idx++)

/* iterate over all nodes with given property */
#define fdt_for_each_node_with_prop(offset, fdt, startoffset, propname) \
    for (offset = fdt_node_offset_by_prop(fdt, startoffset, propname); \
         offset >= 0; \
         offset = fdt_node_offset_by_prop(fdt, offset, propname))

/* helper to read array of u32 cells and/or check array index */
static inline int fdt_getprop_array_u32(const void *fdt, int node,
                                        const char *propname, unsigned idx,
                                        uint32_t *val)
{
    const fdt32_t *prop;
    int len;

    prop = fdt_getprop(fdt, node, propname, &len);
    if (!prop) {
        return -1;
    }
    if (idx >= (len / 4)) {
        return -1;
    }

    if (val) {
        *val = (uint32_t)fdt32_to_cpu(*(prop + idx));
    }
    return 0;
}

/* helper to read a value when we know it is in only one cell */
static inline int fdt_getprop_cell(const void *fdt, int node,
                                   const char *prop, uint32_t *val)
{
    const fdt32_t *data = fdt_getprop(fdt, node, prop, NULL);

    if (!data) {
        return -1;
    }
    if (val) {
        *val = fdt32_to_cpu(*data);
    }
    return 0;
}

/* helper to read a value when it may be in more than one cell */
static inline int fdt_getprop_long(const void *fdt, int node,
                                   const char *prop, uint64_t *val)
{
    uint64_t r = 0;
    int size;
    const fdt32_t *data = fdt_getprop(fdt, node, prop, &size);

    if (!data) {
        return -1;
    }

    if (val) {
        size /= 4;
        for (; size--; data++) {
            r = (r << 32) | fdt32_to_cpu(*data);
        }
        *val = r;
    }
    return 0;
}

/* helper function to strip manufacturer from compatibility string */
static inline const char *strip_compat_string(const char *s1)
{
    const char *s = strchr(s1, ',');

    if (s == NULL) {
        return s1;
    }
    return s + 1;
}

/* helper function to substitute character in string */
static inline char *subst_compat_string(const char *s1, const char src,
                                            const char dst)
{
    char *s = g_strdup(s1);
    char *p = s;

    while (*p != '\0') {
        if (*p == src) {
            *p = dst;
        }
        p++;
    }

    return s;
}

#endif /* QEMU_MACH_FDT_PARSE_H */
