/*
 * Copyright (c) 2025, sakumisu
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "usbd_core.h"
#include "usbd_cdc_acm.h"

/*!< endpoint address */
#define CDC_IN_EP  0x81
#define CDC_OUT_EP 0x02
#define CDC_INT_EP 0x83

#define USBD_VID           0x1209
#define USBD_PID           0x5741
#define USBD_MAX_POWER     100
#define USBD_LANGID_STRING 1033

/*!< config descriptor size */
#define USB_CONFIG_SIZE (9 + CDC_ACM_DESCRIPTOR_LEN)

#ifdef CONFIG_USB_HS
#define CDC_MAX_MPS 512
#else
#define CDC_MAX_MPS 64
#endif

/* GDB-visible counters for enumeration debugging. */
volatile uint32_t usb_event_reset_count;
volatile uint32_t usb_event_connected_count;
volatile uint32_t usb_event_configured_count;
volatile uint32_t usb_device_desc_req_count;
volatile uint32_t usb_config_desc_req_count;
volatile uint32_t usb_string_desc_req_count;
volatile uint32_t usb_last_string_index;

static const uint8_t device_descriptor[] = {
    USB_DEVICE_DESCRIPTOR_INIT(USB_2_0, 0xEF, 0x02, 0x01, USBD_VID, USBD_PID, 0x0100, 0x01)
};

static const uint8_t config_descriptor[] = {
    USB_CONFIG_DESCRIPTOR_INIT(USB_CONFIG_SIZE, 0x02, 0x01, USB_CONFIG_BUS_POWERED, USBD_MAX_POWER),
    CDC_ACM_DESCRIPTOR_INIT(0x00, CDC_INT_EP, CDC_OUT_EP, CDC_IN_EP, CDC_MAX_MPS, 0x02)
};

static const uint8_t device_quality_descriptor[] = {
    ///////////////////////////////////////
    /// device qualifier descriptor
    ///////////////////////////////////////
    0x0a,
    USB_DESCRIPTOR_TYPE_DEVICE_QUALIFIER,
    0x00,
    0x02,
    0x00,
    0x00,
    0x00,
    0x40,
    0x00,
    0x00,
};

static const char *string_descriptors[] = {
    (const char[]){ 0x09, 0x04 }, /* Langid */
    "ArduPilot",                  /* Manufacturer */
    "CUAVv5 RTT",                 /* Product */
    "RTTUSB0001",                 /* Serial Number */
};

static const uint8_t *device_descriptor_callback(uint8_t speed)
{
    usb_device_desc_req_count++;
    return device_descriptor;
}

static const uint8_t *config_descriptor_callback(uint8_t speed)
{
    usb_config_desc_req_count++;
    return config_descriptor;
}

static const uint8_t *device_quality_descriptor_callback(uint8_t speed)
{
    return device_quality_descriptor;
}

static const char *string_descriptor_callback(uint8_t speed, uint8_t index)
{
    usb_string_desc_req_count++;
    usb_last_string_index = index;
    if (index > 3) {
        return NULL;
    }
    return string_descriptors[index];
}

const struct usb_descriptor cdc_descriptor = {
    .device_descriptor_callback = device_descriptor_callback,
    .config_descriptor_callback = config_descriptor_callback,
    .device_quality_descriptor_callback = device_quality_descriptor_callback,
    .string_descriptor_callback = string_descriptor_callback
};

extern void usbd_serial_reset_tx(void);
extern void usbd_serial_rearm_rx(void);

static void usbd_event_handler(uint8_t busid, uint8_t event)
{
    switch (event) {
        case USBD_EVENT_RESET:
            usb_event_reset_count++;
            usbd_serial_reset_tx();
            break;
        case USBD_EVENT_CONNECTED:
            usb_event_connected_count++;
            break;
        case USBD_EVENT_DISCONNECTED:
            usbd_serial_reset_tx();
            break;
        case USBD_EVENT_RESUME:
            usbd_serial_reset_tx();
            usbd_serial_rearm_rx();
            break;
        case USBD_EVENT_SUSPEND:
            break;
        case USBD_EVENT_CONFIGURED:
            usb_event_configured_count++;
            usbd_serial_reset_tx();
            usbd_serial_rearm_rx();
            break;
        case USBD_EVENT_SET_REMOTE_WAKEUP:
            break;
        case USBD_EVENT_CLR_REMOTE_WAKEUP:
            break;

        default:
            break;
    }
}

extern void usbd_cdc_acm_serial_init(uint8_t busid, uint8_t in_ep, uint8_t out_ep);

void cdc_acm_chardev_init(uint8_t busid, uintptr_t reg_base)
{
    usbd_desc_register(busid, &cdc_descriptor);

    usbd_cdc_acm_serial_init(busid, CDC_IN_EP, CDC_OUT_EP);
    usbd_initialize(busid, reg_base, usbd_event_handler);
}

static int cdc_acm_enter(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    finsh_set_device("usb-acm0");
    rt_console_set_device("usb-acm0");

    return 0;
}
MSH_CMD_EXPORT(cdc_acm_enter, cdc_acm_enter);

static int cdc_acm_exit(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    finsh_set_device(RT_CONSOLE_DEVICE_NAME);
    rt_console_set_device(RT_CONSOLE_DEVICE_NAME);

    return 0;
}
MSH_CMD_EXPORT(cdc_acm_exit, cdc_acm_exit);