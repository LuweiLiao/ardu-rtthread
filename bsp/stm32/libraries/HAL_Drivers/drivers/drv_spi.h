/*
 * Copyright (c) 2006-2025 RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2018-11-5      SummerGift   first version
 */

#ifndef __DRV_SPI_H__
#define __DRV_SPI_H__

#include <rtthread.h>
#include "rtdevice.h"
#include <rthw.h>
#include <drv_common.h>
#include "drv_dma.h"
#include <ipc/completion.h>

#ifdef __cplusplus
extern "C" {
#endif

rt_err_t rt_hw_spi_device_attach(const char *bus_name, const char *device_name, rt_base_t cs_pin);
rt_err_t rt_hw_spi_device_detach(const char *device_name);

#ifdef __cplusplus
}
#endif

struct stm32_spi_config
{
    SPI_TypeDef *Instance;
    char *bus_name;
    IRQn_Type irq_type;
    struct dma_config *dma_rx, *dma_tx;
};

struct stm32_spi_device
{
    rt_uint32_t pin;
    char *bus_name;
    char *device_name;
};

#define SPI_USING_RX_DMA_FLAG   (1<<0)
#define SPI_USING_TX_DMA_FLAG   (1<<1)

/*
 * Forward-declare the LLD bus context so drv_spi.h does not need to pull in
 * drv_spi_lld.h (which is STM32F7-specific).  The concrete type is resolved
 * in drv_spi.c and drv_spi_lld.c which both include drv_spi_lld.h directly.
 */
struct spi_lld_bus;

/* stm32 spi dirver class */
struct stm32_spi
{
    SPI_HandleTypeDef handle;
    struct stm32_spi_config *config;
    struct rt_spi_configuration *cfg;

    struct
    {
        DMA_HandleTypeDef handle_rx;
        DMA_HandleTypeDef handle_tx;
    } dma;

    rt_uint8_t spi_dma_flag;
    struct rt_spi_bus spi_bus;

    struct rt_completion cpt;

    /*
     * Optional Low-Level DMA context (STM32F7 only).
     * When non-NULL, spixfer() calls spi_lld_xfer() instead of the HAL DMA
     * path, eliminating the SPI_EndRxTxTransaction busy-wait from the ISR.
     */
    struct spi_lld_bus *lld;
};

#endif /*__DRV_SPI_H__ */
