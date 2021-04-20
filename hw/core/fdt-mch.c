/*
 * FDT Parsed Machine
 *
 * Author: Alfred Wanga <awanga@gmail.com>
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
#include "qemu/units.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "qemu/log.h"
#include "qemu/option.h"
#include "hw/boards.h"
#include "hw/qdev-properties.h"
#include "hw/loader.h"
#include "exec/address-spaces.h"
#include "sysemu/sysemu.h"
#include "sysemu/device_tree.h"

#include "hw/block/flash.h"
#include "hw/core/cpu.h"
#include "hw/fdt-mch/fdt-mch.h"
#include "hw/i2c/i2c.h"
#include "hw/misc/unimp.h"
#include "migration/vmstate.h"

static const char FDT_NODE_CPU[] = "cpus";
static const char FDT_NODE_MEM[] = "memory";

/*
 * TODO: ideally we don't want this -- goal is to use all supported devices
 * unless fixup implementation is really painful or impractical
 */
static int mch_fdt_device_blocklist(const char *dev_id)
{
    unsigned i;
    const char *blocklist[] = {
        "pl050", /* need to split out to keyboard/mouse devices */
    };

    for (i = 0; i < ARRAY_SIZE(blocklist); i++) {
        if (strncmp(dev_id, blocklist[i], strlen(blocklist[i])) == 0) {
            return -1;
        }
    }
    return 0;
}

/*
 * TODO: if this function gets too large, move to a separate file
 *
 * attempt to detect and add needed device properties before realization
 * (properties that are needed for all devices of a given type should be
 * handled in the appropriate mch_fdt_add_* function)
 */
static void mch_fdt_device_fixup(DynamicState *s, const void *fdt, int node,
                             DeviceState *dev, const char *dev_id)
{
    /* const char *node_name = fdt_get_name(fdt, node, NULL); */

    /* pl080x DMA fixup */
    if (strncmp(dev_id, "pl08x", 4) == 0) {
        MemoryRegion *sysmem = get_system_memory();
        object_property_set_link(OBJECT(dev), "downstream",
                                 OBJECT(sysmem), &error_fatal);
        return;
    }
}

static int mch_fdt_add_properties(const void *fdt, int node,
                             DeviceState *dev)
{
    int offset;
    const char *prop_skiplist[] = {
        "#",
        "compatible",
        "reg",
        "ranges",
        "clock",
        "interrupt",
        "gpio",
    };

    fdt_for_each_property(fdt, node, offset) {
        unsigned i;
        int len;
        const void *val;
        const char *propname;

        val = fdt_getprop_by_offset(fdt, offset, &propname, &len);

        /* skip properties beginning with skip list substrings */
        for (i = 0; i < ARRAY_SIZE(prop_skiplist); i++) {
            if (strncmp(propname, prop_skiplist[i],
                        strlen(prop_skiplist[i])) == 0) {
                break;
            }
        }
        if (i < ARRAY_SIZE(prop_skiplist)) {
            continue;
        }

        if (len == 0) {
            /*qdev_prop_set_bit(dev, propname, true);*/
            pr_debug("found bool property %s in %s",
                     propname, fdt_get_name(fdt, node, NULL));
        } else if (len == 4) {
            uint32_t prop_val = fdt32_to_cpu(*(const fdt32_t *)val);
            /*qdev_prop_set_uint32(dev, propname, prop_val);*/
            pr_debug("found property value %s (%u) in %s",
                     propname, prop_val, fdt_get_name(fdt, node, NULL));
        } else if (len == 8) {
            /*uint64_t prop_val;

            fdt_getprop_long(fdt, node, propname, &prop_val);
            qdev_prop_set_uint64(dev, propname, prop_val);*/
            pr_debug("found property value %s in %s",
                     propname, fdt_get_name(fdt, node, NULL));
        } else {
            /* do length checking to verify it is a string */
            const char *str = (const char *)fdt_getprop(fdt, node,
                                                        propname, &len);

            /* arbitrary 1K string limit sanity check */
            if (len == strnlen(str, 1023) + 1) {
                qdev_prop_set_string(dev, propname, str);
                pr_debug("found property string %s in %s",
                         propname, fdt_get_name(fdt, node, NULL));
            } else {
                pr_debug("found property of unknown type %s in %s (%u != %lu)",
                         propname, fdt_get_name(fdt, node, NULL),
                         len, strnlen(str, 1023) + 1);
            }
        }
    }
    return 0;
}

static DeviceState *try_create_fdt_dummy_device(DynamicState *s,
                                                const void *fdt, int node)
{
    DeviceState *dev = NULL;
    const char *node_name = fdt_get_name(fdt, node, NULL);
    const char *compat;
    char dummy_name[64];
    unsigned i;
    uint64_t reg_addr, reg_size, size = 0;

    /* get most specific compatible name and strip manufacturer */
    if (fdt_getprop(fdt, node, FDT_PROP_COMPAT, NULL) == 0) {
        return NULL;
    }
    compat = str_strip(fdt_stringlist_get(fdt, node, FDT_PROP_COMPAT,
                                          0, NULL), ',');

    snprintf(dummy_name, 64, "%s.%s", compat, node_name);
    fdt_for_each_reg_prop(i, fdt, node, &reg_addr, &reg_size) {
        size += reg_size;
    }
    if (i == 0) {
        return NULL;
    }

    pr_debug("created dummy device: %s, size = %llu", dummy_name, size);

    dev = qdev_new(TYPE_UNIMPLEMENTED_DEVICE);
    qdev_prop_set_string(dev, "name", dummy_name);
    qdev_prop_set_uint64(dev, "size", size);

    mch_fdt_add_properties(fdt, node, dev);

    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_abort);

    fdt_for_each_reg_prop(i, fdt, node, &reg_addr, &reg_size) {
        sysbus_mmio_map_overlap(SYS_BUS_DEVICE(dev), i,
                                reg_addr, -1000);
    }

    return dev;
}

static DeviceState *try_create_fdt_device(DynamicState *s,
                                          const void *fdt, int node)
{
    DeviceState *dev = NULL;
    unsigned i, compat_num;

     /* try to instantiate "regular" device node using compatible string */
    compat_num = fdt_stringlist_count(fdt, node, FDT_PROP_COMPAT);
    for (i = 0; i < compat_num; i++) {
        const char *compat = fdt_stringlist_get(fdt, node,
                                FDT_PROP_COMPAT, i, NULL);

        /* strip manufacturer from string if exists */
        compat = str_strip(compat, ',');
        if (mch_fdt_device_blocklist(compat)) {
            continue;
        }

        /* try to create new device */
        dev = qdev_try_new(compat);
        if (dev) {
            mch_fdt_device_fixup(s, fdt, node, dev, compat);
            break;
        } else {
            /* try version of compatibility string with underscores */
            char *alt_compat = subst_compat_string(compat, '-', '_');

            dev = qdev_try_new(alt_compat);
            if (dev) {
                mch_fdt_device_fixup(s, fdt, node, dev, compat);
                g_free(alt_compat);
                break;
            }
            g_free(alt_compat);
        }
    }

    return dev;
}

static DeviceState *mch_fdt_add_pci_bus(DynamicState *s,
                DeviceState *parent_dev, const void *fdt, int node)
{
    DeviceState *dev = NULL;
    SysBusDevice *busdev;
    PCIBus *bus;
    const char *parent_name = fdt_get_name(fdt, node, NULL);
    int subnode;

    dev = try_create_fdt_device(s, fdt, node);
    if (dev) {
        unsigned child_addr_sz, parent_addr_sz, region, row, row_sz, size_sz;
        const fdt32_t *ranges;
        uint64_t reg_addr;

        busdev = SYS_BUS_DEVICE(dev);
        sysbus_realize_and_unref(busdev, &error_abort);

        /* add mmio regions */
        fdt_for_each_reg_prop(region, fdt, node, &reg_addr, NULL) {
            pr_debug("pci mmio: region %u: %llx", region, reg_addr);
            sysbus_mmio_map(busdev, region, reg_addr);
        }

        /* get cell sizes for range property */
        fdt_getprop_cell(fdt, node, "#size-cells", &size_sz);
        fdt_getprop_cell(fdt, node, "#address-cells", &child_addr_sz);
        fdt_getprop_cell(fdt, fdt_parent_offset(fdt, node),
                         "#address-cells", &parent_addr_sz);

        /* iterate over all ranges */
        row_sz = child_addr_sz + parent_addr_sz + size_sz;
        ranges = fdt_getprop(fdt, node, "ranges", NULL);
        fdt_for_each_cell_array(row, fdt, node, "ranges", row_sz) {
            uint64_t child_addr = 0, parent_addr = 0, size = 0;
            unsigned i;

            /*
             * TODO: these loops will lose the most significant bits
             *       if the cell count is greater than 2. those values
             *       are rarely used, and so are intentionally ignored
             *       (for now - may revisit in the future)
             */
            for (i = 0; i < child_addr_sz; i++) {
                child_addr = (child_addr << 32) | fdt32_to_cpu(*(ranges++));
            }
            for (i = 0; i < parent_addr_sz; i++) {
                parent_addr = (parent_addr << 32) | fdt32_to_cpu(*(ranges++));
            }
            for (i = 0; i < size_sz; i++) {
                size = (size << 32) | fdt32_to_cpu(*(ranges++));
            }

            pr_debug("pci mmio: region %u: %llx (sz=%llx)",
                     region, parent_addr, size);
            sysbus_mmio_map(busdev, region++, parent_addr);
        }

        bus = (PCIBus *)qdev_get_child_bus(dev, "pci");
        pr_debug("added pci bus %s", parent_name);
    } else {
        pr_debug("failed to instantiate pci bus %s", parent_name);
    }
    mch_fdt_dev_add_mapping(s, dev, node);

    /* iterate over pci devices */
    fdt_for_each_subnode(subnode, fdt, node) {
        const char *node_name = fdt_get_name(fdt, subnode, NULL);
        DeviceState *child_dev = NULL;
#if 0
        uint64_t reg_addr;

        /* if bus device setup failed, add null mapping for child nodes */
        if (dev) {
            /* skip devices with no reg value */
            if (fdt_simple_addr_size(fdt, subnode, 0, &reg_addr, NULL)) {
                pr_debug("i2c slave %s has no reg address! skipping...",
                         node_name);
                /* add null mapping to prevent recursive rescan */
                mch_fdt_dev_add_mapping(s, NULL, subnode);
                continue;
            }

            /* try to instantiate pci device using compatible string */
            child_dev = try_create_fdt_device(s, fdt, subnode);
            if (child_dev) {
                /*qdev_prop_set_uint8(child_dev, "address", (uint8_t)reg_addr);
                i2c_slave_realize_and_unref(I2C_SLAVE(child_dev),
                                            bus, &error_abort);*/
                pr_debug("added %s to pci bus %s", node_name, parent_name);
            }
        }
#endif
        mch_fdt_dev_add_mapping(s, child_dev, subnode);
    }

    return dev;
}

static DeviceState *mch_fdt_add_i2c_bus(DynamicState *s,
                DeviceState *parent_dev, const void *fdt, int node)
{
    DeviceState *dev = NULL;
    SysBusDevice *busdev;
    I2CBus *bus;
    const char *parent_name = fdt_get_name(fdt, node, NULL);
    int subnode;

    dev = try_create_fdt_device(s, fdt, node);
    if (dev) {
        busdev = SYS_BUS_DEVICE(dev);
        sysbus_realize_and_unref(busdev, &error_abort);
        bus = (I2CBus *)qdev_get_child_bus(dev, "i2c");
        pr_debug("added i2c bus %s", parent_name);
    } else {
        pr_debug("failed to instantiate i2c bus %s", parent_name);
    }
    mch_fdt_dev_add_mapping(s, dev, node);

    /* iterate over i2c devices */
    fdt_for_each_subnode(subnode, fdt, node) {
        const char *node_name = fdt_get_name(fdt, subnode, NULL);
        DeviceState *child_dev = NULL;
        uint64_t reg_addr;

        /* if bus device setup failed, add null mapping for child nodes */
        if (dev) {
            /* skip devices with no reg value */
            if (fdt_simple_addr_size(fdt, subnode, 0, &reg_addr, NULL)) {
                pr_debug("i2c slave %s has no reg address! skipping...",
                         node_name);
                /* add null mapping to prevent recursive rescan */
                mch_fdt_dev_add_mapping(s, NULL, subnode);
                continue;
            }

            /* try to instantiate i2c device using compatible string */
            child_dev = try_create_fdt_device(s, fdt, subnode);
            if (child_dev) {
                qdev_prop_set_uint8(child_dev, "address", (uint8_t)reg_addr);
                i2c_slave_realize_and_unref(I2C_SLAVE(child_dev),
                                            bus, &error_abort);
                pr_debug("added %s to i2c bus %s", node_name, parent_name);
            }
        }
        mch_fdt_dev_add_mapping(s, child_dev, subnode);
    }

    return dev;
}

static DeviceState *mch_fdt_add_spi_bus(DynamicState *s,
                DeviceState *parent_dev, const void *fdt, int node)
{
    DeviceState *dev = NULL;
    SysBusDevice *busdev;
    SSIBus *bus;
    const char *parent_name = fdt_get_name(fdt, node, NULL);
    int subnode;

    dev = try_create_fdt_device(s, fdt, node);
    if (dev) {
        unsigned num_cs = 1;
        int idx;

        busdev = SYS_BUS_DEVICE(dev);
        sysbus_realize_and_unref(busdev, &error_abort);

        /* property has many variants, but always has substring "num-cs" */
        idx = fdt32_to_cpu(*(const fdt32_t *)fdt_find_property_match(fdt,
                                                                     node,
                                                                     "num-cs",
                                                                     NULL));
        if (idx > 0) {
            num_cs = idx;
        } else {
            /* count children and round to nearest power of two */
            fdt_for_each_subnode(subnode, fdt, node) {
                /* increment num_cs to next power of two */
                if (++idx > num_cs) {
                    num_cs <<= 1;
                }
            }
        }

        bus = (SSIBus *)qdev_get_child_bus(dev, "spi");
        pr_debug("added spi bus %s (num_cs = %u)", parent_name, num_cs);
    } else {
        pr_debug("failed to instantiate spi bus %s", parent_name);
    }
    mch_fdt_dev_add_mapping(s, dev, node);

    /* iterate over spi devices */
    fdt_for_each_subnode(subnode, fdt, node) {
        const char *node_name = fdt_get_name(fdt, subnode, NULL);
        DeviceState *child_dev = NULL;
#if 0
        uint64_t reg_addr;

        /* if bus device setup failed, add null mapping for child nodes */
        if (dev) {
            /* skip devices with no reg value */
            if (fdt_simple_addr_size(fdt, subnode, 0, &reg_addr, NULL)) {
                pr_debug("spi slave %s has no reg address! skipping...",
                         node_name);
                /* add null mapping to prevent recursive rescan */
                mch_fdt_dev_add_mapping(s, NULL, subnode);
                continue;
            }

            /* try to instantiate spi device using compatible string */
            child_dev = try_create_fdt_device(s, fdt, subnode);
            if (child_dev) {
                qdev_prop_set_uint8(child_dev, "address", (uint8_t)reg_addr);
                i2c_slave_realize_and_unref(I2C_SLAVE(child_dev),
                                            bus, &error_abort);
                pr_debug("added %s to spi bus %s", node_name, parent_name);
            }
        }
#endif
        mch_fdt_dev_add_mapping(s, child_dev, subnode);
    }

    return dev;
}

static DeviceState *mch_fdt_add_generic_bus(DynamicState *s,
                DeviceState *parent_dev, const void *fdt, int node)
{
    DeviceState *dev = NULL;
    /* TODO: add generic bus code here */
    pr_debug("detected %s as generic bus", fdt_get_name(fdt, node, NULL));
    return dev;
}

static DeviceState *mch_fdt_add_simple_device(DynamicState *s,
                DeviceState *parent_dev, const void *fdt, int node)
{
    DeviceState *dev = NULL;
    SysBusDevice *busdev;
    uint64_t reg_addr, reg_size;
    unsigned i;

    /* try to instantiate "regular" device node using compatible string */
    dev = try_create_fdt_device(s, fdt, node);
    if (dev) {
        /* hook up clocks before device realization */
        mch_fdt_link_clocks(s, dev, fdt, node);

        busdev = SYS_BUS_DEVICE(dev);
        sysbus_realize_and_unref(busdev, &error_abort);
    } else {
        const char *node_name = fdt_get_name(fdt, node, NULL);

#if 0 /*def CONFIG_PFLASH_CFI01*/
        /* generic flash devices */
        if (fdt_compat_strstr(fdt, node, "cfi-flash") == 0 ||
            fdt_compat_strstr(fdt, node, "jedec-flash") == 0) {
            unsigned sect_size, bank_width = 1;
            DriveInfo *dinfo = drive_get(IF_PFLASH, 0,
                                         s->drive_count[IF_PFLASH]++);

            /* FIXME: handle three element regs */
            fdt_simple_addr_size(fdt, node, 0, &reg_addr, &reg_size);
            fdt_getprop_cell(fdt, node, "bank-width", &bank_width);

            /* guess sector size - should cover most (?) cases */
            if (reg_size < 8 * MiB) {
                sect_size = 64 * KiB;
            } else if (reg_size < 32 * MiB) {
                sect_size = 128 * KiB;
            } else {
                sect_size = 256 * KiB;
            }

            /* FIXME: how to differentiate between Intel & AMD CFI in fdt? */
            if (!pflash_cfi01_register(reg_addr, node_name, reg_size,
                          dinfo ? blk_by_legacy_dinfo(dinfo) : NULL,
                          sect_size, bank_width, 0x0, 0x0, 0x0, 0x0, 0)) {
                pr_debug("could not instantiate flash %s", node_name);
                return NULL;
            }

            return dev;
        }
#endif
        /* generic memory devices */
        if (fdt_compat_strstr(fdt, node, "mtd-ram") == 0 ||
            fdt_compat_strstr(fdt, node, "mmio-sram") == 0) {
            /* TODO: add support for generic memory devices */
        }

        /* TODO: try to catch other generic / irregular devices here */

        /* if all else fails, try to create a dummy device */
        if (dev == NULL) {
            dev = try_create_fdt_dummy_device(s, fdt, node);
            if (dev) {
                /* skip adding memory region for dummy device */
                return dev;
            }
        }

        /* no device could be instantiated */
        if (dev == NULL) {
            return NULL;
        }
    }

    /* add memory region when reg property exists */
    fdt_for_each_reg_prop(i, fdt, node, &reg_addr, &reg_size) {
        sysbus_mmio_map(busdev, i, reg_addr);
    }

    return dev;
}

/* TODO: should we move phase 0 & 1 of mch_fdt_build_interrupt_tree here? */
static DeviceState *mch_fdt_add_intr_controller(DynamicState *s,
                DeviceState *parent_dev, const void *fdt, int node)
{
    DeviceState *dev = NULL;
    const char *node_name = fdt_get_name(fdt, node, NULL);

    dev = mch_fdt_add_simple_device(s, parent_dev, fdt, node);
    if (dev) {
        FDTDevInfo *info;

        /* do not realize device until second pass is complete */

        mch_fdt_dev_add_mapping(s, dev, node);
        info = mch_fdt_dev_find_mapping(s, node);
        pr_debug("added %s as interrupt controller", node_name);
    } else {
        pr_debug("failed to instantiate interrupt controller %s", node_name);
    }
    return dev;
}

static DeviceState *mch_fdt_add_gpio_controller(DynamicState *s,
                DeviceState *parent_dev, const void *fdt, int node)
{
    DeviceState *dev = NULL;
    const char *node_name = fdt_get_name(fdt, node, NULL);

    dev = mch_fdt_add_simple_device(s, parent_dev, fdt, node);
    if (dev) {
        FDTDevInfo *info;

        /* do not realize device until second pass is complete */

        mch_fdt_dev_add_mapping(s, dev, node);
        info = mch_fdt_dev_find_mapping(s, node);
        pr_debug("added %s as gpio controller", node_name);
    } else {
        pr_debug("failed to instantiate gpio controller %s", node_name);
    }
    return dev;
}

static DeviceState *mch_fdt_add_device_node(DynamicState *s,
                DeviceState *parent_dev, const void *fdt, int node)
{
    DeviceState *dev = NULL;
    FDTDevInfo *info;
    bool has_subnodes = fdt_first_subnode(fdt, node) > 0;
    const char *node_name = fdt_get_name(fdt, node, NULL);
    const char *dev_type = (const char *)fdt_getprop(fdt, node,
                                                     "device_type", NULL);

    /* has this node been scanned already? */
    info = mch_fdt_dev_find_mapping(s, node);
    if (info != NULL) {
        /* TODO: ensure connectivity to parent device */
        return info->dev;
    }

    /* skip clock nodes, but keep count for second fixup pass */
    if (fdt_getprop(fdt, node, "#clock-cells", NULL) > 0) {
        s->num_clocks++;
        goto done;
    }

    /* check for explicit bus device types */
    if (dev_type != NULL) {

        /* check for pci bus */
        if (strncmp(dev_type, "pci", 3) == 0) {
            dev = mch_fdt_add_pci_bus(s, parent_dev, fdt, node);
            goto done;
        }

        /* check for soc (generic) busses */
        if (strncmp(dev_type, "soc", 3) == 0) {
            dev = mch_fdt_add_generic_bus(s, parent_dev, fdt, node);
            goto done;
        }
    }

    /* check if node is a bus */
    if (has_subnodes) {
        unsigned i, compat_num;
        bool has_ranges = (fdt_getprop(fdt, node, "ranges", NULL) != NULL);

        /* check for generic busses */
        if (has_ranges) {
            dev = mch_fdt_add_generic_bus(s, parent_dev, fdt, node);
            goto done;
        }

        /* check for i2c/spi bus */
        compat_num = fdt_stringlist_count(fdt, node, FDT_PROP_COMPAT);
        for (i = 0; i < compat_num; i++) {
            const char *compat = fdt_stringlist_get(fdt, node,
                                                FDT_PROP_COMPAT, i, NULL);

            /* FIXME: need more foolproof way to detect peripheral busses */
            if (strstr(compat, "i2c") != NULL &&
                strstr(node_name, "i2c") != NULL) {
                dev = mch_fdt_add_i2c_bus(s,
                                        parent_dev, fdt, node);
                goto done;
            }
            if (strstr(compat, "spi") != NULL &&
                strstr(node_name, "spi") != NULL) {
                dev = mch_fdt_add_spi_bus(s,
                                        parent_dev, fdt, node);
                goto done;
            }
        }
    }

    /* check for gpio/interrupt controller device (must check gpio first!) */
    if (fdt_getprop(fdt, node, "gpio-controller", NULL) > 0) {
        dev = mch_fdt_add_gpio_controller(s, parent_dev, fdt, node);
        goto done;
    }
    if (fdt_getprop(fdt, node, "interrupt-controller", NULL) > 0) {
        dev = mch_fdt_add_intr_controller(s, parent_dev, fdt, node);
        goto done;
    }

    /* try to instantiate "regular" device node using compatible string */
    if (!dev) {
        dev = mch_fdt_add_simple_device(s, parent_dev, fdt, node);

        if (!dev) {
            pr_debug("No device created for node %s", node_name);
            return NULL;
        }
    }

done:
    return dev;
}

static int mch_fdt_scan_node(DynamicState *s, DeviceState *parent,
                                 const void *fdt, int node)
{
    DeviceState *dev = NULL;
    int cnt, subnode;

    /* check for compatible property list */
    cnt = fdt_stringlist_count(fdt, node, FDT_PROP_COMPAT);
    if (cnt > 0) {
        /* add device to machine */
        dev = mch_fdt_add_device_node(s, parent, fdt, node);
    }

    fdt_for_each_subnode(subnode, fdt, node) {
        /* recursively search subnodes */
        mch_fdt_scan_node(s, dev, fdt, subnode);
    }

    return 0;
}

static int machine_load_device_tree(const char *dtb_filename, void **fdt_ptr)
{
    void *fdt = NULL;
    int fdt_size;

    fdt = load_device_tree(dtb_filename, &fdt_size);
    if (!fdt) {
        error_report("Error while loading device tree file '%s'",
                     dtb_filename);
        return -1;
    }

    g_assert(fdt_ptr);
    *fdt_ptr = fdt;
    return 0;
}

static void mch_fdt_parse_init(MachineState *mch)
{
    DynamicState *s = g_new(DynamicState, 1);
    MemoryRegion *ram = g_new(MemoryRegion, 1);
    Error *err = NULL;
    void *fdt = NULL;
    uint64_t reg_addr, reg_size;
    int len, offset, cpu_node;

    QSLIST_INIT(&s->dev_map);
    s->mch = mch;
    s->ram = ram;

    /* load the device tree with some checking */
    if (!mch->dtb) {
            error_report("FDT Parser machine requires use of -dtb parameter");
            exit(1);
    }
    if (machine_load_device_tree(mch->dtb, &fdt)) {
            error_report("Cannot locate device tree file!");
            exit(1);
    }

    /* add machine properties */
    object_property_add_uint64_ptr(OBJECT(mch), "cpu-freq",
                                   &s->default_cpu_rate,
                                   OBJ_PROP_FLAG_READWRITE);
    object_property_set_description(OBJECT(mch), "cpu-freq",
                                    "set clock frequency for CPU when the "
                                    "device tree does not specify");

    /* get model name from fdt */
    s->model_name =
        g_strdup((const char *)qemu_fdt_getprop(fdt, "/", "model", &len, &err));
    pr_debug("Scanning Device Tree for %s...\n", s->model_name);

    s->num_clocks = 0;
    s->num_cpus = 0;

    /* look for cpu node in device tree */
    cpu_node = fdt_subnode_offset(fdt, 0, FDT_NODE_CPU);
    if (cpu_node < 0) {
        unsigned idx;

        if (!mch->cpu_type) {
            error_report("Device tree has no CPU node. "
                         "Use -cpu to manually determine CPU");
            exit(1);
        }

        /* no CPU node found - try to use information passed to QEMU */
        s->num_cpus = MAX(mch->smp.cpus, 1);
        s->cpu = g_new0(CPUState *, s->num_cpus);
        for (idx = 0; idx < s->num_cpus; idx++) {
            s->cpu[idx] = cpu_create(mch->cpu_type);
            if (!s->cpu[idx]) {
                error_report("Could not manually init CPU %s",
                        mch->cpu_type);
                exit(1);
            }
        }
        pr_debug("No CPU node found. Creating %u %s CPU(s)",
                 s->num_cpus, mch->cpu_type);
    } else {
        unsigned idx = 0;

        /* count number of cpu nodes first and allocate pointers */
        fdt_for_each_subnode(offset, fdt, cpu_node) {
            s->num_cpus++;
        }
        s->cpu = g_new0(CPUState *, s->num_cpus);

        /* scan for cpus */
        fdt_for_each_subnode(offset, fdt, cpu_node) {
            const char *cpu_path, *cpu_type;
            uint64_t freq;

            cpu_path = fdt_get_name(fdt, offset, NULL);
            cpu_type = (const char *)qemu_fdt_getprop(fdt, cpu_path,
                                        FDT_PROP_COMPAT, &len, &error_fatal);

            pr_debug("Found CPU %s (%s)", cpu_path, cpu_type);

            freq = qemu_fdt_getprop_cell(fdt, cpu_path,
                        "timebase-frequency", &len, &err);
            if (len < 0) {

                freq = s->default_cpu_rate;
                if (freq == 0) {
                        error_report("No cpu frequency found in fdt. "
                                     "Provide value using cpu-freq property.");
                }
                pr_debug("No frequency found in fdt. Default to %lluMHz",
                         freq / 1000000);
            }

            /* create cpu using compatible string */
            s->cpu[idx] = cpu_create(cpu_type);
            if (!s->cpu[idx]) {
                /* try stripping manufacturer from cpu type */
                const char *cpu_type_model = str_strip(cpu_type, ',');

                if (cpu_type_model) {
                    s->cpu[idx] = cpu_create(cpu_type_model);
                }

                if (!cpu_type_model || !s->cpu[idx]) {
                    error_report("Unable to initialize CPU");
                    exit(1);
                }
            }
            idx++;
        }
    }

    /* hook up CPU interrupts */
    mch_fdt_intc_cpu_fixup(s, fdt);

    /* get system memory size */
    fdt_simple_addr_size(fdt, fdt_subnode_offset(fdt, 0, FDT_NODE_MEM),
                         0, &reg_addr, &reg_size);
    pr_debug("System Memory = %lluMB @ 0x%llx", reg_size / MiB, reg_addr);
    mch->ram_size = reg_size;
    if (!mch->ram_size) {
        error_report("No memory subnode in device tree found");
        exit(1);
    }

    /* register RAM */
    memory_region_init_ram(ram, NULL, "ram", mch->ram_size, &error_fatal);
    memory_region_add_subregion(get_system_memory(), reg_addr, ram);

    /* initialize clocktree */
    mch_fdt_init_clocks(s, fdt);

    /* iterate through all root nodes recursively */
    fdt_for_each_subnode(offset, fdt, 0) {
        const char *node_name = fdt_get_name(fdt, offset, NULL);

        if (strcmp(node_name, FDT_NODE_CPU) == 0 ||
            strcmp(node_name, FDT_NODE_MEM) == 0)
            continue;

        mch_fdt_scan_node(s, NULL, fdt, offset);
    }

    /* second pass - connectivity fixup of devices */
    mch_fdt_intc_build_tree(s, fdt);
    mch_fdt_gpio_connect(s, fdt);

    error_report("Completed init. Exiting...");
    exit(1);
}

static void mch_fdt_parse_machine_init(MachineClass *mc)
{
    mc->desc = "device tree parsed machine";
    mc->init = mch_fdt_parse_init;
    /*mc->max_cpus = DTB_PARSE_MAX_NUM_CPUS;*/
}

DEFINE_MACHINE("fdt_parse", mch_fdt_parse_machine_init)
