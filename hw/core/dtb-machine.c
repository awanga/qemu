/*
 * DTB-Parsed Machine
 *
 * Author: Alfred Wanga <awanga@gmail.com>
 *
 * Portions written by Benjamin Fair
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 *
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
#include "hw/dtb-parse/dtb-parse.h"
#include "migration/vmstate.h"

static const char DTB_CPU_NODE[] = "cpus";
static const char DTB_MEM_NODE[] = "memory";

static const char DTB_PROP_COMPAT[] = "compatible";

typedef struct {
    MachineState *mch;
    MemoryRegion *ram;
    CPUState *cpu[DTB_PARSE_MAX_NUM_CPUS];

    const char *model_name;
    unsigned ncpus;

    /* TODO: this may really want to be a hash, not a linked list */
    struct device_fdt_mapping {
        struct device_fdt_mapping *next;
        DeviceState *dev;
        enum fdt_node_type {
            NODE_TYPE_UNKNOWN    = 0,
            NODE_TYPE_BUS,
            NODE_TYPE_DEVICE,
            NODE_TYPE_CLOCK,
        } dev_type;
        int offset;
    } *mapping;

} DynamicState;

/* from libfdt code written by Benjamin Fair */
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

/* from libfdt code written by Benjamin Fair */
int fdt_simple_addr_size(const void *fdt, int nodeoffset, int idx,
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

static int add_dev_fdt_mapping(DynamicState *s, DeviceState *dev,
                                      int node_offset)
{
    struct device_fdt_mapping *node =
                g_malloc0(sizeof(struct device_fdt_mapping));

    node->dev = dev;
    node->offset = node_offset;
    node->next = s->mapping;
    s->mapping = node;
    return 0;
}

static struct DeviceState *find_dev_fdt_mapping(DynamicState *s, int node)
{
    struct device_fdt_mapping *head = s->mapping;

    while (head != NULL) {
        if (head->offset == node) {
            return head->dev;
        }
        head = head->next;
    }
    return NULL;
}

static DeviceState *machine_dtb_add_clocksource(DynamicState *s,
                DeviceState *parent_dev, const void *fdt, int node)
{
    DeviceState *dev = NULL;
    /* TODO: add clocksource code here */
    pr_debug("adding %s as clocksource", fdt_get_name(fdt, node, NULL));
    return dev;
}

static DeviceState *machine_dtb_add_pci_bus(DynamicState *s,
                DeviceState *parent_dev, const void *fdt, int node)
{
    DeviceState *dev = NULL;
    /* TODO: add pci bus code here */
    pr_debug("adding %s as pci bus", fdt_get_name(fdt, node, NULL));
    return dev;
}

static DeviceState *machine_dtb_add_i2c_bus(DynamicState *s,
                DeviceState *parent_dev, const void *fdt, int node)
{
    DeviceState *dev = NULL;
    SysBusDevice *busdev;
    I2CBus *bus;
    unsigned i, compat_num;
    int subnode;

    compat_num = fdt_stringlist_count(fdt, node, DTB_PROP_COMPAT);
    for (i = 0; i < compat_num; i++) {
        const char *compat = fdt_stringlist_get(fdt, node,
                            DTB_PROP_COMPAT, i, NULL);

        /* strip manufacturer and try to create new device */
        dev = qdev_try_new(str_fdt_compat_strip(compat));
        if (dev) {
            busdev = SYS_BUS_DEVICE(dev);
            pr_debug("adding i2c bus %s", str_fdt_compat_strip(compat));
            sysbus_realize_and_unref(busdev, &error_fatal);
            break;
        }
    }

    if (!dev) {
        const char *node_name = fdt_get_name(fdt, node, NULL);
        /*
         * allow i2c device instantiation to proceed if bus device
         * instantiation fails
         */
        pr_debug("failed to instantiate i2c bus %s", node_name);
        bus = i2c_init_bus(NULL, node_name);
    } else {
        bus = (I2CBus *)qdev_get_child_bus(dev, "i2c");
    }
    add_dev_fdt_mapping(s, dev, node);

    /* iterate over i2c devices */
    fdt_for_each_subnode(subnode, fdt, node) {
        const char *node_name = fdt_get_name(fdt, subnode, NULL);
        DeviceState *child_dev = NULL;
        uint64_t reg_addr;

        /* skip devices with no reg value */
        if (fdt_simple_addr_size(fdt, subnode, 0, &reg_addr, NULL)) {
            pr_debug("i2c slave %s has no reg address! skipping...", node_name);
            /* add to mapping to prevent recursive rescan */
            add_dev_fdt_mapping(s, NULL, subnode);
            continue;
        }

        /* try to instantiate i2c device using compatible string */
        compat_num = fdt_stringlist_count(fdt, node, DTB_PROP_COMPAT);
        for (i = 0; i < compat_num; i++) {
            const char *compat = fdt_stringlist_get(fdt, node,
                            DTB_PROP_COMPAT, i, NULL);

            /* strip manufacturer and try to create new i2c device */
            child_dev = qdev_try_new(str_fdt_compat_strip(compat));
            if (child_dev) {
                pr_debug("adding i2c slave %s", str_fdt_compat_strip(compat));
                qdev_prop_set_uint8(child_dev, "address",
                                            (uint8_t)reg_addr);
                i2c_slave_realize_and_unref(I2C_SLAVE(child_dev),
                                            bus, &error_abort);
                break;
            }
        }
        add_dev_fdt_mapping(s, child_dev, subnode);
    }

    return dev;
}

static DeviceState *machine_dtb_add_spi_bus(DynamicState *s,
                DeviceState *parent_dev, const void *fdt, int node)
{
    DeviceState *dev = NULL;
    /* TODO: add spi bus code here */
    pr_debug("adding %s as spi bus", fdt_get_name(fdt, node, NULL));
    return dev;
}

static DeviceState *machine_dtb_add_generic_bus(DynamicState *s,
                DeviceState *parent_dev, const void *fdt, int node)
{
    DeviceState *dev = NULL;
    /* TODO: add generic bus code here */
    pr_debug("adding %s as generic bus", fdt_get_name(fdt, node, NULL));
    return dev;
}

static DeviceState *machine_dtb_add_intr_controller(DynamicState *s,
                DeviceState *parent_dev, const void *fdt, int node)
{
    DeviceState *dev = NULL;
    /* TODO: add interrupt controller code here */
    pr_debug("adding %s as interrupt controller",
                    fdt_get_name(fdt, node, NULL));
    return dev;
}

static DeviceState *machine_dtb_add_gpio_controller(DynamicState *s,
                DeviceState *parent_dev, const void *fdt, int node)
{
    DeviceState *dev = NULL;
    /* TODO: add gpio controller code here */
    pr_debug("adding %s as gpio controller", fdt_get_name(fdt, node, NULL));
    return dev;
}

static DeviceState *machine_dtb_add_simple_device(DynamicState *s,
                DeviceState *parent_dev, const void *fdt, int node)
{
    DeviceState *dev = NULL;
    SysBusDevice *busdev;
    uint64_t reg_addr, reg_size;
    unsigned i, compat_num;

    /* try to instantiate "regular" device node using compatible string */
    compat_num = fdt_stringlist_count(fdt, node, DTB_PROP_COMPAT);
    for (i = 0; i < compat_num; i++) {
        const char *compat = fdt_stringlist_get(fdt, node,
                                DTB_PROP_COMPAT, i, NULL);

        /* strip manufacturer and try to create new device */
        dev = qdev_try_new(str_fdt_compat_strip(compat));
        if (dev) {
            busdev = SYS_BUS_DEVICE(dev);
            pr_debug("creating device %s", str_fdt_compat_strip(compat));
            sysbus_realize_and_unref(busdev, &error_abort);
            break;
        }
    }

    if (!dev) {
        pr_debug("failed to add simple device %s",
                            fdt_get_name(fdt, node, NULL));
        return NULL;
    }

    /* add memory region when reg property exists */
    fdt_for_each_reg_prop(i, fdt, node, &reg_addr, &reg_size) {
        pr_debug("adding mmio region %llx", reg_addr);
        sysbus_mmio_map(busdev, i, reg_addr);
    }

    return dev;
}

static DeviceState *machine_dtb_add_dummy_device(DynamicState *s,
                DeviceState *parent_dev, const void *fdt, int node)
{
    DeviceState *dev = NULL;
    /* TODO: add dummy device code here */
    return dev;
}

static DeviceState *machine_dtb_add_device_node(DynamicState *s,
                DeviceState *parent_dev, const void *fdt, int node)
{
    DeviceState *dev = NULL;
    bool has_subnodes = fdt_first_subnode(fdt, node) > 0;
    const char *node_name = fdt_get_name(fdt, node, NULL);
    const char *dev_type = (const char *)fdt_getprop(fdt, node,
                                                     "device_type", NULL);

    /* check for explicit bus device types */
    if (dev_type != NULL) {

        /* check for pci bus */
        if (strncmp(dev_type, "pci", 3) == 0) {
            dev = machine_dtb_add_pci_bus(s, parent_dev, fdt, node);
            goto done;
        }

        /* check for soc (generic) busses */
        if (strncmp(dev_type, "soc", 3) == 0) {
            dev = machine_dtb_add_generic_bus(s, parent_dev, fdt, node);
            goto done;
        }
    }

    /* check if node is a bus */
    if (has_subnodes) {
        unsigned i, compat_num;
        bool has_ranges = (fdt_getprop(fdt, node, "ranges", NULL) != NULL);

        /* check for generic busses */
        if (has_ranges) {
            dev = machine_dtb_add_generic_bus(s, parent_dev, fdt, node);
            goto done;
        }

        /* check for i2c/spi bus */
        compat_num = fdt_stringlist_count(fdt, node, DTB_PROP_COMPAT);
        for (i = 0; i < compat_num; i++) {
            const char *compat = fdt_stringlist_get(fdt, node,
                                                DTB_PROP_COMPAT, i, NULL);

            /* FIXME: need more foolproof way to detect peripheral busses */
            if (strstr(compat, "i2c") != NULL &&
                strstr(node_name, "i2c") != NULL) {
                dev = machine_dtb_add_i2c_bus(s,
                                        parent_dev, fdt, node);
                goto done;
            }
            if (strstr(compat, "spi") != NULL &&
                strstr(node_name, "spi") != NULL) {
                dev = machine_dtb_add_spi_bus(s,
                                        parent_dev, fdt, node);
                goto done;
            }
        }
    }

    /* check for gpio/interrupt controller device (must check gpio first!) */
    if (fdt_getprop(fdt, node, "gpio-controller", NULL) > 0) {
        dev = machine_dtb_add_gpio_controller(s, parent_dev, fdt, node);
        goto done;
    }
    if (fdt_getprop(fdt, node, "interrupt-controller", NULL) > 0) {
        dev = machine_dtb_add_intr_controller(s, parent_dev, fdt, node);
        goto done;
    }

    /* special case: check for "clock-cells" property */
    if (fdt_getprop(fdt, node, "#clock-cells", NULL) > 0) {
        dev = machine_dtb_add_clocksource(s, parent_dev, fdt, node);
        goto done;
    }

    /* try to instantiate "regular" device node using compatible string */
    if (!dev) {
        dev = machine_dtb_add_simple_device(s, parent_dev, fdt, node);
    }

    if (!dev) {
        /* fallback: try to create dummy device */
        dev = machine_dtb_add_dummy_device(s, parent_dev, fdt, node);
        if (!dev) {
            pr_debug("No device created for node %s", node_name);
            return NULL;
        }
    }

done:
    return dev;
}

static int machine_dtb_scan_node(DynamicState *s, DeviceState *parent,
                                 const void *fdt, int node)
{
    DeviceState *dev = NULL;
    int cnt, subnode;

    /* sanity check: has this node been scanned already? */
    if (find_dev_fdt_mapping(s, node) != NULL) {
        /* TODO: ensure connectivity to parent device */
        return 0;
    }

    /* check for compatible property list */
    cnt = fdt_stringlist_count(fdt, node, DTB_PROP_COMPAT);
    if (cnt > 0) {

        /* add device to machine */
        dev = machine_dtb_add_device_node(s, parent, fdt, node);
    }

    fdt_for_each_subnode(subnode, fdt, node) {
        const char *node_name = fdt_get_name(fdt, subnode, NULL);

        /* recursively search subnodes */
        machine_dtb_scan_node(s, dev, fdt,
                fdt_subnode_offset(fdt, node, node_name));
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

static void machine_dtb_parse_init(MachineState *mch)
{
    DynamicState *s = g_new(DynamicState, 1);
    MemoryRegion *ram = g_new(MemoryRegion, 1);
    Error *err = NULL;
    void *fdt = NULL;
    uint64_t reg_addr, reg_size;
    int len, offset, cpu_node;

    s->mapping = NULL;
    s->mch = mch;
    s->ram = ram;

    /* load the device tree with some checking */
    if (!mch->dtb) {
            error_report("DTB Parser machine requires use of -dtb parameter");
            exit(1);
    }
    if (machine_load_device_tree(mch->dtb, &fdt)) {
            error_report("Cannot locate device tree file!");
            exit(1);
    }

    /* get model name from dtb */
    s->model_name =
        g_strdup((const char *)qemu_fdt_getprop(fdt, "/", "model", &len, &err));
    pr_debug("Scanning Device Tree for %s...\n", s->model_name);

    /* identify and init the cpu(s) */
    s->ncpus = 0;

    /* look for cpu node in device tree */
    cpu_node = fdt_subnode_offset(fdt, 0, DTB_CPU_NODE);
    if (cpu_node < 0) {
        if (!mch->cpu_type) {
            error_report("Device tree has no CPU node. "
                         "Use -cpu to manually determine CPU");
            exit(1);
        }

        /* no CPU node found - try to use information passed to QEMU */
        s->cpu[0] = cpu_create(mch->cpu_type);
        if (!s->cpu[0]) {
            error_report("No CPU node found. Could not manually init CPU %s",
                    mch->cpu_type);
            exit(1);
        }

        /*
         * TODO: when cpus node is missing, typically single CPU topology.
         *       Need to decide if supporting manually setting multi-core
         *       without cpus node use case is worth implementing
         */
        pr_debug("No CPU node found. Setting CPU to %s", mch->cpu_type);
        s->ncpus++;
    } else {
        /* scan for cpus */
        fdt_for_each_subnode(offset, fdt, cpu_node) {
            const char *cpu_path, *cpu_type;
            uint32_t freq;

            cpu_path = fdt_get_name(fdt, offset, NULL);
            cpu_type = (const char *)qemu_fdt_getprop(fdt, cpu_path,
                                        DTB_PROP_COMPAT, &len, &error_fatal);

            pr_debug("Found CPU %s (%s)", cpu_path, cpu_type);

            freq = qemu_fdt_getprop_cell(fdt, cpu_path,
                        "timebase-frequency", &len, &err);
            if (len < 0) {
                /*
                 * FIXME: create machine property to pass default
                 * cpu frequency instead of hardcoding value
                 */
                freq = 200000000;
                pr_debug("No frequency found. Default to %dMHz",
                        freq / 1000000);
            }

            /* create cpu using compatible string */
            s->cpu[s->ncpus] = cpu_create(cpu_type);
            if (!s->cpu[s->ncpus]) {
                /* try stripping manufacturer from cpu type */
                const char *cpu_type_model = str_fdt_compat_strip(cpu_type);

                if (cpu_type_model) {
                    s->cpu[s->ncpus] = cpu_create(cpu_type_model);
                }

                if (!cpu_type_model || !s->cpu[s->ncpus]) {
                    error_report("Unable to initialize CPU");
                    exit(1);
                }
            }
            s->ncpus++;
        }
    }

    /* get system memory size */
    fdt_simple_addr_size(fdt, fdt_subnode_offset(fdt, 0, DTB_MEM_NODE),
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

    /* iterate through all root nodes recursively */
    fdt_for_each_subnode(offset, fdt, 0) {
        const char *node_name = fdt_get_name(fdt, offset, NULL);

        if (strcmp(node_name, DTB_CPU_NODE) == 0 ||
            strcmp(node_name, DTB_MEM_NODE) == 0)
            continue;

        machine_dtb_scan_node(s, NULL, fdt,
            fdt_subnode_offset(fdt, 0, node_name));
    }

    /*
     * TODO: connectivity fixup code after devices are created
     *       including clocks, interrupts, gpio, etc.
     */

    error_report("Completed init. Exiting...");
    exit(1);
}

static void machine_dtb_parse_machine_init(MachineClass *mc)
{
    mc->desc = "device tree parsed machine";
    mc->init = machine_dtb_parse_init;
    /*mc->max_cpus = DTB_PARSE_MAX_NUM_CPUS;*/
}

DEFINE_MACHINE("dtb_parse", machine_dtb_parse_machine_init)
