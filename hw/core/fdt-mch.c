/*
 *  FDT Parsed Machine
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

#include "hw/core/cpu.h"
#include "hw/i2c/i2c.h"
#include "hw/fdt-mch/fdt-mch.h"
#include "migration/vmstate.h"

static const char FDT_NODE_CPU[] = "cpus";
static const char FDT_NODE_MEM[] = "memory";

static const char FDT_PROP_COMPAT[] = "compatible";

/*
 * TODO: ideally we don't want this -- goal is to use all supported devices
 * unless fixup implementation is really painful or impractical
 */
static int fdt_device_blocklist(const char *dev_id)
{
    unsigned i;
    const char *blocklist[] = {
        "pl050", /* need to split out to keyboard/mouse devices */
    };

    for (i = 0; i < sizeof(blocklist) / sizeof(blocklist[0]); i++) {
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
static void fdt_device_fixup(DynamicState *s, const void *fdt, int node,
                             DeviceState *dev, const char *dev_id)
{
    const char *node_name = fdt_get_name(fdt, node, NULL);

    /* pl080x DMA fixup */
    if (strncmp(dev_id, "pl08x", 4) == 0) {
        MemoryRegion *sysmem = get_system_memory();
        object_property_set_link(OBJECT(dev), "downstream",
                                 OBJECT(sysmem), &error_fatal);
        return;
    }
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
        compat = strip_compat_string(compat);
        if (fdt_device_blocklist(compat)) {
            continue;
        }

        /* try to create new device */
        dev = qdev_try_new(compat);
        if (dev) {
            fdt_device_fixup(s, fdt, node, dev, compat);
            break;
        } else {
            /* try version of compatibility string with underscores */
            char *alt_compat = subst_compat_string(compat, '-', '_');

            dev = qdev_try_new(alt_compat);
            if (dev) {
                fdt_device_fixup(s, fdt, node, dev, compat);
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
    /* TODO: add pci bus code here */
    pr_debug("detected %s as pci bus", fdt_get_name(fdt, node, NULL));
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
        /*
         * allow i2c device instantiation to proceed if bus device
         * instantiation fails
         */
        pr_debug("failed to instantiate i2c bus %s", parent_name);
    }
    mch_fdt_dev_add_mapping(s, dev, node);

    /* iterate over i2c devices */
    fdt_for_each_subnode(subnode, fdt, node) {
        const char *node_name = fdt_get_name(fdt, subnode, NULL);
        DeviceState *child_dev = NULL;
        uint64_t reg_addr;

        /* if bus device setup failed, skip children, but still map */
        if (dev) {
            /* skip devices with no reg value */
            if (fdt_simple_addr_size(fdt, subnode, 0, &reg_addr, NULL)) {
                pr_debug("i2c slave %s has no reg address! skipping...",
                         node_name);
                /* add to mapping to prevent recursive rescan */
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
    /* TODO: add spi bus code here */
    pr_debug("detected %s as spi bus", fdt_get_name(fdt, node, NULL));
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
        /* TODO: should we try to catch generic parts here i.e. cfi-flash? */
        return NULL;
    }

    /* add memory region when reg property exists */
    fdt_for_each_reg_prop(i, fdt, node, &reg_addr, &reg_size) {
        sysbus_mmio_map(busdev, i, reg_addr);
    }

    return dev;
}

/* TODO: should we move phase 1 of mch_fdt_build_interrupt_tree here? */
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
                const char *cpu_type_model = strip_compat_string(cpu_type);

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
    pr_debug("System Memory = %lluMB @ 0x%llx",
                    reg_size / 1024 / 1024, reg_addr);
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
