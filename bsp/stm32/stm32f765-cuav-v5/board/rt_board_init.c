/*
 * Strong symbol for rt_hw_board_init. Set VTOR first for ArduPilot bootloader
 * (app at 0x08008000), then HAL, clock, pin/usart init.
 */
#include <rtthread.h>
#include "board.h"
#include "drv_gpio.h"

#ifdef BSP_USING_SPI
#include "drv_spi.h"
#endif

extern int rt_hw_pin_init(void);
extern int rt_hw_usart_init(void);
extern void __libc_init_array(void);

#ifdef BSP_USING_SPI
static void _spi_device_init(void)
{
#ifdef BSP_USING_SPI1
    rt_hw_spi_device_attach("spi1", "spi10", GET_PIN(F, 11)); /* ICM42688  CS=PF11 */
    rt_hw_spi_device_attach("spi1", "spi11", GET_PIN(F, 4));  /* BMI055_G  CS=PF4  */
    rt_hw_spi_device_attach("spi1", "spi12", GET_PIN(G, 10)); /* BMI055_A  CS=PG10 */
#endif
#ifdef BSP_USING_SPI2
    rt_hw_spi_device_attach("spi2", "spi20", GET_PIN(F, 5));  /* RAMTRON   CS=PF5  */
#endif
#ifdef BSP_USING_SPI4
    rt_hw_spi_device_attach("spi4", "spi40", GET_PIN(F, 10)); /* MS5611    CS=PF10 */
#endif
}
#endif

void rt_hw_board_init(void)
{
    SCB->VTOR = 0x08008000U;

    if (HAL_Init() != HAL_OK) {
        while (1) { }
    }
    SystemClock_Config();
    rt_hw_pin_init();
    rt_hw_usart_init();
#ifdef RT_USING_HEAP
    rt_system_heap_init(HEAP_BEGIN, HEAP_END);
#endif
}

#ifdef BSP_USING_SPI
extern int rt_hw_spi_init(void);

static int _spi_device_board_init(void)
{
    rt_hw_spi_init();
    _spi_device_init();
    return 0;
}
INIT_PREV_EXPORT(_spi_device_board_init);
#endif

static int rtt_run_cpp_ctors(void)
{
    __libc_init_array();
    return 0;
}
INIT_COMPONENT_EXPORT(rtt_run_cpp_ctors);
