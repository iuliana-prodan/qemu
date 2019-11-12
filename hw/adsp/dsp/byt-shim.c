/* Core DSP SHIM support for Baytrail audio DSP.
 *
 * Copyright (C) 2016 Intel Corporation
 *
 * Author: Liam Girdwood <liam.r.girdwood@linux.intel.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, see <http://www.gnu.org/licenses/>.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu-common.h"
#include "sysemu/sysemu.h"
#include "hw/boards.h"
#include "hw/loader.h"
#include "elf.h"
#include "exec/memory.h"
#include "exec/address-spaces.h"
#include "migration/vmstate.h"
#include "qemu/main-loop.h"
#include "sysemu/runstate.h"

#include "qemu/io-bridge.h"
#include "hw/audio/adsp-dev.h"
#include "hw/adsp/shim.h"
#include "hw/adsp/log.h"
#include "byt.h"
#include "common.h"

static void rearm_ext_timer(struct adsp_dev *adsp, struct adsp_io_info *info)
{
    uint32_t wake = info->region[SHIM_EXT_TIMER_CNTLL >> 2];

    info->region[SHIM_EXT_TIMER_STAT >> 2] =
        (qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) - adsp->timer[0].start) /
        (1000000 / adsp->timer[0].clk_kHz);

    timer_mod(adsp->timer[0].timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
            muldiv64(wake - info->region[SHIM_EXT_TIMER_STAT >> 2],
                1000000, adsp->timer[0].clk_kHz));
}

static void *pmc_work(void *data)
{
    struct adsp_io_info *info = data;
    struct adsp_dev *adsp = info->adsp;

    /* delay for PMC to do the work */
    usleep(50);

    /* perform any action after IRQ - ideally we should do this in thread*/
    switch (adsp->pmc_cmd) {
    case PMC_SET_LPECLK:

        /* set the clock bits */
        info->region[SHIM_CLKCTL >> 2] &= ~SHIM_FR_LAT_CLK_MASK;
        info->region[SHIM_CLKCTL >> 2] |=
            info->region[SHIM_FR_LAT_REQ >> 2] & SHIM_FR_LAT_CLK_MASK;

        /* tell the DSP clock has been updated */
        info->region[SHIM_CLKCTL >> 2] &= ~SHIM_CLKCTL_FRCHNGGO;
        info->region[SHIM_CLKCTL >> 2] |= SHIM_CLKCTL_FRCHNGACK;

        break;
    default:
        break;
    }

    log_text(adsp->log, LOG_IRQ_BUSY,
        "irq: SC send busy interrupt 0x%x\n", adsp->pmc_cmd);

    /* now send IRQ to DSP from SC completion */
    info->region[SHIM_IPCLPESCH >> 2] &= ~SHIM_IPCLPESCH_BUSY;
    info->region[SHIM_IPCLPESCH >> 2] |= SHIM_IPCLPESCH_DONE;

    info->region[SHIM_ISRLPESC >> 2] &= ~SHIM_ISRLPESC_BUSY;
    info->region[SHIM_ISRLPESC >> 2] |= SHIM_ISRLPESC_DONE;

    qemu_mutex_lock_iothread();
    adsp_set_lvl1_irq(adsp, adsp->desc->pmc_irq, 1);
    qemu_mutex_unlock_iothread();
    return NULL;
}

void byt_ext_timer_cb(void *opaque)
{
    struct adsp_io_info *info = opaque;
    struct adsp_dev *adsp = info->adsp;
    uint32_t pisr = info->region[SHIM_PISR >> 2];

    pisr |= SHIM_PISR_EXTT;
    info->region[SHIM_PISR >> 2] = pisr;
    adsp_set_lvl1_irq(adsp, adsp->desc->ext_timer_irq, 1);
}

static void shim_reset(void *opaque)
{
    struct adsp_io_info *info = opaque;
    struct adsp_reg_space *space = info->space;

    memset(info->region, 0, space->desc.size);
}

/* SHIM IO from ADSP */
static uint64_t shim_read(void *opaque, hwaddr addr,
        unsigned size)
{
    struct adsp_io_info *info = opaque;
    struct adsp_dev *adsp = info->adsp;
    struct adsp_reg_space *space = info->space;

    switch (addr) {
    case SHIM_EXT_TIMER_STAT:
        info->region[addr >> 2] =
        (qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) - adsp->timer[0].start) /
            (1000000 / adsp->timer[0].clk_kHz);
        break;
    case SHIM_PISR:
        info->region[addr >> 2] = 0;
        break;
    default:
        break;
    }

    log_read(adsp->log, space, addr, size,
        info->region[addr >> 2]);

    return info->region[addr >> 2];
}

/* SHIM IO from ADSP */
static void shim_write(void *opaque, hwaddr addr,
        uint64_t val, unsigned size)
{
    struct adsp_io_info *info = opaque;
    struct adsp_dev *adsp = info->adsp;
    struct adsp_reg_space *space = info->space;
    struct qemu_io_msg_reg32 reg32;
    struct qemu_io_msg_irq irq;
    uint32_t active, isrx, isrlpesc;

    log_write(adsp->log, space, addr, val, size,
        info->region[addr >> 2]);

    /* special case registers */
    switch (addr) {
    case SHIM_IPCDL:

        /* set value via SHM */
        info->region[addr >> 2] = val;

        /* reset the CPU and halt if we are dead to ease debugging */
        if ((val & 0xffff0000) == 0xdead0000) {

            log_text(adsp->log, LOG_CPU_RESET,
                "firmware is dead. cpu held in reset\n");

            cpu_reset(CPU(adsp->xtensa[0]->cpu));
            //vm_stop(RUN_STATE_SHUTDOWN); TODO: fix, causes hang
            adsp->in_reset = 1;
        }
        break;
    case SHIM_IPCDH:
        /* DSP to host IPC command */

        /* set value via SHM */
        info->region[addr >> 2] = val;

        /* set/clear status bit */
        isrx = info->region[SHIM_ISRX >> 2] & ~(SHIM_ISRX_DONE | SHIM_ISRX_BUSY);
        isrx |= val & SHIM_IPCD_BUSY ? SHIM_ISRX_BUSY : 0;
        isrx |= val & SHIM_IPCD_DONE ? SHIM_ISRX_DONE : 0;
        info->region[SHIM_ISRX >> 2] = isrx;

        /* do we need to send an IRQ ? */
        if (val & SHIM_IPCD_BUSY) {

            log_text(adsp->log, LOG_IRQ_BUSY,
                "irq: send busy interrupt 0x%8.8lx\n", val);

            /* send IRQ to parent */
            irq.hdr.type = QEMU_IO_TYPE_IRQ;
            irq.hdr.msg = QEMU_IO_MSG_IRQ;
            irq.hdr.size = sizeof(irq);
            irq.irq = 0;

            qemu_io_send_msg(&irq.hdr);
        }
        break;
    case SHIM_IPCXH:
        /* DSP to host IPC notify */

        /* set value via SHM */
        info->region[addr >> 2] = val;

        /* set/clear status bit */
        isrx = info->region[SHIM_ISRX >> 2] & ~(SHIM_ISRX_DONE | SHIM_ISRX_BUSY);
        isrx |= val & SHIM_IPCX_BUSY ? SHIM_ISRX_BUSY : 0;
        isrx |= val & SHIM_IPCX_DONE ? SHIM_ISRX_DONE : 0;
        info->region[SHIM_ISRX >> 2] = isrx;

        /* do we need to send an IRQ ? */
        if (val & SHIM_IPCX_DONE) {

            log_text(adsp->log, LOG_IRQ_DONE,
                "irq: send done interrupt 0x%8.8lx\n", val);

            /* send IRQ to parent */
            irq.hdr.type = QEMU_IO_TYPE_IRQ;
            irq.hdr.msg = QEMU_IO_MSG_IRQ;
            irq.hdr.size = sizeof(irq);
            irq.irq = 0;

            qemu_io_send_msg(&irq.hdr);
        }
        break;
    case SHIM_IMRD:

        /* set value via SHM */
        info->region[addr >> 2] = val;

        /* DSP IPC interrupt mask */
        active = info->region[SHIM_ISRD >> 2] & ~(info->region[SHIM_IMRD >> 2]);

        log_text(adsp->log, LOG_IRQ_ACTIVE,
            "irq: IMRD masking %x mask %x active %x\n",
            info->region[SHIM_ISRD >> 2],
            info->region[SHIM_IMRD >> 2], active);

        if (!active) {
            adsp_set_lvl1_irq(adsp, adsp->desc->ia_irq, 0);
        }

        break;
    case SHIM_CSR:

        /* set value via SHM */
        info->region[addr >> 2] = val;

        /* now send msg to HOST VM to notify register write */
        reg32.hdr.type = QEMU_IO_TYPE_REG;
        reg32.hdr.msg = QEMU_IO_MSG_REG32W;
        reg32.hdr.size = sizeof(reg32);
        reg32.reg = addr;
        reg32.val = val;
        qemu_io_send_msg(&reg32.hdr);
        break;
    case SHIM_PISR:
        /* write 1 to clear bits */
        /* set value via SHM */
        info->region[addr >> 2] &= ~val;

        if (val & SHIM_PISR_EXTT) {
            adsp_set_lvl1_irq(adsp, adsp->desc->ext_timer_irq, 0);
        }

        break;
    case SHIM_PIMR:

        /* set value via SHM */
        info->region[addr >> 2] = val;

        /* DSP IO interrupt mask */
        active = info->region[SHIM_PISR >> 2] & ~(info->region[SHIM_PIMR >> 2]);

        log_text(adsp->log, LOG_IRQ_ACTIVE,
            "irq: PIMR masking %x mask %x active %x\n",
            info->region[SHIM_PISR >> 2],
            info->region[SHIM_PIMR >> 2], active);

        if (!(active & SHIM_PISR_EXTT)) {
            adsp_set_lvl1_irq(adsp, adsp->desc->ext_timer_irq, 0);
        }
        break;
    case SHIM_EXT_TIMER_CNTLL:
        /* set the timer timeout value via SHM */
        info->region[addr >> 2] = val;
        if (info->region[SHIM_EXT_TIMER_CNTLH >> 2] & SHIM_EXT_TIMER_RUN)
            rearm_ext_timer(adsp, info);
        break;
    case SHIM_EXT_TIMER_CNTLH:

        /* enable the timer ? */
        if (val & SHIM_EXT_TIMER_RUN &&
            !(info->region[addr >> 2] & SHIM_EXT_TIMER_RUN)) {
                adsp->timer[0].start = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
                rearm_ext_timer(adsp, info);
        }

        /* set value via SHM */
        info->region[addr >> 2] = val;

        /* set/clear ext timer */
        if (val & SHIM_EXT_TIMER_CLEAR)
            info->region[SHIM_EXT_TIMER_STAT >> 2] = 0;

        break;
    case SHIM_EXT_TIMER_STAT:
        /* set status value via SHM - should not be written to ? */
        info->region[addr >> 2] = val;
        rearm_ext_timer(adsp, info);
        break;
    case SHIM_IPCLPESCH:
        /* DSP to SCU IPC command */

        /* set value via SHM */
        info->region[addr >> 2] = val;

        /* set/clear status bit */
        isrlpesc = info->region[SHIM_ISRLPESC >> 2] &
            ~(SHIM_ISRLPESC_DONE | SHIM_ISRLPESC_BUSY);
        isrlpesc |= val & SHIM_IPCLPESCH_BUSY ? SHIM_ISRLPESC_BUSY : 0;
        isrlpesc |= val & SHIM_IPCLPESCH_DONE ? SHIM_ISRLPESC_DONE : 0;
        info->region[SHIM_ISRLPESC >> 2] = isrlpesc;

        /* do we need to send an IRQ ? */
        if (val & SHIM_IPCLPESCH_BUSY) {

            adsp->pmc_cmd = val & 0xff;

            /* perform any action prior to IRQ */
            switch (adsp->pmc_cmd) {
            case PMC_SET_LPECLK:
                info->region[SHIM_CLKCTL >> 2] |= SHIM_CLKCTL_FRCHNGGO;
                info->region[SHIM_CLKCTL >> 2] &= ~SHIM_CLKCTL_FRCHNGACK;
                break;
            default:
                break;
            }

            qemu_thread_create(&adsp->pmc_thread, "pmc",
                pmc_work, info, QEMU_THREAD_DETACHED);
        }
        break;
    case SHIM_IMRLPESC:

        /* set value via SHM */
        info->region[addr >> 2] = val;

        /* DSP IPC interrupt mask */
        active = info->region[SHIM_ISRLPESC >> 2] & ~(info->region[SHIM_IMRLPESC >> 2]);

        log_text(adsp->log, LOG_IRQ_ACTIVE,
            "irq: SHIM_IMRLPESC masking %x mask %x active %x\n",
            info->region[SHIM_ISRLPESC >> 2],
            info->region[SHIM_IMRLPESC >> 2], active);


        if (!active) {
            adsp_set_lvl1_irq(adsp, adsp->desc->pmc_irq, 0);
        }

        break;
    default:
        break;
    }
}

/* 32 bit SHIM IO from host */
static void do_shim32(struct adsp_dev *adsp, struct qemu_io_msg *msg)
{
    struct qemu_io_msg_reg32 *m = (struct qemu_io_msg_reg32 *)msg;

    switch (m->reg) {
    case SHIM_CSR:
        /* check for reset bit and stall bit */
        if (!adsp->in_reset && (m->val & SHIM_CSR_RST)) {

            log_text(adsp->log, LOG_CPU_RESET, "cpu: reset\n");

            cpu_reset(CPU(adsp->xtensa[0]->cpu));
            //vm_stop(RUN_STATE_SHUTDOWN); TODO: fix, causes hang
            adsp->in_reset = 1;

        } else if (adsp->in_reset && !(m->val & SHIM_CSR_STALL)) {

            log_text(adsp->log, LOG_CPU_RESET, "cpu: running\n");

            cpu_resume(CPU(adsp->xtensa[0]->cpu));
            vm_start();
            adsp->in_reset = 0;
        }
        break;
    default:
        break;
    }
}

/* 64 bit SHIM IO from host */
static void do_shim64(struct adsp_dev *adsp, struct qemu_io_msg *msg)
{
    struct qemu_io_msg_reg64 *m = (struct qemu_io_msg_reg64 *)msg;

    switch (m->reg) {
    case SHIM_CSR:
        /* check for reset bit and stall bit */
        if (!adsp->in_reset && (m->val & SHIM_CSR_RST)) {

            log_text(adsp->log, LOG_CPU_RESET, "cpu: reset\n");

            cpu_reset(CPU(adsp->xtensa[0]->cpu));
            //vm_stop(RUN_STATE_SHUTDOWN); TODO: fix, causes hang
            adsp->in_reset = 1;

        } else if (adsp->in_reset && !(m->val & SHIM_CSR_STALL)) {

            log_text(adsp->log, LOG_CPU_RESET, "cpu: running\n");

            cpu_resume(CPU(adsp->xtensa[0]->cpu));
            vm_start();
            adsp->in_reset = 0;
        }
        break;
    default:
        break;
    }
}

void adsp_byt_shim_msg(struct adsp_dev *adsp, struct qemu_io_msg *msg)
{
    switch (msg->msg) {
    case QEMU_IO_MSG_REG32W:
        do_shim32(adsp, msg);
        break;
    case QEMU_IO_MSG_REG32R:
        break;
    case QEMU_IO_MSG_REG64W:
        do_shim64(adsp, msg);
        break;
    case QEMU_IO_MSG_REG64R:
        break;
    default:
        fprintf(stderr, "unknown register msg %d\n", msg->msg);
        break;
    }
}

void adsp_byt_irq_msg(struct adsp_dev *adsp, struct qemu_io_msg *msg)
{
    struct adsp_io_info *info = adsp->shim;
    uint32_t active;

    active = info->region[SHIM_ISRD >> 2] & ~(info->region[SHIM_IMRD >> 2]);

    log_text(adsp->log, LOG_IRQ_ACTIVE,
        "IRQ: from HOST status %x mask %x active %x cmd %x\n",
        info->region[SHIM_ISRD >> 2],
        info->region[SHIM_IMRD >> 2], active,
        info->region[SHIM_IPCX >> 2]);

    if (active) {
        qemu_mutex_lock_iothread();
        adsp_set_lvl1_irq(adsp, adsp->desc->ia_irq, 1);
        qemu_mutex_unlock_iothread();
    }
}

const MemoryRegionOps byt_shim_ops = {
    .read = shim_read,
    .write = shim_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

void adsp_byt_shim_init(struct adsp_dev *adsp, MemoryRegion *parent,
        struct adsp_io_info *info)
{
    shim_reset(info);
    adsp->shim = info;
    adsp->timer[0].timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, &byt_ext_timer_cb, info);
    adsp->timer[0].clk_kHz = 2500;
}
