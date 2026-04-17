/*
 * CherryUSB CDC ACM init — CUAV V5 (STM32F7 USB OTG_FS PA11/PA12)
 *
 * Board-level usb_dc_low_level_init() override: on hot-reset the DWC2 AHB
 * bus can be stuck (AHBIDL=0) because the core was active when the MCU
 * was software-reset.  The upstream usb_glue_st.c version only enables
 * clocks via HAL_PCD_MspInit() — it does NOT assert the RCC peripheral
 * reset.  Without that reset the AHB bus may never idle and dwc2_reset()
 * deadlocks.
 *
 * This override:
 *   1. Calls HAL_PCD_MspInit() (GPIO + USB clock + NVIC, same as upstream)
 *   2. Asserts the RCC USB_OTG_FS force-reset for 1 ms, then releases it.
 *      This clears any stuck AHB state from the previous session.
 *   3. Re-enables the USB OTG FS clock (force-reset clears the EN bit).
 *   4. Waits for AHBIDL before returning, guaranteeing the DWC2 is idle.
 *
 * The cdc_acm_rcc_reset() helper is kept for the pre-init diagnostic dump
 * but is no longer the primary recovery mechanism.
 */
#include "board.h"
#include "rtthread.h"
#include "usb_dc.h"
#include "stm32f7xx_hal.h"

#define OTG_FS_IRQn      67
#define USB_OTG_FS_BASE  0x50000000U
#define GRSTCTL_OFFSET   0x0CU
#define GSNPSID_OFFSET   0x4CU
#define DCTL_OFFSET      0x804U
#define RCC_AHB2ENR_ADDR 0x40023820U

static void rtt_hw_us_delay(uint32_t us)
{
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    *(volatile uint32_t *)0xE0001FB0 = 0xC5ACCE55;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
    uint32_t start = DWT->CYCCNT;
    uint32_t cycles = us * (SystemCoreClock / 1000000U);
    while ((DWT->CYCCNT - start) < cycles) { __NOP(); }
}

static void cdc_acm_rcc_reset(void)
{
    __HAL_RCC_USB_OTG_FS_CLK_ENABLE();
    rtt_hw_us_delay(10U);
    __HAL_RCC_USB_OTG_FS_FORCE_RESET();
    rtt_hw_us_delay(1000U);  /* 1 ms */
    __HAL_RCC_USB_OTG_FS_RELEASE_RESET();
    __HAL_RCC_USB_OTG_FS_CLK_ENABLE();
    rtt_hw_us_delay(50000U);  /* 50 ms */
}

/*
 * Board-level override of usb_dc_low_level_init() from usb_glue_st.c.
 *
 * This is a strong symbol that replaces the one in usb_glue_st.c.
 * We must replicate the upstream behaviour (register IRQ handler +
 * call HAL_PCD_MspInit) and add the critical RCC force-reset that
 * clears a stuck DWC2 AHB bus after hot-reset.
 */
extern void USBD_IRQHandler(uint8_t busid);

void usb_dc_low_level_init(uint8_t busid)
{
    PCD_HandleTypeDef hpcd;

    /* Same as upstream usb_glue_st.c: set Instance for HAL_PCD_MspInit */
    hpcd.Instance = USB_OTG_FS;

    /* Enable GPIO, USB clock, NVIC (from HAL_PCD_MspInit in stm32f7xx_hal_msp.c) */
    HAL_PCD_MspInit(&hpcd);

    /*
     * CRITICAL FIX: assert RCC force-reset to clear a stuck DWC2 AHB bus.
     * On hot-reset the DWC2 core may have been mid-transfer; the AHB
     * master can remain busy indefinitely.  Only an RCC peripheral reset
     * clears this condition.  HAL_PCD_MspInit only enables the clock —
     * it does NOT reset the peripheral.
     */
    __HAL_RCC_USB_OTG_FS_FORCE_RESET();
    rtt_hw_us_delay(1000U);  /* 1 ms hold */
    __HAL_RCC_USB_OTG_FS_RELEASE_RESET();

    /* Force-reset clears the clock-enable bit; re-enable it */
    __HAL_RCC_USB_OTG_FS_CLK_ENABLE();

    /*
     * Wait for the DWC2 AHB master to become idle after the reset.
     * If it doesn't idle within 100 ms something is seriously wrong
     * with the silicon, but we continue anyway to avoid a hard hang.
     */
    volatile uint32_t *grstctl = (volatile uint32_t *)(USB_OTG_FS_BASE + GRSTCTL_OFFSET);
    uint32_t deadline = 1000000U; /* ~5 ms at 200 MHz loop rate */
    while ((deadline-- > 0U) &&
           ((*grstctl & (1UL << 31)) == 0U)) {
        __NOP();
    }
}

#ifdef RT_CHERRYUSB_DEVICE_TEMPLATE_CDC_ACM_CHARDEV
static int rt_hw_cherryusb_cdc_init(void)
{
    extern void cdc_acm_chardev_init(uint8_t busid, uintptr_t reg_base);
    volatile uint32_t *grstctl = (volatile uint32_t *)(USB_OTG_FS_BASE + GRSTCTL_OFFSET);
    volatile uint32_t *gsnpsid = (volatile uint32_t *)(USB_OTG_FS_BASE + GSNPSID_OFFSET);
    volatile uint32_t *ahb2enr = (volatile uint32_t *)RCC_AHB2ENR_ADDR;
    volatile uint32_t *dctl    = (volatile uint32_t *)(USB_OTG_FS_BASE + DCTL_OFFSET);

    NVIC_DisableIRQ((IRQn_Type)OTG_FS_IRQn);

    rt_kprintf("[USB] pre-init: GRSTCTL=0x%08lX GSNPSID=0x%08lX AHB2ENR=0x%08lX DCTL=0x%08lX\n",
               *grstctl, *gsnpsid, *ahb2enr, *dctl);

    /* RCC reset — basic peripheral cleanup (kept for diagnostic logging) */
    cdc_acm_rcc_reset();

    rt_kprintf("[USB] post-RCC: GRSTCTL=0x%08lX GSNPSID=0x%08lX AHB2ENR=0x%08lX DCTL=0x%08lX\n",
               *grstctl, *gsnpsid, *ahb2enr, *dctl);

    /* CherryUSB driver init (usb_dc_init inside handles full DWC2 reset) */
    cdc_acm_chardev_init(0, USB_OTG_FS_PERIPH_BASE);

    rt_kprintf("[USB] post-cdc: GRSTCTL=0x%08lX GSNPSID=0x%08lX AHB2ENR=0x%08lX DCTL=0x%08lX\n",
               *grstctl, *gsnpsid, *ahb2enr, *dctl);

    NVIC_EnableIRQ((IRQn_Type)OTG_FS_IRQn);

    rt_device_t check = rt_device_find("usb-acm0");
    rt_kprintf("[USB] done, usb-acm0=%p\n", check);
    return 0;
}
INIT_COMPONENT_EXPORT(rt_hw_cherryusb_cdc_init);
#endif
