/*
 *  FDT Machine Clock Routines
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
#include "hw/qdev-properties.h"
#include "exec/address-spaces.h"
#include "sysemu/sysemu.h"
#include "sysemu/device_tree.h"

#include "hw/core/cpu.h"
#include "hw/qdev-clock.h"
#include "hw/fdt-mch/fdt-mch.h"

static const char DTB_PROP_CLOCKS[] = "clocks";
static const char DTB_PROP_CLKFREQ[] = "clock-frequency";

typedef struct clock_cb_parameters {
    Clock *clk;
    int node;
    uint32_t mult, div;
} ClockParameters;


/* callback to update derived clocks */
static void mch_fdt_clock_cb(void *opaque)
{
    ClockParameters *param = (ClockParameters *)opaque;
    uint64_t new_freq;

    g_assert(!param);
    new_freq = clock_get(param->clk->source);

    /* FIXME: need proper DIV_MULT() - for now try to detect overflow */
    if (new_freq * param->mult < new_freq) {
        new_freq /= param->div;
        new_freq *= param->mult;
    } else {
        new_freq *= param->mult;
        new_freq /= param->div;
    }
    clock_set_hz(param->clk, new_freq);
}

void mch_fdt_build_clocktree(DynamicState *s, const void *fdt)
{
    unsigned idx = 0;
    int *node_map;
    int node;

    if (s->num_clocks == 0) {
        pr_debug("no clocks found in device tree");
        return;
    }
    pr_debug("Found %u clocks in device tree", s->num_clocks);
    pr_debug("Building clock tree...");

    /* allocate node map and pointers for clocks */
    node_map = g_new0(int, s->num_clocks);
    s->clocks = g_new0(Clock *, s->num_clocks);

    /* phase 1: search device tree for clock sources first */
    fdt_for_each_node_with_prop(node, fdt, -1, "#clock-cells") {
        const char *node_name = fdt_get_name(fdt, node, NULL);
        const void *freq_prop;
        int len;

        g_assert(idx <= s->num_clocks);
        s->clocks[idx] = clock_new(OBJECT(s->mch), node_name);

        /* add frequency for clock sources */
        freq_prop = fdt_getprop(fdt, node, DTB_PROP_CLKFREQ, &len);
        if (freq_prop) {
            uint64_t freq = fdt_read_long(freq_prop, len / 4);

            clock_set_hz(s->clocks[idx], freq);
            pr_debug("* adding clocksource %s at %llu", node_name, freq);
        } else {
            pr_debug("* found derivative clock %s", node_name);
        }

        node_map[idx] = node;
        idx++;
    }

    /*
     * pr_debug("dump clock nodes");
     * for (idx = 0; idx < s->num_clocks; idx++) {
     *     pr_debug(" node %u (%s) = %d", idx,
     *         fdt_get_name(fdt, node_map[idx], NULL), node_map[idx]);
     * }
     */

    /* phase 2: connect clocks & devices to source */
    fdt_for_each_node_with_prop(node, fdt, -1, DTB_PROP_CLOCKS) {
        const char *node_name = fdt_get_name(fdt, node, NULL);
        DeviceState *dev = NULL;
        Clock *target_clk = NULL;
        uint32_t phandle;

        /* is this a derived clock, or a device? */
        if (fdt_getprop(fdt, node, "#clock-cells", NULL) > 0) {
            /* get index for derived clock structure */
            for (idx = 0; idx < s->num_clocks; idx++) {
                if (node_map[idx] == node) {
                    break;
                }
            }
            g_assert(idx < s->num_clocks);
            target_clk = s->clocks[idx];
        } else {
            /* if this device hasn't been mapped, skip it */
            if (mch_fdt_dev_find_mapping(s, node, &dev)) {
                continue;
            }
            /* if no device was created, then skip it */
            if (!dev) {
                pr_debug("no device for %s found. skiping...",
                            node_name);
                continue;
            }
        }

        for (idx = 0;
             fdt_read_array_u32(fdt, node, DTB_PROP_CLOCKS, idx, &phandle) == 0;
             idx++) {
            Clock *parent_clk;
            unsigned node_idx;
            int ref_node = fdt_node_offset_by_phandle(fdt, phandle);

            /* lookup list index for node of clock source */
            for (node_idx = 0; node_idx < s->num_clocks; node_idx++) {
                if (node_map[node_idx] == ref_node) {
                    break;
                }
            }
            g_assert(node_idx < s->num_clocks);
            parent_clk = s->clocks[node_idx];

            /* link parent clock to derived clock or device */
            if (!dev) {
                ClockParameters *param = g_malloc0(sizeof(ClockParameters));
                const fdt32_t *val;
                int len;

                param->clk = target_clk;
                param->node = node;
                param->mult = 1;
                param->div = 1;

                /*
                 * TODO: For now, we only support "fixed-factor-clock" type.
                 *       May add support for more complex derived clocks in
                 *       the future.
                 */
                val = fdt_getprop(fdt, node, "clock-mult", &len);
                if (val > 0) {
                    param->mult = fdt_read_long(val, len);
                }
                val = fdt_getprop(fdt, node, "clock-div", &len);
                if (val > 0) {
                    param->div = fdt_read_long(val, len);
                }

                clock_set_source(target_clk, parent_clk);
                clock_set_callback(target_clk, mch_fdt_clock_cb,
                                                        (void *)param);
            } else {
                /* get clock name from fdt if clock-names property exists */
                const char *clock_name = fdt_stringlist_get(fdt, node,
                                                "clock-names", idx, NULL);

                /* use node name if no clock-names property found */
                if (!clock_name) {
                    /* terminate string at '@' if it exists */
                    char *alt_clock_name =
                        subst_compat_string(fdt_get_name(fdt, node, NULL),
                                                '@', '\0');
                    qdev_connect_clock_in(dev, alt_clock_name, parent_clk);
                    g_free(alt_clock_name);
                } else {
                    /* link parent clock to device */
                    qdev_connect_clock_in(dev, clock_name, parent_clk);
                }
            }
        }
    }

    g_free(node_map);
}

