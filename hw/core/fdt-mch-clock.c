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
#include "hw/sysbus.h"
#include "hw/qdev-properties.h"
#include "exec/address-spaces.h"
#include "sysemu/sysemu.h"
#include "sysemu/device_tree.h"

#include "hw/core/cpu.h"
#include "hw/qdev-clock.h"
#include "hw/fdt-mch/fdt-mch.h"

static const char FDT_PROP_CLOCKS[] = "clocks";
static const char FDT_PROP_CLKFREQ[] = "clock-frequency";

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

void mch_fdt_init_clocks(DynamicState *s, const void *fdt)
{
    unsigned idx = 0;
    int node;

    /* count number of clock sources */
    s->num_clocks = 0;
    fdt_for_each_node_with_prop(node, fdt, -1, "#clock-cells") {
        s->num_clocks++;
    }

    if (s->num_clocks == 0) {
        pr_debug("no clocks found in device tree");
        return;
    }
    pr_debug("Found %u clocks in device tree", s->num_clocks);

    /* allocate node map and pointers for clocks */
    s->clock_node_map = g_new0(int, s->num_clocks);
    s->clocks = g_new0(Clock *, s->num_clocks);

    /* create clock objects for use during device initialization */
    fdt_for_each_node_with_prop(node, fdt, -1, "#clock-cells") {
        const char *node_name = fdt_get_name(fdt, node, NULL);
        uint64_t freq;

        g_assert(idx <= s->num_clocks);
        s->clocks[idx] = clock_new(OBJECT(s->mch), node_name);

        /* add frequency for clock sources */
        if (fdt_getprop_long(fdt, node, FDT_PROP_CLKFREQ, &freq) == 0) {
            clock_set_hz(s->clocks[idx], freq);
            pr_debug("* adding clocksource %s at %llu", node_name, freq);
        } else {
            pr_debug("* found derivative clock %s", node_name);
        }

        s->clock_node_map[idx] = node;
        idx++;
    }

    /* hook up derivative clocks */
    fdt_for_each_node_with_prop(node, fdt, -1, FDT_PROP_CLOCKS) {
        const char *node_name = fdt_get_name(fdt, node, NULL);
        Clock *target_clk = NULL;
        uint32_t phandle;

        /* is this a derived clock? */
        if (fdt_getprop(fdt, node, "#clock-cells", NULL) > 0) {
            /* get index for derived clock structure */
            for (idx = 0; idx < s->num_clocks; idx++) {
                if (s->clock_node_map[idx] == node) {
                    break;
                }
            }
            assert(idx < s->num_clocks);
            target_clk = s->clocks[idx];
        } else {
            /* skip devices */
            continue;
        }

        /* iterate over all clocks listed */
        for (idx = 0;
             fdt_getprop_array_u32(fdt, node, FDT_PROP_CLOCKS,
                                   idx, &phandle) == 0;
             idx++) {
            Clock *parent_clk;
            unsigned node_idx;
            int ref_node = fdt_node_offset_by_phandle(fdt, phandle);

            /* lookup list index for node of clock source */
            for (node_idx = 0; node_idx < s->num_clocks; node_idx++) {
                if (s->clock_node_map[node_idx] == ref_node) {
                    break;
                }
            }
            g_assert(node_idx < s->num_clocks);
            parent_clk = s->clocks[node_idx];

            /* link parent clock to derived clock */
                ClockParameters *param = g_malloc0(sizeof(ClockParameters));

                param->clk = target_clk;
                param->node = node;
                param->mult = 1;
                param->div = 1;

                /*
                 * TODO: For now, we only support "fixed-factor-clock" type.
                 *       May add support for more complex derived clocks in
                 *       the future.
                 */
                fdt_getprop_cell(fdt, node, "clock-mult", &param->mult);
                fdt_getprop_cell(fdt, node, "clock-div", &param->div);

                clock_set_source(target_clk, parent_clk);
                clock_set_callback(target_clk, mch_fdt_clock_cb,
                                                        (void *)param);
        }
    }
}

void mch_fdt_link_clocks(DynamicState *s, DeviceState *dev,
                         const void *fdt, int node)
{
    uint32_t phandle;
    unsigned idx;

    /* do we even have clocks for this device? */
    if (fdt_getprop(fdt, node, FDT_PROP_CLOCKS, NULL) < 0) {
        return;
    }

    /* iterate over all clocks referenced */
    for (idx = 0;
         fdt_getprop_array_u32(fdt, node, FDT_PROP_CLOCKS, idx, &phandle) == 0;
         idx++) {
        const char *clock_name;
        Clock *parent_clk;
        unsigned node_idx;
        int ref_node = fdt_node_offset_by_phandle(fdt, phandle);

        /* lookup list index for node of clock source */
        for (node_idx = 0; node_idx < s->num_clocks; node_idx++) {
            if (s->clock_node_map[node_idx] == ref_node) {
                break;
            }
        }
        assert(node_idx < s->num_clocks);
        parent_clk = s->clocks[node_idx];

        /* get clock name from fdt if clock-names property exists */
        clock_name = fdt_stringlist_get(fdt, node, "clock-names", idx, NULL);
        if (!clock_name) {
            /* use clk node name if no clock-names property found */
            char *alt_clock_name =
                        subst_compat_string(fdt_get_name(fdt, node, NULL),
                                                '@', '\0');

            /* link parent clock to device */
            if (qdev_init_clock_in(dev, alt_clock_name, NULL, NULL) != NULL) {
                qdev_connect_clock_in(dev, alt_clock_name, parent_clk);
            }
            g_free(alt_clock_name);
        } else {

            /* link parent clock to device */
            if (qdev_init_clock_in(dev, clock_name, NULL, NULL) != NULL) {
                qdev_connect_clock_in(dev, clock_name, parent_clk);
            }
        }
    }
}
