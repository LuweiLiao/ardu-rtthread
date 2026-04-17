/*
 * usb_dc_dwc2.h - Public declarations for DWC2 USB driver
 * Allows usbd_serial.c to access DWC2 IN endpoint registers for EPENA check.
 */
#ifndef USB_DC_DWC2_H
#define USB_DC_DWC2_H

#include "usb_dwc2_reg.h"

/* Access IN endpoint registers (single-bus, busid=0) */
#define DWC2_INEP(ep_idx) \
    ((DWC2_INEndpointTypeDef *)(g_usbdev_bus[0].reg_base + USB_OTG_IN_ENDPOINT_BASE + ((ep_idx)*USB_OTG_EP_REG_SIZE)))

#endif /* USB_DC_DWC2_H */
