/*
 * Copyright (c) 2025, sakumisu
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <rtthread.h>
#include <rtdevice.h>

#include "usbd_core.h"
#include "usbd_cdc_acm.h"

#define DEV_FORMAT_CDC_ACM "usb-acm%d"

#ifndef CONFIG_USBDEV_MAX_CDC_ACM_CLASS
#define CONFIG_USBDEV_MAX_CDC_ACM_CLASS (4)
#endif

#ifndef CONFIG_USBDEV_SERIAL_RX_BUFSIZE
#define CONFIG_USBDEV_SERIAL_RX_BUFSIZE (2048)
#endif

#ifndef CONFIG_USBDEV_SERIAL_TX_BUFSIZE
#define CONFIG_USBDEV_SERIAL_TX_BUFSIZE (4096)
#endif

struct usbd_serial {
    struct rt_device parent;
    uint8_t busid;
    uint8_t in_ep;
    uint8_t out_ep;
    struct usbd_interface intf_ctrl;
    struct usbd_interface intf_data;
    usb_osal_sem_t tx_done;
    uint8_t minor;
    volatile uint8_t tx_active;
    char name[32];
    struct rt_ringbuffer rx_rb;
    rt_uint8_t rx_rb_buffer[CONFIG_USBDEV_SERIAL_RX_BUFSIZE];
    struct rt_ringbuffer tx_rb;
    rt_uint8_t tx_rb_buffer[CONFIG_USBDEV_SERIAL_TX_BUFSIZE];
    USB_MEM_ALIGNX uint8_t tx_pkt[USB_ALIGN_UP(64, CONFIG_USB_ALIGN_SIZE)];
};

static uint32_t g_devinuse = 0;

volatile uint32_t dbg_serial_write_calls = 0;
volatile int32_t  dbg_serial_write_ret   = 0;
volatile int32_t  dbg_serial_sem_ret     = 0;
volatile uint32_t dbg_serial_write_ok    = 0;
volatile uint32_t dbg_serial_write_timeout = 0;
volatile uint32_t dbg_serial_write_notcfg = 0;
volatile uint32_t dbg_serial_bulkin_cnt  = 0;
volatile uint32_t dbg_serial_tx_kick     = 0;
volatile uint32_t dbg_serial_tx_kick_fail = 0;

static USB_NOCACHE_RAM_SECTION USB_MEM_ALIGNX uint8_t g_usbd_serial_cdc_acm_rx_buf[CONFIG_USBDEV_MAX_CDC_ACM_CLASS][USB_ALIGN_UP(512, CONFIG_USB_ALIGN_SIZE)];

static struct usbd_serial g_usbd_serial_cdc_acm[CONFIG_USBDEV_MAX_CDC_ACM_CLASS];

static void usbd_serial_kick_tx(struct usbd_serial *serial);

volatile uint32_t dbg_serial_rx_rearm = 0;
volatile uint32_t dbg_serial_rx_rearm_skip = 0;
volatile uint32_t dbg_serial_bulkout_cnt = 0;

void usbd_serial_reset_tx(void)
{
    for (uint8_t devno = 0; devno < CONFIG_USBDEV_MAX_CDC_ACM_CLASS; devno++) {
        struct usbd_serial *serial = &g_usbd_serial_cdc_acm[devno];
        serial->tx_active = 0;
        rt_ringbuffer_reset(&serial->tx_rb);
        if (serial->in_ep) {
            usbd_ep_recover_stuck(serial->busid, serial->in_ep);
        }
        if (serial->out_ep) {
            dbg_serial_rx_rearm++;
            usbd_ep_start_read(serial->busid, serial->out_ep,
                g_usbd_serial_cdc_acm_rx_buf[serial->minor],
                usbd_get_ep_mps(serial->busid, serial->out_ep));
        } else {
            dbg_serial_rx_rearm_skip++;
        }
    }
}

void usbd_serial_rearm_rx(void)
{
    for (uint8_t devno = 0; devno < CONFIG_USBDEV_MAX_CDC_ACM_CLASS; devno++) {
        struct usbd_serial *serial = &g_usbd_serial_cdc_acm[devno];
        if (serial->out_ep) {
            uint16_t mps = usbd_get_ep_mps(serial->busid, serial->out_ep);
            if (mps > 0) {
                dbg_serial_rx_rearm++;
                usbd_ep_start_read(serial->busid, serial->out_ep,
                    g_usbd_serial_cdc_acm_rx_buf[serial->minor], mps);
            } else {
                dbg_serial_rx_rearm_skip++;
            }
        } else {
            dbg_serial_rx_rearm_skip++;
        }
    }
}

static struct usbd_serial *usbd_serial_alloc(void)
{
    uint8_t devno;
    struct usbd_serial *serial;

    for (devno = 0; devno < CONFIG_USBDEV_MAX_CDC_ACM_CLASS; devno++) {
        if ((g_devinuse & (1U << devno)) == 0) {
            g_devinuse |= (1U << devno);

            serial = &g_usbd_serial_cdc_acm[devno];
            memset(serial, 0, sizeof(struct usbd_serial));
            serial->minor = devno;
            snprintf(serial->name, CONFIG_USBHOST_DEV_NAMELEN, DEV_FORMAT_CDC_ACM, serial->minor);
            return serial;
        }
    }
    return NULL;
}

static void usbd_serial_free(struct usbd_serial *serial)
{
    uint8_t devno = serial->minor;

    if (devno < 32) {
        g_devinuse &= ~(1U << devno);
    }
    memset(serial, 0, sizeof(struct usbd_serial));
}

static rt_err_t usbd_serial_open(struct rt_device *dev, rt_uint16_t oflag)
{
    struct usbd_serial *serial;

    RT_ASSERT(dev != RT_NULL);

    serial = (struct usbd_serial *)dev;

    for (int i = 0; i < 300; i++) {
        if (usb_device_is_configured(serial->busid))
            break;
        rt_thread_mdelay(10);
    }

    if (usb_device_is_configured(serial->busid) && serial->out_ep) {
        usbd_ep_start_read(serial->busid, serial->out_ep,
                           g_usbd_serial_cdc_acm_rx_buf[serial->minor],
                           usbd_get_ep_mps(serial->busid, serial->out_ep));
    }
    return RT_EOK;
}

static rt_ssize_t usbd_serial_read(struct rt_device *dev,
                                   rt_off_t pos,
                                   void *buffer,
                                   rt_size_t size)
{
    struct usbd_serial *serial;

    RT_ASSERT(dev != RT_NULL);

    serial = (struct usbd_serial *)dev;

    if (!usb_device_is_configured(serial->busid)) {
        return -RT_EPERM;
    }

    return rt_ringbuffer_get(&serial->rx_rb, (rt_uint8_t *)buffer, size);
}

static volatile uint32_t dbg_serial_unstick_cnt = 0;

static rt_ssize_t usbd_serial_write(struct rt_device *dev,
                                    rt_off_t pos,
                                    const void *buffer,
                                    rt_size_t size)
{
    struct usbd_serial *serial;

    RT_ASSERT(dev != RT_NULL);

    serial = (struct usbd_serial *)dev;

    dbg_serial_write_calls++;

    if (!usb_device_is_configured(serial->busid)) {
        dbg_serial_write_notcfg++;
        serial->tx_active = 0;
        return -RT_EPERM;
    }

    /*
     * Self-heal stuck tx_active: if tx_active has been 1 for many
     * consecutive write calls that ALSO fail to enqueue (tx_rb full),
     * the IN endpoint is stuck.  Only count failed enqueues — successful
     * writes prove the TX chain is making progress even while tx_active
     * remains 1 (normal during sustained traffic with multiple drain calls).
     */
    static uint32_t tx_stuck_counter = 0;

    rt_size_t written = rt_ringbuffer_put(&serial->tx_rb, (const rt_uint8_t *)buffer, size);

    if (written > 0) {
        dbg_serial_write_ok++;
        tx_stuck_counter = 0;
        /* kick_tx has its own atomic tx_active guard — safe to call
         * unconditionally from thread context. */
        usbd_serial_kick_tx(serial);
    } else {
        dbg_serial_write_timeout++;
        if (serial->tx_active && ++tx_stuck_counter > 100) {
            usbd_ep_recover_stuck(serial->busid, serial->in_ep);
            serial->tx_active = 0;
            tx_stuck_counter = 0;
            dbg_serial_unstick_cnt++;
        }
    }

    return written;
}

static void usbd_serial_kick_tx(struct usbd_serial *serial)
{
    if (!usb_device_is_configured(serial->busid)) {
        serial->tx_active = 0;
        rt_ringbuffer_reset(&serial->tx_rb);
        return;
    }

    /*
     * Atomic tx_active guard: only one caller (ISR or thread) may
     * proceed to submit a USB transfer at a time.  Check and claim
     * tx_active under PRIMASK so an ISR preemption between the
     * check and the set is impossible — this is the root-fix for
     * the EPENA race that caused 8-14 s endpoint stalls.
     */
    uint32_t primask;
    __asm volatile("mrs %0, primask" : "=r"(primask));
    __asm volatile("cpsid i" ::: "memory");
    if (serial->tx_active) {
        /* Another transfer is in-flight; the completion ISR (or
         * the drain loop) will call kick_tx again when it finishes. */
        __asm volatile("msr primask, %0" :: "r"(primask) : "memory");
        return;
    }
    serial->tx_active = 1;
    __asm volatile("msr primask, %0" :: "r"(primask) : "memory");

    uint16_t mps = usbd_get_ep_mps(serial->busid, serial->in_ep);
    if (!mps) mps = 64;

    uint32_t avail = rt_ringbuffer_data_len(&serial->tx_rb);
    if (avail == 0) {
        serial->tx_active = 0;
        return;
    }

    uint16_t to_send = (avail > mps) ? mps : (uint16_t)avail;

    rt_size_t got = rt_ringbuffer_get(&serial->tx_rb, serial->tx_pkt, to_send);
    if (got == 0) {
        serial->tx_active = 0;
        return;
    }

    dbg_serial_tx_kick++;
    int ret = usbd_ep_start_write(serial->busid, serial->in_ep, serial->tx_pkt, got);
    if (ret < 0) {
        serial->tx_active = 0;
        dbg_serial_tx_kick_fail++;
    }
}

#ifdef RT_USING_DEVICE_OPS
const static struct rt_device_ops usbd_serial_ops = {
    NULL,
    usbd_serial_open,
    NULL,
    usbd_serial_read,
    usbd_serial_write,
    NULL
};
#endif

rt_err_t usbd_serial_register(struct usbd_serial *serial,
                              void *data)
{
    rt_err_t ret;
    struct rt_device *device;
    RT_ASSERT(serial != RT_NULL);

    device = &(serial->parent);

    device->type = RT_Device_Class_Char;
    device->rx_indicate = RT_NULL;
    device->tx_complete = RT_NULL;

#ifdef RT_USING_DEVICE_OPS
    device->ops = &usbd_serial_ops;
#else
    device->init = NULL;
    device->open = usbd_serial_open;
    device->close = NULL;
    device->read = usbd_serial_read;
    device->write = usbd_serial_write;
    device->control = NULL;
#endif
    device->user_data = data;

    ret = rt_device_register(device, serial->name, RT_DEVICE_FLAG_RDWR | RT_DEVICE_FLAG_INT_RX | RT_DEVICE_FLAG_REMOVABLE);

#ifdef RT_USING_POSIX_DEVIO
    device->fops = NULL;
#endif
    rt_ringbuffer_init(&serial->rx_rb, serial->rx_rb_buffer, sizeof(serial->rx_rb_buffer));
    rt_ringbuffer_init(&serial->tx_rb, serial->tx_rb_buffer, sizeof(serial->tx_rb_buffer));

    return ret;
}

void usbd_cdc_acm_bulk_out(uint8_t busid, uint8_t ep, uint32_t nbytes)
{
    struct usbd_serial *serial;

    dbg_serial_bulkout_cnt++;

    for (uint8_t devno = 0; devno < CONFIG_USBDEV_MAX_CDC_ACM_CLASS; devno++) {
        serial = &g_usbd_serial_cdc_acm[devno];
        if (serial->out_ep == ep) {
            rt_ringbuffer_put(&serial->rx_rb, g_usbd_serial_cdc_acm_rx_buf[serial->minor], nbytes);
            usbd_ep_start_read(serial->busid, serial->out_ep,
                g_usbd_serial_cdc_acm_rx_buf[serial->minor],
                usbd_get_ep_mps(serial->busid, serial->out_ep));

            if (serial->parent.rx_indicate) {
                serial->parent.rx_indicate(&serial->parent, nbytes);
            }
            break;
        }
    }
}

void usbd_cdc_acm_bulk_in(uint8_t busid, uint8_t ep, uint32_t nbytes)
{
    struct usbd_serial *serial;

    dbg_serial_bulkin_cnt++;

    for (uint8_t devno = 0; devno < CONFIG_USBDEV_MAX_CDC_ACM_CLASS; devno++) {
        serial = &g_usbd_serial_cdc_acm[devno];
        if (serial->in_ep == ep) {
            /*
             * No ZLP for CDC ACM streaming — data is consumed as a
             * byte stream by the host CDC driver.  Sending a ZLP
             * after MPS-aligned packets can stall the DWC2 endpoint
             * (PKTCNT=1/XFRSIZ=0 stuck) and is unnecessary here.
             * ChibiOS CDC likewise never sends ZLP for stream data.
             */
            /* Clear tx_active and re-arm the next transfer.
             * After XFRC the hardware cleared EPENA, so kick_tx's
             * usbd_ep_start_write will NOT enter the EPENA flush
             * path (the deadly 200K nop dwc2_flush_txfifo).
             * kick_tx now keeps PRIMASK=1 through the entire
             * usbd_ep_start_write call, so even in ISR context
             * (where PRIMASK is already 1) the race window between
             * tx_active check and EPENA set is closed. */
            serial->tx_active = 0;
            usbd_serial_kick_tx(serial);
            return;
        }
    }
}

void usbd_cdc_acm_serial_init(uint8_t busid, uint8_t in_ep, uint8_t out_ep)
{
    struct usbd_serial *serial;

    struct usbd_endpoint cdc_out_ep = {
        .ep_addr = out_ep,
        .ep_cb = usbd_cdc_acm_bulk_out
    };

    struct usbd_endpoint cdc_in_ep = {
        .ep_addr = in_ep,
        .ep_cb = usbd_cdc_acm_bulk_in
    };

    serial = usbd_serial_alloc();
    if (serial == NULL) {
        USB_LOG_ERR("No more serial device available\n");
        return;
    }

    serial->busid = busid;
    serial->in_ep = in_ep;
    serial->out_ep = out_ep;
    serial->tx_done = usb_osal_sem_create(0);
    serial->tx_active = 0;

    usbd_add_interface(busid, usbd_cdc_acm_init_intf(busid, &serial->intf_ctrl));
    usbd_add_interface(busid, usbd_cdc_acm_init_intf(busid, &serial->intf_data));
    usbd_add_endpoint(busid, &cdc_out_ep);
    usbd_add_endpoint(busid, &cdc_in_ep);

    if (usbd_serial_register(serial, NULL) != RT_EOK) {
        USB_LOG_ERR("Failed to register serial device\n");
        usbd_serial_free(serial);
        return;
    }

    USB_LOG_INFO("USB CDC ACM Serial Device %s initialized\n", serial->name);
}

volatile uint32_t dbg_dtr_set_cnt = 0;
volatile uint32_t dbg_dtr_clear_cnt = 0;
static volatile uint8_t g_dtr_active = 0;

bool usb_cdc_dtr_active(void)
{
    return g_dtr_active != 0;
}

void usbd_cdc_acm_set_dtr(uint8_t busid, uint8_t intf, bool dtr)
{
    (void)busid;
    (void)intf;
    if (dtr) {
        dbg_dtr_set_cnt++;
        g_dtr_active = 1;
        for (uint8_t devno = 0; devno < CONFIG_USBDEV_MAX_CDC_ACM_CLASS; devno++) {
            struct usbd_serial *serial = &g_usbd_serial_cdc_acm[devno];
            if (serial->in_ep) {
                usbd_ep_recover_stuck(serial->busid, serial->in_ep);
            }
            serial->tx_active = 0;
            rt_ringbuffer_reset(&serial->tx_rb);
            if (serial->out_ep) {
                dbg_serial_rx_rearm++;
                usbd_ep_start_read(serial->busid, serial->out_ep,
                    g_usbd_serial_cdc_acm_rx_buf[serial->minor],
                    usbd_get_ep_mps(serial->busid, serial->out_ep));
            }
        }
    } else {
        dbg_dtr_clear_cnt++;
        g_dtr_active = 0;
        for (uint8_t devno = 0; devno < CONFIG_USBDEV_MAX_CDC_ACM_CLASS; devno++) {
            struct usbd_serial *serial = &g_usbd_serial_cdc_acm[devno];
            if (serial->in_ep) {
                usbd_ep_recover_stuck(serial->busid, serial->in_ep);
            }
            serial->tx_active = 0;
            rt_ringbuffer_reset(&serial->tx_rb);
        }
    }
}
