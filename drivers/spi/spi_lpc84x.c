/*
 * Copyright (c) 2026 Aerlync Labs Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT nxp_lpc84x_spi

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(spi_lpc84x, CONFIG_SPI_LOG_LEVEL);

#include <zephyr/drivers/spi.h>
#include <zephyr/drivers/clock_control.h>
#include <zephyr/drivers/pinctrl.h>
#include <fsl_spi.h>

#include "spi_context.h"

struct spi_lpc84x_config {
	SPI_Type *base;
	const struct device *clock_dev;
	clock_control_subsys_t clock_subsys;
	const struct pinctrl_dev_config *pincfg;
#ifdef CONFIG_SPI_LPC84X_INTERRUPT
	/* Pointer to the instance-specific IRQ connect function */
	void (*irq_config_func)(const struct device *dev);
#endif
};

struct spi_lpc84x_data {
	struct spi_context ctx;
	bool initialized;
	const struct spi_config *last_cfg;
#ifdef CONFIG_SPI_LPC84X_INTERRUPT
	spi_master_handle_t fsl_handle;    /* FSL transactional handle            */
	struct k_sem transfer_sem;         /* signals ISR → transceive completion */
	volatile status_t transfer_status; /* HAL callback result                 */
#endif
};

#ifdef CONFIG_SPI_LPC84X_INTERRUPT
static void spi_lpc84x_transfer_callback(SPI_Type *base, spi_master_handle_t *handle,
					 status_t status, void *userData);
#endif

static int spi_lpc84x_configure(const struct device *dev, const struct spi_config *spi_cfg)
{
	const struct spi_lpc84x_config *config = dev->config;
	struct spi_lpc84x_data *data = dev->data;
	spi_master_config_t master_config;
	uint32_t clock_freq;
	int ret;

	if (data->initialized && data->last_cfg != NULL &&
	    data->last_cfg->frequency == spi_cfg->frequency &&
	    data->last_cfg->operation == spi_cfg->operation) {
		data->ctx.config = spi_cfg;
		return 0;
	}

	if (spi_cfg->operation & SPI_HALF_DUPLEX) {
		LOG_ERR("Half-duplex not supported");
		return -ENOTSUP;
	}

	if (spi_cfg->operation & SPI_OP_MODE_SLAVE) {
		LOG_ERR("Slave mode not supported");
		return -ENOTSUP;
	}

	if (SPI_WORD_SIZE_GET(spi_cfg->operation) != 8) {
		LOG_ERR("Word sizes other than 8 bits are not supported");
		return -ENOTSUP;
	}

	SPI_MasterGetDefaultConfig(&master_config);

	if (spi_cfg->operation & SPI_MODE_CPOL) {
		master_config.clockPolarity = kSPI_ClockPolarityActiveLow;
	} else {
		master_config.clockPolarity = kSPI_ClockPolarityActiveHigh;
	}

	if (spi_cfg->operation & SPI_MODE_CPHA) {
		master_config.clockPhase = kSPI_ClockPhaseSecondEdge;
	} else {
		master_config.clockPhase = kSPI_ClockPhaseFirstEdge;
	}

	master_config.baudRate_Bps = spi_cfg->frequency;

	if (clock_control_get_rate(config->clock_dev, config->clock_subsys, &clock_freq)) {
		return -EINVAL;
	}

	if (!data->initialized) {
		SPI_MasterInit(config->base, &master_config, clock_freq);
		data->initialized = true;

#ifdef CONFIG_SPI_LPC84X_INTERRUPT
		/*
		 * Create the FSL transactional handle once.
		 * The callback and userData are set here; the handle is reused
		 * for every subsequent transfer.
		 */
		SPI_MasterTransferCreateHandle(config->base, &data->fsl_handle,
					       spi_lpc84x_transfer_callback, (void *)dev);
#endif
	} else {
		ret = SPI_MasterSetBaudRate(config->base, master_config.baudRate_Bps, clock_freq);
		if (ret != kStatus_Success) {
			LOG_DBG("Master baudrate set failed\n");
			return -EINVAL;
		}
		uint32_t cfg = config->base->CFG;

		cfg &= ~(SPI_CFG_CPOL_MASK | SPI_CFG_CPHA_MASK);
		if (spi_cfg->operation & SPI_MODE_CPOL) {
			cfg |= SPI_CFG_CPOL_MASK;
		}
		if (spi_cfg->operation & SPI_MODE_CPHA) {
			cfg |= SPI_CFG_CPHA_MASK;
		}
		config->base->CFG = cfg;
	}

	data->ctx.config = spi_cfg;
	data->last_cfg = spi_cfg;

	return 0;
}

#ifdef CONFIG_SPI_LPC84X_INTERRUPT

/*
 * FSL transactional callback – called from SPI_MasterTransferHandleIRQ()
 * inside the Zephyr ISR dispatcher (spi_lpc84x_irq_handler below).
 *
 * NOTE: The FSL HAL passes the handle as the second parameter.  We only need
 * the `status` to signal completion; the device pointer is recovered via
 * userData.
 */
static void spi_lpc84x_transfer_callback(SPI_Type *base, spi_master_handle_t *handle,
					 status_t status, void *userData)
{
	const struct device *dev = (const struct device *)userData;
	struct spi_lpc84x_data *data = dev->data;

	ARG_UNUSED(base);
	ARG_UNUSED(handle);

	data->transfer_status = status;
	k_sem_give(&data->transfer_sem);
}

/*
 * Zephyr ISR – connected to the hardware IRQ line.
 * Delegates to the FSL IRQ handler which drives the state machine and fires
 * spi_lpc84x_transfer_callback() when a transfer finishes.
 */
static void spi_lpc84x_irq_handler(const struct device *dev)
{
	const struct spi_lpc84x_config *config = dev->config;
	struct spi_lpc84x_data *data = dev->data;

	SPI_MasterTransferHandleIRQ(config->base, &data->fsl_handle);
}

/*
 * Interrupt-driven transceive: iterates through the Zephyr scatter-gather
 * buffer lists one byte at a time using the FSL non-blocking API, blocking
 * on a semaphore for each byte until the ISR completes it.
 *
 * For short transfers a whole-buffer approach is also straightforward, but
 * the scatter-gather model below keeps parity with the polling path and
 * correctly handles multi-segment buf_sets.
 */
static int spi_lpc84x_transceive_irq(const struct device *dev, const struct spi_config *spi_cfg,
				     const struct spi_buf_set *tx_bufs,
				     const struct spi_buf_set *rx_bufs)
{
	const struct spi_lpc84x_config *config = dev->config;
	struct spi_lpc84x_data *data = dev->data;
	int ret = 0;

	spi_context_lock(&data->ctx, false, NULL, NULL, spi_cfg);

	ret = spi_lpc84x_configure(dev, spi_cfg);
	if (ret) {
		goto out;
	}

	spi_context_buffers_setup(&data->ctx, tx_bufs, rx_bufs, 1);
	spi_context_cs_control(&data->ctx, true);

	while (spi_context_tx_buf_on(&data->ctx) || spi_context_rx_buf_on(&data->ctx)) {

		uint8_t tx_data = 0U;
		uint8_t rx_data = 0U;
		spi_transfer_t xfer = {0};

		if (spi_context_tx_buf_on(&data->ctx)) {
			tx_data = *data->ctx.tx_buf;
			xfer.txData = &tx_data;
		} else {
			/* TX exhausted – send dummy so RX can proceed */
			xfer.txData = NULL;
		}

		xfer.rxData = spi_context_rx_buf_on(&data->ctx) ? &rx_data : NULL;
		xfer.dataSize = 1U;
		xfer.configFlags = kSPI_EndOfTransfer;

		/* Reset completion semaphore */
		k_sem_reset(&data->transfer_sem);

		status_t status =
			SPI_MasterTransferNonBlocking(config->base, &data->fsl_handle, &xfer);
		if (status != kStatus_Success) {
			LOG_ERR("SPI_MasterTransferNonBlocking failed: %d", (int)status);
			ret = -EIO;
			break;
		}

		/* Block until ISR fires the semaphore */
		if (k_sem_take(&data->transfer_sem, K_MSEC(100)) != 0) {
			LOG_ERR("SPI interrupt transfer timed out");
			SPI_MasterTransferAbort(config->base, &data->fsl_handle);
			ret = -ETIMEDOUT;
			break;
		}

		if (data->transfer_status == kStatus_SPI_Error ||
		    data->transfer_status == kStatus_SPI_Timeout) {
			ret = -EIO;
			break;
		}

		LOG_DBG("irq: tx=0x%02x rx=0x%02x", tx_data, rx_data);

		if (spi_context_rx_buf_on(&data->ctx)) {
			*data->ctx.rx_buf = rx_data;
		}

		spi_context_update_tx(&data->ctx, 1, 1);
		spi_context_update_rx(&data->ctx, 1, 1);
	}

	spi_context_cs_control(&data->ctx, false);

out:
	spi_context_release(&data->ctx, ret);
	LOG_DBG("IRQ transceive completed (ret=%d)", ret);
	return ret;
}
#else

static int spi_lpc84x_transceive_poll(const struct device *dev, const struct spi_config *spi_cfg,
				      const struct spi_buf_set *tx_bufs,
				      const struct spi_buf_set *rx_bufs)
{
	const struct spi_lpc84x_config *config = dev->config;
	struct spi_lpc84x_data *data = dev->data;
	int ret;

	spi_context_lock(&data->ctx, false, NULL, NULL, spi_cfg);

	ret = spi_lpc84x_configure(dev, spi_cfg);
	if (ret) {
		goto out;
	}

	spi_context_buffers_setup(&data->ctx, tx_bufs, rx_bufs, 1);

	spi_context_cs_control(&data->ctx, true);

	while (spi_context_tx_buf_on(&data->ctx) || spi_context_rx_buf_on(&data->ctx)) {
		spi_transfer_t transfer = {0};
		uint8_t tx_data = 0;
		uint8_t rx_data = 0;

		if (spi_context_tx_buf_on(&data->ctx)) {
			tx_data = *data->ctx.tx_buf;
		}

		transfer.txData = &tx_data;
		transfer.rxData = &rx_data;
		transfer.dataSize = 1;
		transfer.configFlags = kSPI_EndOfTransfer;

		if (SPI_MasterTransferBlocking(config->base, &transfer) != kStatus_Success) {
			ret = -EIO;
			break;
		}

		LOG_DBG("irq: tx=0x%02x rx=0x%02x", tx_data, rx_data);

		if (spi_context_rx_buf_on(&data->ctx)) {
			*data->ctx.rx_buf = rx_data;
		}

		spi_context_update_tx(&data->ctx, 1, 1);
		spi_context_update_rx(&data->ctx, 1, 1);
	}

	spi_context_cs_control(&data->ctx, false);

out:
	spi_context_release(&data->ctx, ret);

	return ret;
}
#endif

static int spi_lpc84x_transceive_sync(const struct device *dev, const struct spi_config *config,
				      const struct spi_buf_set *tx_bufs,
				      const struct spi_buf_set *rx_bufs)
{
#ifdef CONFIG_SPI_LPC84X_INTERRUPT
	return spi_lpc84x_transceive_irq(dev, config, tx_bufs, rx_bufs);
#else
	return spi_lpc84x_transceive_poll(dev, config, tx_bufs, rx_bufs);
#endif
}

#ifdef CONFIG_SPI_ASYNC
static int spi_lpc84x_transceive_async(const struct device *dev, const struct spi_config *spi_cfg,
				       const struct spi_buf_set *tx_bufs,
				       const struct spi_buf_set *rx_bufs, spi_callback_t cb,
				       void *userdata)
{
	return -ENOTSUP;
}
#endif

static int spi_lpc84x_release(const struct device *dev, const struct spi_config *spi_cfg)
{
	struct spi_lpc84x_data *data = dev->data;

	spi_context_unlock_unconditionally(&data->ctx);
	return 0;
}

static int spi_lpc84x_init(const struct device *dev)
{
	const struct spi_lpc84x_config *config = dev->config;
	struct spi_lpc84x_data *data = dev->data;
	int ret;

	if (!device_is_ready(config->clock_dev)) {
		LOG_ERR("clock device is not ready");
		return -ENODEV;
	}

	ret = clock_control_on(config->clock_dev, config->clock_subsys);
	if (ret) {
		return ret;
	}

	ret = pinctrl_apply_state(config->pincfg, PINCTRL_STATE_DEFAULT);
	if (ret) {
		LOG_ERR("Failed to apply pinctrl state");
		return ret;
	}

	LOG_DBG("SWM SPI0_SCK assigned to: %d\n", (int)((SWM0->PINASSIGN.PINASSIGN3 >> 24) & 0xFF));

	ret = spi_context_cs_configure_all(&data->ctx);
	if (ret) {
		return ret;
	}

#ifdef CONFIG_SPI_LPC84X_INTERRUPT
	/* Initialise the completion semaphore (starts at 0, max 1) */
	k_sem_init(&data->transfer_sem, 0, 1);

	/*
	 * Wire up the hardware IRQ.  config->irq_config_func is generated by
	 * the SPI_LPC84X_IRQ_CONNECT() macro for each DT instance.
	 */
	config->irq_config_func(dev);

	LOG_INF("spi_lpc84x: interrupt mode enabled");
#else
	LOG_INF("spi_lpc84x: polling mode enabled");
#endif

	spi_context_unlock_unconditionally(&data->ctx);
	LOG_INF("spi initialized\n");

	return 0;
}

static DEVICE_API(spi, spi_lpc84x_driver_api) = {
	.transceive = spi_lpc84x_transceive_sync,
#ifdef CONFIG_SPI_ASYNC
	.transceive_async = spi_lpc84x_transceive_async,
#endif
	.release = spi_lpc84x_release,
};

/*
 * Interrupt-mode variant:
 *   Generates an irq_config_func_<n> that connects the Zephyr ISR wrapper
 *   to the hardware IRQ number/priority declared in the DT node, and adds
 *   irq_config_func to the config struct.
 */
#ifdef CONFIG_SPI_LPC84X_INTERRUPT

#define SPI_LPC84X_IRQ_CONNECT(n)                                                                  \
	static void irq_config_func_##n(const struct device *dev)                                  \
	{                                                                                          \
		IRQ_CONNECT(DT_INST_IRQN(n), DT_INST_IRQ(n, priority), spi_lpc84x_irq_handler,     \
			    DEVICE_DT_INST_GET(n), 0);                                             \
		irq_enable(DT_INST_IRQN(n));                                                       \
	}

#define SPI_LPC84X_IRQ_CONFIG(n) .irq_config_func = irq_config_func_##n,

#else /* polling */

#define SPI_LPC84X_IRQ_CONNECT(n) /* nothing */
#define SPI_LPC84X_IRQ_CONFIG(n)  /* nothing */

#endif /* CONFIG_SPI_LPC84X_INTERRUPT */

#define SPI_LPC84X_INIT(n)                                                                         \
	PINCTRL_DT_INST_DEFINE(n);                                                                 \
                                                                                                   \
	SPI_LPC84X_IRQ_CONNECT(n)                                                                  \
                                                                                                   \
	static const struct spi_lpc84x_config spi_lpc84x_config_##n = {                            \
		.base = (SPI_Type *)DT_INST_REG_ADDR(n),                                           \
		.clock_dev = DEVICE_DT_GET(DT_INST_CLOCKS_CTLR(n)),                                \
		.clock_subsys = (clock_control_subsys_t)DT_INST_CLOCKS_CELL(n, name),              \
		.pincfg = PINCTRL_DT_INST_DEV_CONFIG_GET(n),                                       \
		SPI_LPC84X_IRQ_CONFIG(n)};                                                         \
                                                                                                   \
	static struct spi_lpc84x_data spi_lpc84x_data_##n = {                                      \
		SPI_CONTEXT_INIT_LOCK(spi_lpc84x_data_##n, ctx),                                   \
		SPI_CONTEXT_INIT_SYNC(spi_lpc84x_data_##n, ctx),                                   \
		SPI_CONTEXT_CS_GPIOS_INITIALIZE(DT_DRV_INST(n), ctx)};                             \
                                                                                                   \
	DEVICE_DT_INST_DEFINE(n, spi_lpc84x_init, NULL, &spi_lpc84x_data_##n,                      \
			      &spi_lpc84x_config_##n, POST_KERNEL, CONFIG_SPI_INIT_PRIORITY,       \
			      &spi_lpc84x_driver_api);

DT_INST_FOREACH_STATUS_OKAY(SPI_LPC84X_INIT)
