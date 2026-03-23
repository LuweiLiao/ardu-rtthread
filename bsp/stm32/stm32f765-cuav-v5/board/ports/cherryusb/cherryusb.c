/*
 * CherryUSB CDC ACM board init for CUAV V5 (STM32F7 USB OTG_FS PA11/PA12)
 */
#include "board.h"
#include "rtthread.h"
#include "usb_dc.h"

/* rtt_dbg_uart_begin_called is defined in AP_HAL_RTT/UARTDriver.cpp when
 * building with ArduPilot. Provide a stub for RTT standalone builds. */
#if !defined(ARDUPILOT_FULL) || ARDUPILOT_FULL == 0
volatile int rtt_dbg_uart_begin_called = 0;
#else
extern volatile int rtt_dbg_uart_begin_called;
#endif

#define USB_OTG_FS_DCTL_ADDR    (USB_OTG_FS_PERIPH_BASE + 0x800U + 0x04U)
#define USB_OTG_FS_PCGCCTL_ADDR (USB_OTG_FS_PERIPH_BASE + 0xE00U)
#define USB_OTG_FS_GRSTCTL_ADDR (USB_OTG_FS_PERIPH_BASE + 0x010U)
#define USB_OTG_FS_GCCFG_ADDR   (USB_OTG_FS_PERIPH_BASE + 0x030U)

#define USB_OTG_GRSTCTL_CSRST  (1U << 0)
#define USB_OTG_GRSTCTL_AHBIDL (1U << 31)

#ifdef RT_CHERRYUSB_DEVICE_TEMPLATE_CDC_ACM_CHARDEV
static int rt_hw_cherryusb_cdc_init(void)
{
    extern void cdc_acm_chardev_init(uint8_t busid, uintptr_t reg_base);
    volatile uint32_t *const usb_dctl = (volatile uint32_t *)USB_OTG_FS_DCTL_ADDR;
    volatile uint32_t *const usb_pcgcctl = (volatile uint32_t *)USB_OTG_FS_PCGCCTL_ADDR;
    volatile uint32_t *const usb_grstctl = (volatile uint32_t *)USB_OTG_FS_GRSTCTL_ADDR;

    /* Re-present the USB device after bootloader handoff:
     * force a visible disconnect, wait long enough for the host to notice,
     * then rebuild the DWC2 device controller and reconnect. */
    __HAL_RCC_USB_OTG_FS_CLK_ENABLE();
    *usb_pcgcctl &= ~(0x1U | 0x2U); /* STOPCLK | GATECLK */
    
    /* Force VBUS valid for device mode (no external VBUS sensing) */
    volatile uint32_t *const usb_gccfg = (volatile uint32_t *)USB_OTG_FS_GCCFG_ADDR;
    *usb_gccfg |= (1U << 21) | (1U << 20); /* NOVBUSSENS | VBUSBSEN */
    
    *usb_dctl |= USB_OTG_DCTL_SDIS;
    (void)usb_dc_deinit(0);
    for (uint32_t i = 0; i < 100000U && ((*usb_grstctl & USB_OTG_GRSTCTL_AHBIDL) == 0U); i++) {
        __NOP();
    }
    *usb_grstctl |= USB_OTG_GRSTCTL_CSRST;
    for (uint32_t i = 0; i < 100000U && ((*usb_grstctl & USB_OTG_GRSTCTL_CSRST) != 0U); i++) {
        __NOP();
    }
    rt_thread_mdelay(1500);
    cdc_acm_chardev_init(0, USB_OTG_FS_PERIPH_BASE);
    *usb_pcgcctl &= ~(0x1U | 0x2U); /* STOPCLK | GATECLK */
    *usb_dctl &= ~USB_OTG_DCTL_SDIS;
    {
        rt_device_t check = rt_device_find("usb-acm0");
        rt_kprintf("[USB] cdc_init done, usb-acm0=%p, begin_called=%d\n",
                   check, rtt_dbg_uart_begin_called);
    }
    return 0;
}
INIT_COMPONENT_EXPORT(rt_hw_cherryusb_cdc_init);
#endif
